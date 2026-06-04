# Aura mm 模块设计与实现报告

生成日期: 2026-06-04

## 1. 结论摘要

Aura 可以把 `mm` 设计成跨平台内存管理组件，但不能把所有平台都抽象成完全等价的 Android DMA 内存模型。更客观的结论是:

1. Android/Linux 是最接近目标模型的平台: dma-buf 以文件描述符作为跨进程、跨子系统、跨设备驱动共享的统一载体；Android 12+ GKI 使用 DMA-BUF heaps 替代 ION，并通过 heap name 区分 `system`、`system-uncached` 等分配来源。
2. Android 应同时支持两条路径:
   - 普通跨进程共享内存: `ASharedMemory_create` + `mmap`。
   - 异构/GPU/显示/DSP 共享: `AHardwareBuffer` 或 dma-buf heap fd。`AHardwareBuffer` 更适合作为 NDK 稳定入口；dma-buf heap 更接近“cached/uncached + fd + sync ioctl”的底层能力，但受设备、权限、SELinux、heap 暴露情况影响。
3. Windows 没有面向普通用户态的 dma-buf 等价标准库。普通共享内存使用 `CreateFileMapping` / `MapViewOfFile`；异构图形共享使用 Direct3D 共享资源、共享 heap/resource/fence handle；真正给任意 DMA 设备使用的 common buffer 属于内核驱动 KMDF/WDM 层能力。
4. macOS 没有通用用户态 dma-buf。普通 IPC 共享可用 `shm_open` / `mmap`；图形/视频/跨进程零拷贝共享的核心对象是 `IOSurface`，可与 Metal、OpenGL、历史 OpenCL 路径互操作。直接分配任意设备可 DMA 的物理内存不是常规用户态 API。
5. Linux 桌面/嵌入式可以使用 POSIX shm、`memfd_create`、`mmap` 做普通共享内存，使用 dma-buf heap / DRM GEM / GBM / V4L2 exporter 做异构共享。OpenGL/EGL、Vulkan、OpenCL 的外部内存导入依赖 Khronos 扩展和驱动支持。
6. 用户提到的 “PSS 内存” 需要校正术语: Android 官方文档里 PSS 是 Proportional Set Size，是内存占用统计指标，不是可申请的内存类型。如果需求是“进程共享内存”，Android NDK 对应 `ASharedMemory`；如果需求是“可被硬件共享的物理/设备内存”，对应 `AHardwareBuffer`、dma-buf heap 或厂商 gralloc/DSP 句柄。

因此，Aura `mm` 的优雅设计不应是“每个平台都模拟 dma-buf”，而应是:

- 对外统一为 move-only RAII `au::mm::Memory`。
- 对内按 backend 实现 `Host`、`SharedMemory`、`DmaBuf`、`AndroidHardwareBuffer`、`D3DSharedResource`、`IOSurface` 等策略。
- 所有关键能力都可查询: 是否可导出原生句柄、是否可 CPU map、是否支持 cached/uncached、是否支持 OpenCL/OpenGL/Metal/Direct3D/DSP 导入、flush/invalidate 是否是实操作。
- flush / validate / invalidate 明确语义，不把 CPU cache 同步、GPU fence 同步、跨 API ownership transfer 混在一个函数里。

## 2. 官方资料调研

### 2.1 Android

#### 2.1.1 普通共享内存: ASharedMemory

Android NDK `ASharedMemory_create(name, size)` 创建共享内存区域并返回 fd，调用方可以用 `mmap` 映射；fd 可通过 `ParcelFileDescriptor` 或 Unix domain socket 的 `SCM_RIGHTS` 传递给其他进程。它适合 CPU 侧 IPC、跨进程共享配置/队列/中间结果，但不等价于 GPU/DSP 可直接 DMA 的异构共享内存。

设计含义:

- `au::mm::MemoryKind::Shared` 在 Android 上可落到 `ASharedMemory`。
- 支持 `exportNativeHandle()` 返回 fd。
- 支持 `setProtection(ReadOnly)`，但要注意 Android 文档指出 protection 只能移除权限，已存在的 mapping 不受影响。

#### 2.1.2 异构共享内存: AHardwareBuffer

`AHardwareBuffer` 是 Android NDK 稳定的 native hardware buffer 抽象。它支持分配、引用计数、CPU lock/unlock、跨 Unix socket 传递，也可通过 Java `HardwareBuffer` 或 Binder/AIDL 路径传递。Usage flags 描述 CPU read/write、GPU sampled image、GPU framebuffer、GPU data buffer、protected content、sensor direct data 等用途。

关键边界:

- `AHardwareBuffer_lock` 是 CPU 访问入口，可能因为硬件渲染尚未完成或 cache 需要同步而阻塞。
- CPU 读写 usage 必须与分配时的 CPU usage 兼容。
- `AHardwareBuffer_unlock` 可返回 fence fd；这说明 CPU 访问结束和设备访问同步不应被 Aura 简化成普通 `void*` 生命周期。
- NDK usage flags 不是显式 cached/uncached 选择。Android 低层 dma-buf heap 可以暴露 `system` / `system-uncached`，但 `AHardwareBuffer` 通常由 gralloc/mapper 根据 usage、format、SoC 能力决定具体内存属性。

