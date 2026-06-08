# Aura mm 模块 Android/Linux 设计与实现报告

生成日期: 2026-06-08

## 1. 当前范围

本版 `mm` 暂且只考虑 Android 和 Linux 平台。Windows/macOS 不进入第一阶段设计与实现，只保留公共 API 的可编译 fallback，避免破坏 Aura 在开发机上的构建。

第一阶段目标:

1. 提供 `au::mm` 模块，负责内存申请、销毁、共享、查询、CPU 映射和同步。
2. Public header 位于 `inc/mm/`，保持 C++11 兼容，不暴露 Android/Linux 平台头。
3. 对外使用 move-only RAII 对象管理生命周期，避免 fd、mapping、AHardwareBuffer、dma-buf 泄漏。
4. Android 支持:
   - `ASharedMemory` 普通跨进程共享内存。
   - `AHardwareBuffer` 异构共享内存入口。
   - 可选 dma-buf heap fd 路径，用于 cached/uncached heap 与 DSP/vendor 场景。
5. Linux 支持:
   - POSIX/memfd 共享内存。
   - dma-buf heap 分配、fd 导入导出、CPU sync。
6. 当前仓库实现先落地 API、Host、`MemoryInfo` 查询、POSIX/memfd shared memory、Linux/Android dma-buf heap 框架和 Android AHardwareBuffer 条件编译路径；实际 Android 设备能力仍需在设备 CI 或真机上验证。

## 2. 关键结论

### 2.1 PSS 不是可申请内存

Android 官方文档中的 PSS 是 Proportional Set Size，是内存占用统计口径，不是 allocator 或 memory heap。Aura 不应提供 `allocatePss()` 这种 API。

如果需求是“跨进程共享 CPU 内存”，Android 使用 `ASharedMemory`；如果需求是“CPU/OpenCL/OpenGL/DSP 共享同一块底层 buffer”，Android/Linux 使用 `AHardwareBuffer`、dma-buf heap 或厂商 gralloc/DSP handle。

### 2.2 Android/Linux 可以统一成 fd/handle + capability 模型

Android 和 Linux 都能围绕 fd/handle 建立统一模型:

- 普通共享内存: fd + mmap。
- dma-buf: fd + mmap + `DMA_BUF_IOCTL_SYNC` + device import。
- Android `AHardwareBuffer`: NDK object + lock/unlock + fence，可跨进程传递，也可被图形/计算栈导入。

因此 Aura 的统一对象应是:

- `MemoryDesc`: 调用方表达期望。
- `Memory`: move-only RAII 拥有资源。
- `MappedRange`: RAII 管理 CPU 访问窗口。
- `NativeHandle`: 显式表达 fd/AHardwareBuffer 的所有权。
- `MemoryInfo`: 返回实际 backend、cache policy、能力位。
- `queryDmaHeapAvailable()`: 在 Android/Linux 运行时探测某个 `/dev/dma_heap/<heap>` 对当前进程是否可打开。

### 2.3 cached/uncached 是请求，不是保证

Android dma-buf heap 可以有 `system` 和 `system-uncached`；Linux upstream 通用 heap 以 `system`、`default_cma_region` 等为主，不保证存在 uncached heap。`CachePolicy::Uncached` 必须建模为 request:

- strict 模式: heap 不存在就返回 `err::kErrorNotSupported`。
- best-effort 模式: 可退化到 cached，但 `actualCachePolicy()` 必须返回实际结果。

### 2.4 flush / invalidate 只表示 CPU cache visibility

`flush(offset, size)`:

- CPU 写完后，让 device/other agent 后续读取可见。

`invalidate(offset, size)`:

- device/other agent 写完后，让 CPU 后续读取看到最新内容。

`validate(offset, size)`:

- 兼容用户术语，作为 `invalidate()` 的别名。

这些函数不等价于 GPU/OpenCL/OpenGL queue 同步。设备队列顺序、ownership transfer 和 fence/semaphore 需要在 `au::gpu` 或 vendor adapter 中处理。

## 3. 平台设计

### 3.1 Android

#### 普通共享内存

使用 NDK `ASharedMemory_create` 创建 fd，然后 mmap。适合 CPU IPC，不保证可被 GPU/DSP 零拷贝导入。

Aura backend:

- `Backend::AndroidSharedMemory`
- `MemoryKind::Shared`
- native handle: duplicated `PosixFd`
- CPU map: supported
- flush/invalidate: no-op 或平台同步，跨进程 CPU 可见性由 shared mapping 保证

#### AHardwareBuffer

