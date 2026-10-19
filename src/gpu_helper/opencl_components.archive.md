# OpenCLWrapper 配套组件照片归档说明

已将 `opencl-wrapper-ext1.jpg` 至 `opencl-wrapper-ext10.jpg` 转写为以下五个文件。保留照片中的接口、注释、命名空间、控制流和依赖；仅统一缩进与空白。所有文件存放在用户指定的 `src/gpu-helper`，未加入构建系统。

| 组件 | 文件 | 行数 | 内容 |
| --- | --- | --- | --- |
| OpenCLRegistry | opencl_registry.h / opencl_registry.cpp | 79 / 92 | fd 弱缓存、重复 fd 持有、过期项清理、图像导入 |
| OpenCLBuffer | opencl_buffer.h / opencl_buffer.cpp | 71 / 94 | DMA-BUF 延迟导入、cl::Buffer 与 fd 析构、CPU／设备缓存同步 |
| OpenCLArgBuffer | opencl_arg.h | 77 | buf() 参数包装、共享句柄、类型特征 |

这些文件补齐了此前 [OpenCLWrapper 归档](opencl_wrapper.archive.md) 中列出的三个本地组件缺口。共享指针按照片用于跨异步内核完成事件持有 Buffer；Registry 只持有弱引用。

## 照片覆盖

顶部固定作用域行不重复转写，重叠片段用于交叉核对。

| 照片 | 文件 | 主编辑区行号范围 |
| --- | --- | --- |
| ext1 | opencl_registry.h | 1–49 |
| ext2 | opencl_registry.h | 48–79 |
| ext3 | opencl_buffer.h | 1–49 |
| ext4 | opencl_registry.cpp | 1–49 |
| ext5 | opencl_buffer.h | 44–71 |
| ext6 | opencl_registry.cpp | 50–92 |
| ext7 | opencl_arg.h | 1–50 |
| ext8 | opencl_buffer.cpp | 1–50 |
| ext9 | opencl_arg.h | 44–77 |
| ext10 | opencl_buffer.cpp | 49–94 |

## 依赖与集成边界

- `opencl_wrapper.h`、`opencl_buffer.h`、`opencl_registry.h`、`opencl_arg.h` 的本地引用已齐全。`cl_symbols.h`、`CL/cl_ext.h` 和提供 `clErrorInfo()` 的 `cl_wrapper.cpp` 仍位于 `src/gpu_helper`，将来集成需要相应的 include 路径及链接依赖。
- `cv/ximagef.h`、`mm/xmemory.h`、`log/xerror.h` 与 `log/xlogger.h` 已存在。图像 ABI 依赖的 `vivo_comdef_v1.h` 仍缺失，详情见 [图像归档说明](../../inc/cv/ximagef.archive.md)。未推测其类型或增加占位实现。
- 照片调用 `sys::isMediaTekPlatform()`；当前仓库在 `inc/sys/xsystem_vivo.h` 中声明的是 `au::sys::vivo::isMediaTekPlatform()`。此前 Wrapper 的 Qualcomm 调用有同样差异。保持照片原文，未改写平台接口。
- `opencl_arg.h` 使用 `std::move`，照片未直接包含 `<utility>`；保留原包含链依赖。`<type_traits>` 已按照片包含。
- 两个实现直接包含 `<unistd.h>`；Buffer 导入使用 ARM／Qualcomm 扩展。此归档不包含 Windows／macOS 适配，也未改变照片中的移动端平台条件或此前 Wrapper 的 OpenCL 2.0 编译选项。

## 保留的原代码行为

本次为照片归档，以下内容属于原实现，未作为可运行修复处理：

- `getOrCreate()` 仅按原始 fd 整数复用存活项，不核对新的 `hostPtr`／`size` 或 fd 关闭后的号码复用。持锁范围包含 `dup()`、失败日志、对象分配和容器修改；`getOrCreate()` 的自行清理只覆盖本次查询的过期槽，其他槽需 `purgeExpired()`。
- `getOrCreate()` 是 `noexcept`，但内存分配和容器操作未捕获异常；对象分配失败前已复制的 fd 也没有独立 RAII 守卫。保留照片中的私有构造及 `shared_ptr(new OpenCLBuffer(...))` 写法，未新增移动接口或调整类布局。
- `importImage()` 固定遍历四个平面，将 `fdOffset[i] + dataSize[i]` 作为 span；未增加负偏移／加法溢出校验，也未验证所有平面共享 `fd[0]`。该便利接口只导入 `fd[0]`，独立平面 fd 不能按此接口整体导入。
- `buffer()` 的延迟初始化没有同步保护，多个线程首次使用同一共享句柄时存在竞争。ARM 扩展函数指针首次解析后静态缓存，未随 Wrapper 平台／上下文重新初始化而刷新。
- fd 的复制延长 DMA-BUF 文件资源生命周期，`mHostPtr` 仍是借用的 CPU 映射指针；本组件不拥有其映射或源图像内存。`buf()` 忽略 `syncToDevice()` 的错误码，空 host 指针的同步返回成功。
- `OpenCLBuffer` 析构先释放 `cl::Buffer`，随后调用 `close(mFd)`。因此，先前 Wrapper 关于完成回调线程“只调用 clReleaseMemObject”的注释并不完整；最后一个共享引用也可能在回调线程关闭 fd。回调注册失败即释放共享句柄的行为见 [Wrapper 说明](opencl_wrapper.archive.md)。
- `OpenCLArgBuffer` 含有 `std::shared_ptr`，照片中的“POD wrapper”注释并非严格的 C++ POD 类型描述，原文予以保留。

## 核对结果

五个文件共 413 行，完整覆盖十张照片的连续源码区段。已核对重叠片段、行号锚点、头文件保护、命名空间与括号配对、成员声明与定义，以及参数包装到 Wrapper 的引用关系。

未运行 CMake、编译器或 GPU 测试。文本核对用于确认照片转写完整；依赖缺口和原实现的运行问题仍以上述说明为准。