设计含义:

- Android 默认异构后端优先 `AHardwareBuffer`，因为这是应用/NDK 层更稳定的 API。
- 如果调用方明确要求 `CachePolicy::Uncached` 或需要裸 dma-buf fd，应提供 `DmaBufHeap` 后端作为可选能力，失败时返回 `Unsupported`，不静默退化成普通 heap。
- 对 `AHardwareBuffer`，`flush()` / `invalidate()` 应尽量通过 lock/unlock/fence 和平台 mapper 语义表达；不要承诺可以手动刷任意 cache line。

#### 2.1.3 DMA-BUF heaps 与 cached/uncached

Android Open Source Project 文档说明，Android 12 的 GKI 2.0 使用 DMA-BUF heaps 替代 ION。用户态从 `/dev/dma_heap/<heap_name>` 分配 dma-buf fd，`libdmabufheap` 用 `BufferAllocator::Alloc(heapName, size)` 抽象 heap name，并可在老设备上回退 ION。AOSP 文档给出的映射是:

- cached system heap: `allocator->Alloc("system", size)`
- uncached system heap: `allocator->Alloc("system-uncached", size)`

Linux kernel 文档中，dma-buf heaps 是用户态分配 dma-buf object 的方式；`system` heap 是虚拟连续、cacheable；`cma` heap 是物理连续、cacheable，并依赖 CMA 区域存在。Android 的 `system-uncached` 属于 Android 分支/设备能力，不应当假设所有 Linux 发行版都有。

设计含义:

- `MemoryDesc::cachePolicy` 只能是 request，分配后必须返回 `MemoryInfo::actualCachePolicy`。
- `MemoryDesc::contiguous` 也只能是 request。Linux `cma` 或设备专用 heap 可能满足物理连续；普通 `system` dma-buf 通常不保证物理连续。
- Aura 必须提供 heap 枚举/查询: `listHeaps()`、`isHeapAvailable("system-uncached")`。

#### 2.1.4 DMA-BUF CPU 同步

Linux dma-buf 文档把 CPU 访问同步建模为 begin/end CPU access。用户态 mmap 后需要用 `DMA_BUF_IOCTL_SYNC` 包住 CPU 访问；exporter 的 `begin_cpu_access` 可为 CPU 访问做准备，`end_cpu_access` 可 flush cache 或撤销准备工作。

AOSP `libdmabufheap` 的 `BufferAllocator::CpuSyncStart` / `CpuSyncEnd` 正是对 `DMA_BUF_IOCTL_SYNC` 的封装，并且在旧 ION 路径上有 legacy sync 回退。

设计含义:

- Aura 不应只提供裸 `void* map()`，而应提供 RAII `MappedRange`:
  - 构造时做 begin/invalidate。
  - 析构时按访问模式做 end/flush。
  - 支持显式 `flush(offset, size)` 和 `invalidate(offset, size)`，用于长生命周期 mapping。
- `validate()` 建议命名为 `invalidate()` 或 `validateForCpuRead()`，避免与“校验正确性”混淆。为了贴近用户术语，可保留 `validate()` 作为 `invalidate()` 的别名，但文档中明确它表示 device-to-CPU 可见性同步。

### 2.2 Linux

Linux 上应区分三类内存:

1. 普通进程内存: `malloc`、`new`、`mmap` 匿名页。适合 CPU 算子和普通 buffer。
2. 普通 IPC 共享内存: POSIX `shm_open` / `mmap` 或 Linux `memfd_create`。适合跨进程 CPU 共享，不保证可被 GPU/DSP 直接导入。
3. 设备共享内存: dma-buf fd。来源可以是 dma-buf heap、DRM/GBM、V4L2、camera、codec、display compositor 等 exporter。它是 Linux 异构共享的事实标准。

OpenGL/EGL 路径通常依赖 `EGL_EXT_image_dma_buf_import` 和 `EGL_EXT_image_dma_buf_import_modifiers`。modifier 非常关键，因为同一个 DRM fourcc 在不同 tiling/compression layout 下不是同一内存布局。GStreamer 的 DMA-BUF 设计文档也强调 DRM format 与 DRM modifier 必须一起协商，否则下游可能按 linear layout 错误渲染。

OpenCL 路径依赖 Khronos `cl_khr_external_memory` 以及具体 handle type 扩展，例如 `cl_khr_external_memory_dma_buf`、`opaque_fd`、`win32`、`android_hardware_buffer`。规范提供了 acquire/release external memory object 的 ownership handoff；这应映射到 Aura 的 device access scope，而不是普通 CPU cache flush。

设计含义:

- Linux `DmaBufMemory` 必须保存:
  - fd
  - size
  - plane count
  - per-plane fd/offset/stride
  - DRM fourcc
  - DRM modifier
  - optional fence fd
- `MemoryInfo` 必须暴露这些布局元数据，否则 OpenGL/EGL、Vulkan、GStreamer 等下游无法可靠零拷贝导入。
- 如果 format/modifier 未知，Aura 应禁止声称“图像零拷贝可导入”，最多标为 raw byte buffer。

### 2.3 Windows

#### 2.3.1 普通共享内存

