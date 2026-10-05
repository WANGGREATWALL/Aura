# CL symbols 照片转录、方案比较与替换记录

结论：照片方案更适合作为替换基础，主要优势是平台加载路径、Aura 命名空间和新增接口，而不是不同的符号加载架构。现有方案与照片方案都是 `XDLib` 单例加全局 OpenCL C 转发函数。已基于照片方案完成替换及补齐；未编译，未调用 OpenCL 运行时。

## 照片转录范围

本目录的 `cl_symbols.h`、`cl_symbols.cpp` 是参考归档，不加入构建，也不作为运行实现。

| 照片 | 原始行号 | 转录内容 |
| --- | --- | --- |
| cl-symbols6.jpg | 1–50 | 头文件起始、注释、依赖及 OpenCL 版本宏 |
| cl-symbols5.jpg | 50–96 | 命名空间、单例、加载失败日志及路径列表起始 |
| cl-symbols4.jpg | 93–125 | 平台路径列表和头文件结束 |
| cl-symbols3.jpg | 1–50 | 源文件宏、平台与设备转发 |
| cl-symbols2.jpg | 47–97，98 行截断 | 设备信息、上下文的四个完整转发函数及 `clCreateContext` 签名起始 |
| cl-symbols1.jpg | 432–476；427 行仅签名 | 事件诊断、回调、队列同步、平台扩展函数查询及源文件结束 |

头文件完整转录为 125 行。源文件包含 15 个完整可见转发函数，原始 94–431 行未完整提供：`clCreateContext` 的起始签名和 `clRetainEvent` 的单行签名以注释保留，未猜测照片中不可见的函数体。两张照片边缘的截断文字不冒充完整代码。

照片注释提及 `get()` 单例和 X-Macro 预解析表，但可见实现实际提供 `lib()`，没有 X-Macro 函数表；不能依据这些注释推断其具有预解析性能优势。归档保留这些原始注释。

## 客观比较

比较基线是修改前的 `src/gpu_helper/cl_symbols.h/.cpp`，其中有 51 个完整转发函数。

| 维度 | 修改前仓库实现 | 照片实现 | 判断 |
| --- | --- | --- | --- |
| 加载路径 | Android 及若干硬编码旧 Windows SDK 路径；没有 macOS 框架和 Linux 常规 SONAME | 按 Windows、Apple、Android、Linux 分支选择常规库名和路径 | 照片更适合跨平台使用，尤其是当前 macOS 环境 |
| 初始化失败 | `XCHECK` 失败后 `abort()` | 记录错误后继续返回 | 照片更适合作为可选 GPU 组件，但后续仍需检查空指针 |
| 命名空间与所有权 | 全局 `gpu`；复制操作未显式删除 | `au::gpu`；显式删除复制操作 | 照片更符合 Aura 规范；两者都由 `XDLib` 析构释放句柄 |
| 稳态调用 | 每次调用 `XDLib::get()`：查表时仍获取 `mMutex` | 同样每次调用 `XDLib::get()` | 两者相同，照片没有可证实的性能优势；未做基准测试 |
| 缺失符号 | 未检查查找结果，直接调用 | 同样直接调用 | 两者都有空指针调用风险，不能原样采用 |
| 调用约定 | `clGetEventProfilingInfo`、`clFinish` 定义漏写 `CL_API_CALL` | 两者都带 `CL_API_CALL` | 照片在 32 位 Windows ABI 上更正确；64 位平台也应一致书写 |
| 事件与扩展入口 | 无 `clSetEventCallback`、`clGetExtensionFunctionAddressForPlatform` 转发 | 两者均可见 | 照片覆盖更完整 |
| OpenCL 版本 | Apple 配置 1.2，但 2.0 类型和函数定义未按版本隔离 | 可见配置相同；未拍到的主体无法评价 | 当前实现存在问题，照片主体不能据缺失片段作肯定判断 |
| 句柄生命周期 | `lib()` 返回可修改 `XDLib&`，允许外部卸载/重载 | 同样返回可修改引用 | 两者均不适合直接叠加永久函数指针缓存，需收紧所有权 |