使用 NDK `AHardwareBuffer_allocate` / `AHardwareBuffer_release` 管理。第一阶段只支持 BLOB/raw buffer 场景，不支持 sampled image / framebuffer 这类图像用途；图像 buffer 需要 `ImageLayout`、format、stride、plane 和 GPU adapter 配合，放到后续阶段。

- `width = size`
- `height = 1`
- `layers = 1`
- `format = AHARDWAREBUFFER_FORMAT_BLOB`
- CPU usage 映射到 CPU read/write flags
- raw GPU storage 在平台提供 `AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER` 时映射；否则返回 not-supported
- `MEMORY_USAGE_GPU_SAMPLED` 和 `MEMORY_USAGE_GPU_FRAMEBUFFER` 在第一版返回 not-supported
- CMake 在 Android NDK 构建时显式链接 `libandroid`，用于解析 `ASharedMemory` 和 `AHardwareBuffer` NDK API。

Aura backend:

- `Backend::AndroidHardwareBuffer`
- `MemoryKind::AndroidHardwareBuffer`
- native handle: `AHardwareBuffer*`，导出时 acquire，调用方 release
- CPU map: 仅在 `MemoryDesc::usage` 包含 CPU read/write 时标记 `MEMORY_CAP_CPU_MAPPABLE`，并通过 `AHardwareBuffer_lock`
- unmap: `AHardwareBuffer_unlock`
- fence fd: 第二阶段接入 `SyncFence`

#### dma-buf heap

Android 12+ GKI 以 DMA-BUF heaps 替代 ION。AOSP `libdmabufheap` 推荐通过 heap name 分配，例如 `system` 和 `system-uncached`。Aura 可以先实现直接打开 `/dev/dma_heap/<heap>` + ioctl 的最小路径，后续再决定是否引入 `libdmabufheap` 作为 Android 优化依赖。

Aura backend:

- `Backend::LinuxDmaBufHeap`
- `MemoryKind::DmaBuf`
- heap name: `MemoryDesc::heapName`，默认按 cache policy 选择
- native handle: duplicated `DmaBufFd`
- CPU sync: `DMA_BUF_IOCTL_SYNC`
- heap query: `queryDmaHeapAvailable("system", &available)` 只报告当前进程是否能打开对应 heap，不承诺设备导入能力

### 3.2 Linux

#### 普通共享内存

优先使用 `memfd_create`，不可用时回退 POSIX `shm_open`。当前实现为了兼容受限开发环境，还提供 unlinked temp-file fallback，仍保持 fd + mmap + export/import 模型；Android/Linux 设备实现可继续替换成更贴近目标平台的 memfd 或 ASharedMemory 路径。

Aura backend:

- `Backend::PosixSharedMemory`
- `MemoryKind::Shared`
- native handle: duplicated `PosixFd`
- CPU map: supported

#### dma-buf heap

Linux dma-buf heap 用户态 API 从 `/dev/dma_heap/<heap>` 分配 dma-buf fd。上游当前文档描述的通用 heap:

- `system`: 虚拟连续、cacheable。
- `default_cma_region`: 物理连续、cacheable，依赖 CMA 区域；Linux 6.17 之前名称不稳定，可能是 `reserved`、`linux,cma` 或 `default-pool`。
- `system_cc_shared`: 只在部分 confidential-computing VM 上存在。

Aura 不假设 `system-uncached` 存在；只有设备实际暴露该 heap 时才支持。

CPU 同步:

- CPU read/write 前: `DMA_BUF_IOCTL_SYNC START`
- CPU read/write 后: `DMA_BUF_IOCTL_SYNC END`
- read 对应 invalidate，write 对应 flush

#### OpenCL/OpenGL/DSP 互操作

`mm` 不直接创建 OpenCL/OpenGL 对象，只负责保存 fd、size、layout 和同步语义。导入到 OpenCL/OpenGL/DSP 放到后续 adapter:

- `au::gpu::opencl::importExternalMemory(...)`
- `au::gpu::opengl::importDmaBufImage(...)`
- `au::gpu::dsp::importDmaBuf(...)`

这样 `mm` 保持基础组件定位，避免在核心库中引入重型 GPU 头和驱动依赖。

## 4. Public API 设计

新增:

```text
inc/mm/xmemory_types.h
inc/mm/xmemory.h
src/mm/xmemory.cpp
test/test_xmemory.cpp
```

### 4.1 核心类型