Windows 的普通跨进程共享内存是 file mapping object。`CreateFileMapping(INVALID_HANDLE_VALUE, ...)` 可以创建由系统 paging file 支撑的 named shared memory，`MapViewOfFile` 将其映射到进程地址空间，其他进程用 `OpenFileMapping` 打开同名对象。所有 handle 关闭后，系统释放对应 paging file section。

设计含义:

- `MemoryKind::Shared` 在 Windows 上落到 section/file mapping。
- 原生句柄类型是 `HANDLE`，需要明确 `CloseHandle` 所有权。
- 跨进程传递需要 name、inheritance、DuplicateHandle 或安全描述符；不要直接暴露 `windows.h` 到公共头。

#### 2.3.2 Direct3D 共享资源

Direct3D 11 可用 `D3D11_RESOURCE_MISC_SHARED`、`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`、`D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` 创建共享资源。Microsoft 文档建议 Direct3D 11.1 起使用 `IDXGIResource1::CreateSharedHandle` 和 NT handle，而不是旧的 `IDXGIResource::GetSharedHandle`。Direct3D 12 可用 `ID3D12Device::CreateSharedHandle` 为 heap、resource 或 fence 创建共享 handle，另一个 device/process 通过 OpenSharedHandle 打开。

设计含义:

- Windows 异构共享应作为 `D3DSharedResourceMemory` 后端，不要抽象成 generic physical DMA。
- OpenCL 互操作要走 `cl_khr_d3d11_sharing` 或 `cl_khr_external_memory_win32` 等扩展，取决于驱动支持。
- GPU 同步应使用 keyed mutex、D3D fence 或 shared fence；`flush()` 不应伪装成能解决 GPU queue ordering。

#### 2.3.3 DMA common buffer

KMDF `WdfCommonBufferCreate` 能创建 driver 和 DMA device 同时访问的 common buffer，并提供 driver virtual address 与 device logical address。但这是内核驱动 API，不是普通用户态库能直接调用的跨平台分配器。缓存策略也由 OS 根据处理器架构、总线和 ACPI_CCA 等信息决定。

设计含义:

- Aura 用户态 `mm` 不应承诺 Windows 下能为任意 DSP/NPU/PCIe 设备分配 DMA common buffer。
- 如未来 Aura 提供厂商设备插件，应通过设备 SDK/driver 导出的 handle 导入到 `ExternalMemory`，而不是在核心库里假设可直接申请。

### 2.4 macOS

#### 2.4.1 普通共享内存

macOS 继承 BSD/POSIX 能力，可用 `shm_open`、`shm_unlink`、`mmap` 创建共享内存。Apple 文档也提醒共享内存适合 raw data，例如 pixels/audio，但控制结构最好通过更常规 IPC 管理，以降低数据破坏和安全风险。

设计含义:

- `MemoryKind::Shared` 在 macOS 上落到 POSIX shm 或匿名 `mmap` + fd/descriptor 管理。
- 公共 API 不暴露 Mach 或 Cocoa 类型；实现层用 Objective-C++ / CoreFoundation RAII 包装。

#### 2.4.2 IOSurface

`IOSurface` 是 macOS/iOS 跨进程、跨图形/视频 API 共享 image/framebuffer 数据的核心机制。Apple 文档描述它用于共享硬件加速 buffer data，历史 OpenCL 文档也说明 IOSurface 可跨 API、地址空间和进程共享，CPU 访问时需要 `IOSurfaceLock` / `IOSurfaceUnlock`，系统据此保证设备获取最新数据。Metal 可从 IOSurface 创建 texture，OpenGL 也有 IOSurface 绑定路径；OpenCL 已被 macOS 10.14 标记 deprecated，新的高性能计算应优先 Metal。

设计含义:

- macOS 异构图像内存后端应是 `IOSurfaceMemory`，不是 dma-buf。
- `flush()/invalidate()` 映射到 `IOSurfaceLock/Unlock`、Metal command buffer ordering、必要的 blit/synchronize 操作。
- macOS 不应承诺 DSP/ANE 任意共享，除非通过 CoreVideo/Metal/ML 框架提供可导入对象。

## 3. 高质量开源项目启发

### 3.1 AOSP libdmabufheap

`libdmabufheap` 的关键设计启发:

- 用 heap name 代替旧 ION heap mask/flag。
- allocator 初始化时管理 `/dev/dma_heap/<heap_name>` fd。
- 优先 dma-buf heap，必要时回退 legacy ION。
- `AllocSystem(cpu_access_needed, ...)` 会在 CPU 不需要访问且 `system-uncached` 存在时选择 uncached heap，否则回到 `system`。
- 封装 `DMA_BUF_IOCTL_SYNC` 为 `CpuSyncStart` / `CpuSyncEnd`。

Aura 应吸收:

- `HeapName` 和 `HeapInfo` 是跨 Android/Linux 的一等概念。
- cached/uncached 是分配选择结果，不只是 flag。
- CPU sync 是对象能力，不是全局工具函数。

### 3.2 GStreamer DMA-BUF

GStreamer 把完整 video frame 映射为 `GstBuffer`，每个 fd 由 `GstMemory` mini-object 持有，并通过 caps negotiation 表达 `memory:DMABuf`、DRM fourcc、modifier。它的启发是: 异构内存不仅是“一段 bytes”，还必须携带解释 bytes 的元数据。