扩展函数应通过平台扩展入口查询，返回非空指针仍须检查平台/设备是否支持对应扩展；该入口不能用于查询核心函数。这些约束来自 [Khronos 的 clGetExtensionFunctionAddressForPlatform 说明](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clGetExtensionFunctionAddressForPlatform.html)。本次核心转发仍从已加载库查询，厂商扩展由调用方通过该标准入口获取。

## 实际替换与补齐

运行代码位于 `src/gpu_helper/cl_symbols.h/.cpp`，与本目录照片转录分开保留。

- 沿用照片的分平台库路径、非致命加载失败策略、`au::gpu` 和版本配置；包含仓库自带的 `CL/opencl.hpp`，避免使用仅转发并提示改名的 `CL/cl2.hpp`。
- 按仓库 Khronos 头文件生成 114 个核心入口和 9 个 OpenGL 共享入口，共 123 个转发函数。保留原有全部 51 个入口，覆盖照片中全部 15 个完整函数，并补齐 `clCreateCommandQueue`、缓冲区读写、事件查询、内核工作组查询等 CLHPP 所需入口。
- 每个转发函数使用线程安全的函数内静态初始化，缓存一次类型正确的函数指针，包括缺失结果。后续转发不再进入 `XDLib` 查表或获取其互斥锁；第一次初始化仍可能分配内存、加载库及获取锁，属于冷路径。
- 空指针分支明确返回：普通 `cl_int` 返回 `CL_INVALID_OPERATION`；创建/映射接口返回空并设置可选的 `errcode_ret`；平台枚举返回 `CL_PLATFORM_NOT_FOUND_KHR` 并清零可选的平台数量；扩展查询返回空。`clSVMFree` 无错误返回值，缺失时直接返回，不伪造释放成功。
- 所有全局转发使用 C 链接及 `CL_API_CALL`，保留 `CL_CALLBACK` 回调类型、参数顺序和按版本启用的条件。可选的 2.0 及更高版本符号不会因库已加载而被假定存在。
- `CLSymbols` 不再暴露可修改的 `lib()`，改用 `get<Func>()` 和 `isLoaded()`。库加载后不重载，避免缓存指针被卸载失效；保留全局 `gpu::CLSymbols` 类型别名以兼容旧命名空间。仓库里没有依赖旧 `lib()` 的外部调用点；仓库外的这类调用应改用 `get<Func>()`。
- `src/sys/xdlib.h` 配套修正 Windows 默认加载标志为 0，其他系统维持 `RTLD_NOW | RTLD_LOCAL`，并防止 Windows 的 `min/max` 宏污染。未改变其缓存、加载和卸载实现。

生成脚本是 `src/gpu_helper/tools/generate_cl_symbols.py`，从本地 `CL/cl.h` 和 `CL/cl_gl.h` 读取原型，不需要编译器、网络或 OpenCL 运行时。

## 验证与边界

完成文本核对：123 个函数的返回类型、参数及回调签名与对应 Khronos 原型一致；参数转发顺序、失败返回、版本保护、原有入口覆盖、CLHPP 的 120 个字面量函数引用覆盖及生成结果可重复性均已核对。另核对 50 个照片行号锚点、头文件条件编译和文件括号闭合。Apple 默认启用的 97 个入口均在本机 SDK 的头文件中有声明；本机 SDK 未定义的 `CL_PLATFORM_NOT_FOUND_KHR` 使用 Khronos 规定的 -1001，避免依赖该宏是否存在。

没有运行编译、链接、GPU 测试或性能基准，因此这些检查不能替代跨平台运行验证。现有 CMake 没有把 `gpu_helper` 加入目标，本次未修改构建配置。`cl_wrapper` 和既有的 OpenCL 2.0 演示测试未改动，其自身平台适配不在本次范围内。

Apple 下必须先包含 `cl_symbols.h`，再包含其他 OpenCL 头文件；若先以不一致版本解析 CLHPP，头文件给出明确诊断。已解析的第三方头文件不能通过后续修改宏降级。

单例以 RAII 管理动态库，在静态析构阶段卸载；OpenCL 对象及相关工作须在此之前结束，静态析构后不得再调用入口。由于缓存缺失结果，本次实现不支持进程运行中安装驱动后重试或重新加载库。符号存在也不代表设备支持对应功能，调用方仍须查询能力并处理驱动返回的错误码。