```cpp
namespace au {
namespace mm {

enum class MemoryKind {
    Host,
    Shared,
    DmaBuf,
    AndroidHardwareBuffer,
    External
};

enum class Backend {
    Auto,
    Host,
    PosixSharedMemory,
    AndroidSharedMemory,
    LinuxDmaBufHeap,
    AndroidHardwareBuffer,
    ExternalFd
};

enum class CachePolicy {
    Default,
    Cached,
    Uncached,
    WriteCombined
};

enum class Requirement {
    BestEffort,
    Strict
};

}
}
```

设计要点:

- `MemoryKind` 表示语义。
- `Backend` 表示实际实现。
- `CachePolicy` 和 `Requirement` 共同表达 “请求 cached/uncached，是否允许退化”。

### 4.2 RAII 对象

`Memory`:

- 析构释放资源。
- move-only。
- 查询 size/backend/cache/capability，或通过 `queryInfo()` 一次性获取结构化 `MemoryInfo`。
- 提供 `flush/invalidate/validate`。
- 不暴露平台头或成员布局。

`MappedRange`:

- 构造由 `map()` 返回。
- 析构自动 unmap/end sync。
- 对 dma-buf 自动包住 CPU sync。
- 对 Host/POSIX shared memory no-op sync。

`queryDmaHeapAvailable()`:

- `heapName == nullptr` 或空字符串返回 `err::kErrorInvalidParam`。
- `out == nullptr` 返回 `err::kErrorNullPointer`。
- Android/Linux 上 heap 不存在返回 success + `false`；heap 可打开返回 success + `true`。
- 存在但当前进程权限不足返回 `err::kErrorPermissionDenied`；其他 open 失败返回 `err::kErrorOpenFailed`。
- 非 Android/Linux 平台返回 `err::kErrorNotSupported`。

### 4.3 NativeHandle

```cpp
enum class NativeHandleType {
    None,
    PosixFd,
    DmaBufFd,
    AndroidHardwareBuffer
};

enum class HandleOwnership {
    Borrowed,
    Duplicated,
    Transferred
};
```

默认导出策略:

- fd: `dup()` 后返回，`ownership = Duplicated`。
- AHardwareBuffer: `AHardwareBuffer_acquire()` 后返回，调用方负责 release。

导入策略:

- `Transferred`: 成功后 Aura 接管；失败时调用方仍负责释放。
- `Borrowed`: Aura 自行 duplicate/acquire。
- `Duplicated`: 调用方传入已经可独立拥有的 handle，但 Aura 不消耗它；导入时仍会自行 duplicate/acquire，除非调用方显式使用 `Transferred`。

## 5. 当前实现范围

本次实现的第一版能力:

1. Host memory:
   - aligned allocation
   - CPU map/unmap
   - move-only RAII
   - no-op flush/invalidate
   - `MemoryInfo` / `queryInfo()` 查询
2. POSIX shared memory:
   - Linux 优先 `memfd_create`，不可用时回退 `shm_open`
   - fd + mmap
   - `shm_open` 失败时使用 unlinked temp-file fallback，便于本地构建验证
   - fd export/import
   - current macOS/Linux build fallback 可测试
3. Linux/Android dma-buf heap:
   - 条件编译
   - `/dev/dma_heap/<heap>` allocation
   - uncached 优先尝试 `system-uncached`，兼容 `system_uncached`，best-effort 时退化到 `system`
   - 物理连续优先尝试 `default_cma_region`，兼容旧内核/设备常见 CMA heap 名
   - `queryDmaHeapAvailable()` 运行时查询 heap 对当前进程是否可打开
   - fd import/export
   - mmap + `DMA_BUF_IOCTL_SYNC`
   - heap 不存在返回 `err::kErrorNotSupported`，权限不足返回 `err::kErrorPermissionDenied`，其他 open 失败返回 `err::kErrorOpenFailed`
4. Android AHardwareBuffer:
   - 条件编译
   - BLOB/raw buffer allocation
   - image sampled/framebuffer usage 明确拒绝
   - lock/unlock mapping
   - CPU map capability 按 CPU read/write usage 精确报告
   - acquire/release ownership
5. CMake:
   - `src/mm/xmemory.cpp` 加入 `aura`
   - `inc/aura.h` re-export `mm/xmemory.h`
   - `test/test_xmemory.cpp` 加入测试开关
   - Android 构建通过 `target_link_libraries(aura PRIVATE android)` 链接 NDK `libandroid`