Aura 应吸收:

- `Memory` 只表示存储；`ImageMemoryView` / `PlaneDesc` 表示图像布局。
- FourCC、modifier、stride、offset、plane count 必须随句柄一起导出/导入。
- linear 与 non-linear image 严格区分。

### 3.3 Chromium NativePixmap / Ozone

Chromium 的 Linux native pixmap 设计体现了一个实用边界: 某些 dma-buf 只需要用于 GPU/compositor import，不需要在 client 侧 mmap。设计中存在 opaque pixmap，`Map()` 明确不可用。

Aura 应吸收:

- `Memory` 不一定可 CPU map。
- `Capability::CpuMappable` 必须可查询。
- 对不可 map 的对象，`map()` 应返回 `Unsupported`，而不是尝试隐藏 copy。

### 3.4 Vulkan Memory Allocator / D3D12 Memory Allocator

VMA 和 D3D12MA 的共同启发:

- allocator 是长期对象，allocation 是轻量对象。
- 支持 custom pool、linear allocator、统计、budget、debug name、JSON dump。
- 对 non-coherent memory 提供 flush/invalidate。
- 公共边界接近 C API 或 handle API，内部用 C++ 实现，不依赖异常和 RTTI。

Aura 应吸收:

- `Allocator` 与 `Memory` 分离。
- `Pool` 是可选性能层，不是第一版必需 API。
- 统计和 debug dump 应从第一版留出接口，否则后续很难排查泄漏和碎片。

## 4. Aura mm 目标与非目标

### 4.1 目标

1. RAII 管理内存申请、释放、map/unmap、native handle 所有权，避免泄漏。
2. 统一普通内存、跨进程共享内存、异构共享内存的描述、查询、导入、导出。
3. 支持 Android/Linux dma-buf cached/uncached heap 选择、CPU sync、fd 导入导出。
4. 支持 Android `AHardwareBuffer` 作为稳定 NDK 后端。
5. 支持 Windows file mapping 和 Direct3D shared resource handle。
6. 支持 macOS POSIX shm 和 IOSurface。
7. 公共头文件保持 C++11 兼容，不暴露平台重型头，不让异常跨 Aura ABI 边界。
8. 为 OpenCL/OpenGL/GPU/DSP 互操作提供原生 handle 与布局元数据，而不是把每个后端直接耦合进核心对象。

### 4.2 非目标

1. 不承诺所有平台都有 uncached memory。
2. 不承诺所有平台都能分配物理连续内存。
3. 不承诺用户态能直接分配任意 DMA device 可访问的物理内存。
4. 不把 OpenCL/OpenGL/Metal/Direct3D 的具体资源创建全部塞进 `mm` 核心；核心只负责内存对象、句柄、布局、同步，API 互操作放在 `au::gpu` 或可选 adapter。
5. 不在热路径使用虚函数、异常、动态分配。分配本身不是热路径，但 map/unmap/sync 需要保持清晰、可控、可审计。

## 5. 模块架构

建议新增目录:

```text
inc/mm/xmemory.h
inc/mm/xmemory_types.h
src/mm/xmemory.cpp
src/mm/detail/xmemory_backend.h
src/mm/detail/xmemory_host.cpp
src/mm/detail/xmemory_posix.cpp
src/mm/detail/xmemory_android.cpp
src/mm/detail/xmemory_linux_dmabuf.cpp
src/mm/detail/xmemory_windows.cpp
src/mm/detail/xmemory_macos.mm
test/test_xmemory.cpp
```

命名空间:

```cpp
namespace au {
namespace mm {
// public C++11 facade
}
}
```

内部 backend 可以用 C++17，放在 `au::mm::detail`。

### 5.1 分层

```text
au::mm public facade
  - MemoryDesc / MemoryInfo / Memory / MappedRange / NativeHandle
  - C++11, move-only RAII, no platform headers

C ABI narrow waist
  - au_mm_memory_t opaque handle
  - au_mm_create / au_mm_destroy / au_mm_map / au_mm_unmap / au_mm_export_handle
  - noexcept boundary, error code return

Backend layer
  - HostBackend
  - PosixSharedBackend
  - LinuxDmaBufBackend
  - AndroidHardwareBufferBackend
  - WindowsSectionBackend
  - WindowsD3DSharedBackend
  - MacIOSurfaceBackend

Interop adapters
  - au::gpu::opencl::importMemory(...)
  - au::gpu::opengl::importImage(...)
  - future: vulkan / metal / dsp vendor adapters
```

选择 C ABI narrow waist 的原因: `mm` 未来很可能要被多个 `.so`、不同 NDK 小版本、不同调用方长期依赖。内存对象天然携带平台资源和所有权，适合用 opaque handle 稳定 ABI，再提供 C++11 RAII wrapper。

文件名仍建议使用 `xmemory.h` / `xmemory_types.h`，不强制 `_api.h` 后缀；但内部结构上应按 C ABI 窄腰设计。

## 6. 公开 API 草案

### 6.1 类型定义

