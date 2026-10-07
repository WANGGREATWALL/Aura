# OpenCLWrapper 照片归档说明

已将 `opencl-wrapper1.jpg` 至 `opencl-wrapper10.jpg` 中的代码恢复为 `opencl_wrapper.h`（162 行）与 `opencl_wrapper.cpp`（255 行）。文件存放在此次明确指定的 `src/gpu-helper` 目录；此前的 `CLWrapper` 与 `CLSymbols` 仍位于 `src/gpu_helper`。两个目录未合并，已有文件未修改。

保留照片中的 `au::gpu::OpenCLWrapper` 单例、原始接口与实现：Buffer 参数派发、完成事件绑定的共享句柄持有、平台／设备／上下文／队列初始化、内核缓存、设备与源码哈希二进制缓存，以及二进制 build 失败后的源码 JIT 回退。`clErrorInfo()` 仅声明，按照片复用 `src/gpu_helper/cl_wrapper.cpp` 中已有的定义。

## 照片覆盖

主编辑区连续覆盖两个文件，顶部固定显示的作用域行未重复列入。部分底部行由下一张照片补全。

| 照片 | 文件 | 行号范围 |
| --- | --- | --- |
| 1 | opencl_wrapper.h | 49–93 |
| 2 | opencl_wrapper.h | 1–50 |
| 3 | opencl_wrapper.cpp | 48–96 |
| 4 | opencl_wrapper.cpp | 1–50 |
| 5 | opencl_wrapper.cpp | 95–142 |
| 6 | opencl_wrapper.h | 130–162 |
| 7 | opencl_wrapper.cpp | 141–188 |
| 8 | opencl_wrapper.h | 93–137 |
| 9 | opencl_wrapper.cpp | 224–255 |
| 10 | opencl_wrapper.cpp | 186–233 |

## 依赖缺口

- `opencl_arg.h` 缺失；代码依赖其中提供的 `OpenCLArgBuffer`、`isOpenCLArg<T>()`，以及可访问 `buffer()` 的完整 `OpenCLBuffer` 类型。照片注释提到的 `opencl_buffer.h`、`opencl_registry.h` 也未在当前仓库找到。这 10 张照片没有它们的内容，未推测或补写占位实现。
- `inc/vivo_comdef.h` 依赖的 `vivo_comdef_v1.h` 缺失，当前仓库未提供此代码需要的 `VDKResult*` 定义。
- 照片包含 `sys/xsystem.h`，当前平台识别函数的声明位于 `inc/sys/xsystem_vivo.h`。本次保持照片的 include 列表。
- `cl_symbols.h` 实际位于 `src/gpu_helper`；将来构建此归档时需要相应的 include 路径，并复用已有的 OpenCL 符号包装及 `clErrorInfo()` 定义。
- 头文件直接使用 `std::enable_if`、`std::move`，照片未单独包含 `<type_traits>`、`<utility>`；原代码依赖包含链，本次未改变该行为。
- 默认编译选项为 `-cl-std=CL2.0 -cl-fast-relaxed-math`，初始化要求 MediaTek / Qualcomm 平台；当前 `cl_symbols.h` 在 Apple 上选择 OpenCL 1.2。归档不等于完成 macOS 运行适配或 CMake 集成。

## 保留的原代码行为

此次按照片归档，没有对原实现进行逻辑修复：

- 第 124–130 行使用原始 `new` / `delete` 将 `PendingBag` 转交驱动回调。当 `clSetEventCallback()` 失败时，立即删除句柄包；没有先等待已提交的内核完成。该失败路径可能破坏头文件所述的 Buffer 生命周期保证。
- 第 168–172 行的设备属性查询复用同一个错误变量，仅在最后一次查询后检查，较早查询的错误可能被覆盖。
- 内存内核缓存只按 `nameKernel` 命中；同名且源码／选项变化的后续提交会复用旧内核。
- `enqueue()` 忽略 `flush()` 返回值；回调包分配与容器操作没有异常屏障。`deinit()` 未主动调用 `finish()`，调用方需遵守原代码的同步与销毁约定。
- 注释中关于回调线程只释放 `cl::Buffer` 的说明属于照片原文；由于 `OpenCLBuffer` 的实现尚未提供，此处不能验证其完整析构行为。

## 核对结果

未运行 CMake、编译器或 GPU 测试。仅进行照片与文本检查：10 张照片连续覆盖头文件和实现；51 处行号锚点与照片一致；14 个非内联类成员的参数类型、返回类型、const／noexcept 修饰与声明对应，另核对了构造／析构函数、模板参数派发、完成回调和 JIT 回退分支。括号配对与新增文件的 `git diff --no-index --check` 均通过。

以上确认照片转写内容完整，不表示缺失依赖已补齐或运行逻辑已验证。