6. 当前本地验证:
   - macOS 开发机完成 CMake build
   - 完整 `aura_test` 通过
   - `inc/mm/xmemory.h` C++11 header smoke compile 通过
   - Linux 条件分支语法级编译通过: `c++ -std=c++17 -D__linux__ ... src/mm/xmemory.cpp`
   - Android 条件分支语法级编译通过: `c++ -std=c++17 -D__ANDROID__ -D__linux__ ... src/mm/xmemory.cpp`，使用 `tools/android_stub/android/*.h` 验证专用 stub，仅证明调用形态和条件编译没有明显 C++ 语法错误
   - 本机未发现 Linux 交叉编译器或 Android NDK toolchain；`ANDROID_NDK_HOME` / `ANDROID_HOME` 未设置，`/Users/wangchen/Library/Android/sdk/ndk` 不存在，因此 Android/Linux 目标平台编译和设备能力仍未在本机证明
   - 新增 `tools/validate_mm_targets.sh`，用于执行本机 syntax/warning 检查、Host 构建测试，并在具备目标环境时执行 Linux、Android NDK 构建，以及可选 Android adb 真机测试

暂不实现:

- DRM fourcc/modifier image layout。
- OpenCL/OpenGL import adapter。
- DSP vendor adapter。
- Android fence fd 传递和 queue ownership。
- memory pool/budget/statistics。

这些属于下一阶段，并不影响第一版基础内存生命周期和 fd 共享能力。

## 6. 测试计划

基础测试:

- default `Memory` invalid。
- default `Memory` `map()` 返回 `err::kErrorInvalidHandle`，而不是误报 null pointer。
- Host allocate/map/write/read/unmap。
- Host move 后源对象 invalid。
- Host range 越界返回 `err::kErrorOutOfRange`。
- Host native handle export 返回 `err::kErrorNotSupported`。
- dma-buf heap query 对 null/empty heap、non-Linux fallback、Linux/Android missing heap 做确定性验证。

POSIX shared memory 测试:

- allocate shared memory。
- export fd。
- import fd。
- 两个 `Memory` mapping 读写一致。

平台条件测试:

- 非 Linux/Android 请求 dma-buf 返回 not-supported/open-failed，不崩溃。
- Android AHardwareBuffer 代码只在 `__ANDROID__` 编译。
- Linux dma-buf 代码只在 Linux/Android 编译。
- Linux/Android 运行在有权限访问 `/dev/dma_heap/system` 的设备上时，会实际分配 dma-buf、导出 fd、mmap、flush、unmap；设备无 heap 或无权限时只接受明确错误码。
- Android 运行测试会实际分配 AHardwareBuffer BLOB、lock/unlock、导出 native handle，并释放导出的 acquired 引用；image sampled/framebuffer usage 会验证为 not-supported。
- Android AHardwareBuffer 未请求 CPU read/write usage 时不标记 CPU mappable，`map()` 返回 not-supported。

已执行验证矩阵:

| 验证项 | 命令 / 证据 | 结果 | 覆盖范围 |
|---|---|---|---|
| 本地构建 | `cmake --build build` | 通过 | Aura 静态库与 `aura_test` |
| 完整单测 | `./build/aura_test` | 171 tests passed | Host、POSIX shared fallback、fd roundtrip、RAII、错误码、dma-buf heap query |
| Host 验证脚本 | `sh tools/validate_mm_targets.sh host` | 通过 | 复用目标验证入口完成本机 configure/build/test |
| Syntax 验证脚本 | `sh tools/validate_mm_targets.sh syntax` | 通过 | Public header C++11、当前平台 warning、Linux/Android 条件 warning、Linux/Android 条件测试 warning |
| Public header C++11 | `c++ -std=c++11 -Iinc -Ibuild/inc -x c++ -c ...` | 通过 | `inc/mm/xmemory.h` C++11 兼容性 |
| Linux 条件分支语法 | `c++ -std=c++17 -D__linux__ -Iinc -Ibuild/inc -c src/mm/xmemory.cpp` | 通过 | Linux `memfd` / dma-buf 条件编译语法 |
| Android 条件分支语法 | `c++ -std=c++17 -D__ANDROID__ -D__linux__ -Itools/android_stub -Iinc -Ibuild/inc -c src/mm/xmemory.cpp` | 通过 | `ASharedMemory` / `AHardwareBuffer` 条件编译语法；非 NDK 真编译 |
| Linux 条件测试语法 | `c++ -std=c++17 -DENABLE_TEST_XMEMORY=1 -D__linux__ ... -c test/test_xmemory.cpp` | 通过 | Linux dma-buf 条件测试编译语法 |
| Android 条件测试语法 | `c++ -std=c++17 -DENABLE_TEST_XMEMORY=1 -D__ANDROID__ -D__linux__ -Itools/android_stub ... -c test/test_xmemory.cpp` | 通过 | Android AHardwareBuffer 条件测试编译语法；非 NDK 真编译 |
| Warning 编译 | `c++ -std=c++17 -Wall -Wextra -Wpedantic ... src/mm/xmemory.cpp`，并覆盖 Linux/Android 条件宏 | 通过 | `xmemory.cpp` 和 `test_xmemory.cpp` 当前无本机 warning |
| 格式检查 | `git diff --check` | 通过 | tracked diff whitespace |
| 目标工具链发现 | `command -v aarch64-linux-android21-clang++` / `command -v x86_64-linux-gnu-g++` / `printenv ANDROID_NDK_HOME` | 未发现 | 解释为什么当前只能完成语法级 Android/Linux 检查 |
| 目标验证脚本语法 | `sh -n tools/validate_mm_targets.sh` | 通过 | Host/Linux/Android 验证入口的 shell 语法 |