```cpp
namespace au {
namespace mm {

enum class MemoryKind {
    Host,
    Shared,
    DeviceShared,
    External
};

enum class Backend {
    Auto,
    Host,
    PosixShm,
    AndroidSharedMemory,
    AndroidHardwareBuffer,
    LinuxDmaBufHeap,
    WindowsSection,
    WindowsD3D11,
    WindowsD3D12,
    MacIOSurface
};

enum class CachePolicy {
    Default,
    Cached,
    Uncached,
    WriteCombined
};

enum MemoryUsage : uint64_t {
    MEMORY_USAGE_CPU_READ        = 1ull << 0,
    MEMORY_USAGE_CPU_WRITE       = 1ull << 1,
    MEMORY_USAGE_GPU_SAMPLED     = 1ull << 2,
    MEMORY_USAGE_GPU_STORAGE     = 1ull << 3,
    MEMORY_USAGE_GPU_FRAMEBUFFER = 1ull << 4,
    MEMORY_USAGE_OPENCL          = 1ull << 5,
    MEMORY_USAGE_OPENGL          = 1ull << 6,
    MEMORY_USAGE_DSP             = 1ull << 7,
    MEMORY_USAGE_CROSS_PROCESS   = 1ull << 8,
    MEMORY_USAGE_PROTECTED       = 1ull << 9
};

struct MemoryDesc {
    size_t      size;
    size_t      alignment;
    MemoryKind  kind;
    Backend     backend;
    uint64_t    usage;
    CachePolicy cachePolicy;
    bool        preferPhysicalContiguous;
    const char* debugName;
};

struct PlaneDesc {
    int      fdIndex;
    uint64_t offset;
    uint32_t rowStride;
    uint32_t pixelStride;
};

struct ImageLayout {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t drmFourcc;
    uint64_t drmModifier;
    uint32_t planeCount;
    PlaneDesc planes[4];
};

}
}
```

说明:

- `MemoryUsage` 用 bitmask，而不是 enum class，方便 C ABI 和 C++11。
- `debugName` 只借用，不跨异步保存；实现层复制时自行处理。
- `ImageLayout` 固定最多 4 planes，覆盖常见 DRM/EGL modifier 场景。

### 6.2 RAII Memory

```cpp
namespace au {
namespace mm {

class Memory {
public:
    Memory() noexcept;
    ~Memory() noexcept;

    Memory(Memory&& other) noexcept;
    Memory& operator=(Memory&& other) noexcept;

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    bool valid() const noexcept;
    size_t size() const noexcept;
    Backend backend() const noexcept;
    CachePolicy actualCachePolicy() const noexcept;

    bool isCpuMappable() const noexcept;
    bool canExportNativeHandle() const noexcept;
    bool canImportToOpenCL() const noexcept;
    bool canImportToOpenGL() const noexcept;

    int flush(size_t offset, size_t size) noexcept;
    int invalidate(size_t offset, size_t size) noexcept;
    int validate(size_t offset, size_t size) noexcept; // alias of invalidate

private:
    au_mm_memory_t* mHandle;
};

Memory allocate(const MemoryDesc& desc, int* err = nullptr) noexcept;

}
}
```

### 6.3 RAII CPU mapping

```cpp
namespace au {
namespace mm {

enum class MapAccess {
    Read,
    Write,
    ReadWrite
};

class MappedRange {
public:
    MappedRange() noexcept;
    ~MappedRange() noexcept;

    MappedRange(MappedRange&& other) noexcept;
    MappedRange& operator=(MappedRange&& other) noexcept;

    MappedRange(const MappedRange&) = delete;
    MappedRange& operator=(const MappedRange&) = delete;

    void* data() noexcept;
    const void* data() const noexcept;
    size_t size() const noexcept;

    int flush(size_t offset, size_t size) noexcept;
    int invalidate(size_t offset, size_t size) noexcept;

private:
    au_mm_mapping_t* mMapping;
};

MappedRange map(Memory& memory, MapAccess access, size_t offset, size_t size, int* err = nullptr) noexcept;

}
}
```

同步策略:

- map read: begin CPU access for read；必要时 invalidate。
- map write: begin CPU access for write；析构时 end CPU access；必要时 flush。
- map read/write: 两者都做。
- 对 host memory，flush/invalidate 是 no-op 但返回 OK。
- 对 dma-buf，映射 `DMA_BUF_IOCTL_SYNC START/END`。
- 对 AHardwareBuffer，映射 lock/unlock + fence。
- 对 IOSurface，映射 lock/unlock。
- 对 D3D resource，CPU mapping 只在 staging/readback/upload heap 或可 map resource 上支持；默认 shared GPU resource 不可 map。

### 6.4 Native handle

公共头不包含平台头，使用 tag + integral/pointer-sized storage:

```cpp
namespace au {
namespace mm {

enum class NativeHandleType {
    None,
    PosixFd,
    DmaBufFd,
    AndroidHardwareBuffer,
    Win32Handle,
    IOSurfaceId,
    IOSurfaceRef,
    D3D11Resource,
    D3D12Resource,
    D3D12Heap
};

enum class HandleOwnership {
    Borrowed,
    Duplicated,
    Transferred
};

struct NativeHandle {
    NativeHandleType type;
    HandleOwnership ownership;
    intptr_t value0;
    intptr_t value1;
};

int exportNativeHandle(const Memory& memory, NativeHandleType type, NativeHandle* out) noexcept;
Memory importNativeHandle(const NativeHandle& handle, const MemoryDesc& desc, int* err = nullptr) noexcept;

}
}
```

规则:

- fd 导出默认 `Duplicated`，调用方负责 close；如是 borrow 必须明确。
- Windows HANDLE 导出默认 duplicate，调用方 `CloseHandle`。
- `AHardwareBuffer*` 导出时 acquire 引用，调用方 release。
- `IOSurfaceRef` 导出时 retain，调用方 release。
- 所有 import 都必须明确是否接管所有权。

## 7. 平台后端映射

| Aura backend | Android | Linux | Windows | macOS |
|---|---|---|---|---|
| Host | malloc/aligned allocation | malloc/aligned allocation | `_aligned_malloc` | posix/aligned allocation |
| Shared | `ASharedMemory` | `shm_open` / `memfd_create` | `CreateFileMapping` | `shm_open` / `mmap` |
| DeviceShared image | `AHardwareBuffer` | dma-buf from GBM/DRM/V4L2 or heap | D3D11/D3D12 shared resource | `IOSurface` |
| DeviceShared raw buffer | dma-buf heap, AHB BLOB | dma-buf heap | D3D12 shared heap/resource, vendor SDK | limited; usually Metal buffer not cross-process like IOSurface texture |
| cached/uncached | dma-buf heap name if available; AHB implicit | heap dependent; `system` cacheable, `cma` cacheable, uncached not portable | OS/driver dependent | OS/driver dependent |
| CPU sync | AHB lock/unlock, dma-buf ioctl | dma-buf ioctl | map/unmap; D3D fences for GPU | IOSurface lock/unlock; Metal sync |
| OpenCL import | AHB/dma-buf extensions if driver supports | external memory dma-buf if driver supports | D3D sharing / win32 external memory | deprecated OpenCL IOSurface path |
| OpenGL import | EGL native buffer / EGLImage | EGL dma-buf import | ANGLE/D3D share handle or WGL/DX interop | CGL IOSurface |
| DSP import | vendor HAL/SDK usually requires AHB/dma-buf | vendor driver SDK | vendor driver SDK | not generic |

## 8. 关键设计细节

### 8.1 CachePolicy 是请求，不是保证

`MemoryDesc::cachePolicy = Uncached` 的行为:

1. Android/Linux dma-buf heap: 优先选择 `system-uncached` 或用户指定 uncached heap。
2. 如果 heap 不存在:
   - `Strict` 模式返回 `Unsupported`。
   - `BestEffort` 模式可退化到 `Cached`，但 `MemoryInfo::actualCachePolicy` 必须写明。
3. Windows/macOS: 除非具体后端支持，否则返回 `Unsupported` 或退化到 `Default`。

建议给 `MemoryDesc` 增加:

```cpp
enum class Requirement {
    BestEffort,
    Strict
};

Requirement cacheRequirement;
Requirement contiguousRequirement;
```

### 8.2 flush / invalidate / validate 语义

推荐定义:

- `flush(offset, size)`: CPU 写入后，使 device/other agent 可见。
- `invalidate(offset, size)`: device/other agent 写入后，使 CPU 后续读取看到最新内容。
- `validate(offset, size)`: `invalidate` 的兼容别名，文档中不推荐新代码使用。
- `beginCpuAccess(access)` / `endCpuAccess(access)`: 后端需要时包住一段 CPU 访问；`MappedRange` 自动调用。

不要把这些函数用于 GPU queue 同步。GPU queue ordering 应通过 fence/semaphore/sync object:

- Linux/Android: sync fd / dma fence / EGLSync / OpenCL semaphore。
- Windows: D3D keyed mutex / fence / shared fence。
- macOS: Metal command buffer completion / shared event / IOSurface lock 只处理 CPU access visibility。

### 8.3 NativeHandle 所有权

所有权是 `mm` 模块最容易出 bug 的部分。必须强制编码:

- fd/HANDLE/CFType/AHB 都禁止裸返回不说明所有权。
- `exportNativeHandle(..., Duplicated)` 是默认安全策略。
- `importNativeHandle(..., Transferred)` 成功后由 Aura 关闭；失败时调用方仍拥有，除非文档另行明确。
- `Borrowed` 只允许立即导入并在文档要求调用方保证生命周期。

### 8.4 图像布局不属于“附加信息”，而是 correctness contract

DMA-BUF image 共享时，如果只传 fd 和 size，不传 fourcc/modifier/stride/offset，零拷贝路径是不可靠的。Aura 应把 `ImageLayout` 放进核心类型，而不是交给调用方私下约定。

### 8.5 与 au::gpu 的边界

`au::mm` 不直接包含 OpenCL/OpenGL/Metal/D3D 头。推荐:

```cpp
// au::mm
Memory mem = allocate(desc);
NativeHandle h = exportNativeHandle(mem, NativeHandleType::DmaBufFd);
ImageLayout layout = queryImageLayout(mem);

// au::gpu adapter
au::gpu::OpenGLImage image = au::gpu::importDmaBufImage(glContext, h, layout);
au::gpu::OpenCLMemory clMem = au::gpu::importExternalMemory(clContext, mem);
```