仍缺验证:

- Android NDK 真正交叉编译。
- Linux 发行版或嵌入式 Linux 真正编译。
- Android 真机 `ASharedMemory` 分配、fd 导入导出、mmap 读写。
- Android 真机 `AHardwareBuffer` BLOB 分配、lock/unlock、acquire/release。
- Linux/Android 设备 `/dev/dma_heap/system` 或 `system-uncached` 实际分配与 `DMA_BUF_IOCTL_SYNC`。

目标环境可用后的建议命令:

```bash
sh tools/validate_mm_targets.sh linux
sh tools/validate_mm_targets.sh syntax
ANDROID_NDK_HOME=/path/to/android-ndk sh tools/validate_mm_targets.sh android
ANDROID_NDK_HOME=/path/to/android-ndk AURA_ANDROID_ADB_TEST=1 sh tools/validate_mm_targets.sh android
```

## 7. 后续路线

下一阶段推荐:

1. 增加 `ImageLayout`，覆盖 DRM fourcc/modifier/stride/offset/plane。
2. 增加 `SyncFence`，管理 dma fence fd、AHardwareBuffer unlock fence、EGL/CL acquire/release。
3. 增加 Android 真机测试:
   - `ASharedMemory`
   - `AHardwareBuffer`
   - `/dev/dma_heap/system`
   - 可选 `/dev/dma_heap/system-uncached`
4. 增加 Linux 设备测试:
   - `/dev/dma_heap/system`
   - `/dev/dma_heap/default_cma_region`，并按设备实际情况覆盖旧内核/厂商 CMA heap 名
   - EGL dma-buf import 集成测试

## 8. 资料依据

官方资料:

- Android NDK `ASharedMemory`: https://developer.android.com/ndk/reference/group/memory
- Android NDK `AHardwareBuffer`: https://developer.android.google.cn/ndk/reference/group/a-hardware-buffer
- Android AOSP ION 到 DMA-BUF heaps 迁移: https://source.android.com/docs/core/architecture/kernel/dma-buf-heaps
- Android PSS / RSS / USS 说明: https://developer.android.com/topic/performance/memory-management
- Linux dma-buf heaps: https://docs.kernel.org/userspace-api/dma-buf-heaps.html
- Linux dma-buf sharing and synchronization: https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html
- Khronos OpenCL external memory: https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/cl_khr_external_memory.html
- Khronos EGL dma-buf modifiers: https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt

开源项目:

- AOSP `libdmabufheap` `BufferAllocator`: https://android.googlesource.com/platform/system/memory/libdmabufheap/+/refs/heads/android16-qpr2-release/BufferAllocator.cpp
- GStreamer DMA-BUF design: https://gstreamer.freedesktop.org/documentation/additional/design/dmabuf.html
- Chromium Linux NativePixmap dmabuf: https://chromium.googlesource.com/chromium/src/+/0d58af6fc31ca16210c8a0c475fce91fff516ab2/ui/gfx/linux/client_native_pixmap_factory_dmabuf.cc
- Vulkan Memory Allocator: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator

## 9. 最终建议

第一版 `mm` 应先把 Android/Linux 的生命周期、fd 共享、CPU mapping 和 cache sync 这条主线做扎实。API 上保留能力查询和 native handle 边界，后续再扩展 OpenCL/OpenGL/DSP import adapter。这样不会过早把 GPU/DSP 细节耦合进基础库，同时能满足 Aura 作为高性能基础组件库对 RAII、安全释放和异构共享的核心要求。