这样 `mm` 保持基础组件定位，`gpu` 负责 API-specific import。

## 9. 实现路线

### Phase 0: 类型与基础 RAII

- 新增 `inc/mm/xmemory_types.h`、`inc/mm/xmemory.h`。
- 新增 opaque C handle 和 C++11 move-only wrapper。
- 实现 Host backend:
  - aligned allocation
  - size/alignment query
  - no-op flush/invalidate
  - test move/destroy/map edge cases

验收:

- Public headers C++11 编译通过。
- 无异常跨 API。
- 移动后源对象 invalid。
- double destroy 安全。

### Phase 1: 普通共享内存

- POSIX/macOS/Linux: `shm_open` or `memfd_create` + `ftruncate` + `mmap`。
- Android: `ASharedMemory_create`。
- Windows: `CreateFileMapping` + `MapViewOfFile`。
- 实现 `exportNativeHandle` / `importNativeHandle`。

验收:

- 同进程 fd/HANDLE roundtrip。
- fork 或子进程写入，父进程读回。
- read-only protection 测试。

### Phase 2: Android/Linux dma-buf

- Linux: 打开 `/dev/dma_heap/<heap>`，`DMA_HEAP_IOCTL_ALLOC`。
- Android: 优先集成 `libdmabufheap`；如不引入依赖，内部实现最小 heap open/ioctl 路径。
- 实现 heap list/query。
- 实现 `DMA_BUF_IOCTL_SYNC` begin/end。
- 支持 `system` / `system-uncached` / `cma` / custom heap name。

验收:

- heap 不存在返回 `Unsupported`。
- strict uncached 在无 heap 时失败。
- fd close-on-exec。
- map read/write 用 RAII 自动 sync。
- llseek 或 fallback 获取 size。

### Phase 3: Android AHardwareBuffer

- 使用 NDK `AHardwareBuffer_allocate` / `release`。
- usage flags 从 `MemoryUsage` 映射。
- CPU map 使用 `AHardwareBuffer_lock` / `unlock`。
- 支持 Unix socket send/recv 或 native handle export/import。
- BLOB raw buffer 与 image buffer 分开处理。

验收:

- API level gating。
- unsupported format/usage 返回 `Unsupported`。
- lock 多线程读行为按文档限制测试。
- unlock fence 生命周期测试。

### Phase 4: Windows / macOS 异构后端

Windows:

- `WindowsSectionBackend` 做普通 shared memory。
- `WindowsD3D12Backend` 只在可选编译开关启用。
- shared HANDLE 统一 duplicate/close。
- D3D fence/keyed mutex 放到 sync object。

macOS:

- `.mm` 实现 `IOSurfaceBackend`。
- `IOSurfaceLock/Unlock` 绑定 `MappedRange`。
- Metal/OpenGL import adapter 放 `au::gpu`。

验收:

- 平台头不进入 public header。
- 没有 Objective-C 类型泄漏到 `inc/`。
- 编译开关关闭时核心库可构建。

### Phase 5: Pool、统计、调试

- `Allocator` 长生命周期对象。
- `PoolDesc` 支持固定块、linear/ring、debug name。
- `MemoryStats` 支持按 backend/heap 统计 total/used/free/allocation count。
- JSON dump 可选。
- debug fill、guard page、leak report 可选。

## 10. 测试策略

### 10.1 单元测试

- `HostMemoryBasic`: size=0、alignment 非 2 次幂、超大 size、move、destroy。
- `MappedRange`: read/write/readwrite、offset+size 越界、重复 flush、析构 unmap。
- `NativeHandleOwnership`: duplicated fd/HANDLE 被正确关闭，borrowed 不关闭。
- `SharedMemoryRoundtrip`: export/import 后写读一致。
- `UnsupportedCapabilities`: 请求当前平台不支持后端时返回错误码，不崩溃。

### 10.2 平台测试

Android:

- `ASharedMemory` mmap + fd pass。
- `AHardwareBuffer` BLOB lock/unlock。
- dma-buf heap `system` 分配与 mmap。
- 如设备有 `system-uncached`，验证 strict uncached。

Linux:

- `memfd_create` / POSIX shm。
- dma-buf heap 分配。
- `DMA_BUF_IOCTL_SYNC` start/end。
- EGL dma-buf import 可放集成测试，不默认启用。

Windows:

- named shared memory two-process test。
- D3D shared handle test 放可选 GPU test。

macOS:

- POSIX shm。
- IOSurface create/lock/unlock。
- Metal texture from IOSurface 放可选 GPU test。

### 10.3 静态与运行时检查

- C++11 public header compile job。
- C++17 implementation compile job。
- ASAN: host/shared memory 生命周期。
- TSAN: handle refcount 与 map/unmap 状态。
- Warning build: `-Wall -Wextra -Werror` 或 MSVC `/W4`。

## 11. 风险与决策记录

### 11.1 最大风险: 抽象过度承诺

如果 API 叫 `allocateDmaMemory()` 并在所有平台出现，会误导调用方以为 Windows/macOS 也有通用 dma-buf。推荐 API 用 `MemoryKind::DeviceShared` + `Backend::Auto`，并通过 `MemoryInfo` 说明实际 backend。

### 11.2 第二风险: cache sync 与 GPU sync 混淆

`flush/invalidate` 只解决 CPU cache visibility 或平台等价操作；GPU/OpenCL/OpenGL ownership 和 queue ordering 需要 fence/semaphore/acquire/release。报告和 API 注释必须反复强调这一点。

### 11.3 第三风险: fd/HANDLE 泄漏

`NativeHandle` 必须带所有权。测试必须覆盖 export/import 失败路径。所有 fd 创建时尽量使用 close-on-exec。

### 11.4 第四风险: 图像 layout 信息不足

DMA-BUF image 共享缺少 DRM modifier 时会出现“能导入但画面错误”的隐性 bug。Aura 必须把 layout 作为强类型返回。

### 11.5 第五风险: Android 设备碎片化

不同 SoC、Android 版本、vendor heap、SELinux policy 差异很大。Aura 应提供运行时 capability query 和清晰错误码，而不是靠编译期宏判断。

## 12. 推荐第一版 API 范围

第一版不要一次覆盖所有 GPU API。推荐最小可用范围:

1. `HostMemory`
2. `SharedMemory`
3. Android/Linux `DmaBufMemory`
4. Android `AHardwareBufferMemory`
5. `MappedRange` RAII + flush/invalidate
6. native handle import/export
7. heap/capability query
8. stats/debug name

Windows D3D 与 macOS IOSurface 可以在架构预留后作为第二批实现。这样能先把最复杂、最符合用户异构共享需求的 Android/Linux 路径打牢。

## 13. 建议的错误码

优先复用 `au::err`，新增或映射:

```text
OK
InvalidArgument
Unsupported
OutOfMemory
PermissionDenied
PlatformError
HandleInvalid
HandleOwnershipError
MapFailed
SyncFailed
NotMappable
Busy
```

所有平台 errno/HRESULT/kern_return_t/NDK status 都转换成 Aura 错误码，同时保留 `lastPlatformError()` 供诊断。

## 14. 官方与开源资料

官方资料:

- Android NDK `ASharedMemory`: https://developer.android.com/ndk/reference/group/memory
- Android NDK `AHardwareBuffer`: https://developer.android.google.cn/ndk/reference/group/a-hardware-buffer
- Android AOSP ION 到 DMA-BUF heaps 迁移: https://source.android.com/docs/core/architecture/kernel/dma-buf-heaps
- Android PSS / RSS / USS 说明: https://developer.android.com/topic/performance/memory-management
- Android `dumpsys meminfo`: https://developer.android.com/tools/dumpsys
- Linux dma-buf heaps: https://docs.kernel.org/6.15/userspace-api/dma-buf-heaps.html
- Linux dma-buf sharing and synchronization: https://www.kernel.org/doc/html/v6.6/driver-api/dma-buf.html
- Microsoft named shared memory: https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory
- Microsoft Direct3D 11 shared resource flags: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag
- Microsoft `IDXGIResource::GetSharedHandle`: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
- Microsoft Direct3D 12 shared handle: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
- Microsoft KMDF common buffer: https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfcommonbuffer/nf-wdfcommonbuffer-wdfcommonbuffercreate
- Apple IOSurface: https://developer.apple.com/documentation/iosurface
- Apple Metal texture from IOSurface: https://developer.apple.com/documentation/metal/mtldevice/maketexture%28descriptor%3Aiosurface%3Aplane%3A%29
- Apple shared memory overview: https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/OSX_Technology_Overview/SystemTechnology/SystemTechnology.html
- Apple historical OpenCL + IOSurface guide: https://developer.apple.com/library/archive/documentation/Performance/Conceptual/OpenCL_MacProgGuide/SynchronizingIOSurfacesAcrossProcessors/SynchronizingIOSurfacesAcrossProcessors.html
- Khronos OpenCL external memory: https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/cl_khr_external_memory.html
- Khronos EGL dma-buf modifiers: https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt

开源项目:

- AOSP `libdmabufheap` `BufferAllocator`: https://android.googlesource.com/platform/system/memory/libdmabufheap/+/refs/heads/android16-qpr2-release/BufferAllocator.cpp
- GStreamer DMA-BUF design: https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html
- Chromium Linux NativePixmap dmabuf: https://chromium.googlesource.com/chromium/src/+/0d58af6fc31ca16210c8a0c475fce91fff516ab2/ui/gfx/linux/client_native_pixmap_factory_dmabuf.cc
- Vulkan Memory Allocator: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- D3D12 Memory Allocator: https://gpuopen.com/d3d12-memory-allocator/

## 15. 最终建议

Aura `mm` 应采用“能力驱动的统一内存对象”:

- 用 `MemoryDesc` 表达期望。
- 用 `MemoryInfo` 告知实际能力。
- 用 `Memory` / `MappedRange` 管生命周期。
- 用 `NativeHandle` 管跨进程/跨 API 共享。
- 用 backend adapter 承接平台差异。

这样既能在 Android/Linux 上充分利用 dma-buf、cached/uncached heap、flush/invalidate，又不会在 Windows/macOS 上制造虚假的可移植承诺。第一版优先打通 Host、Shared、Android/Linux dma-buf、AHardwareBuffer，后续再扩展 D3D shared resource 与 IOSurface，会是风险最低、结构最稳的演进路线。
