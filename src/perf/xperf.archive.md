# perf 照片归档说明

已按 `perf1.jpg` 至 `perf10.jpg` 恢复三个头文件到 `inc/perf`。按用户后续要求，照片版本直接替换同名的 `xperf_types.h`；新增 `xperf.h` 与 `xperf_config.h`。保留照片的接口、注释、成员顺序和控制流，仅统一空白与缩进。

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| [xperf.h](../../inc/perf/xperf.h) | 116 | 组合 Timer／Tracer 的 XPerfScoped，两个 sub() 重载，禁用拷贝及移动 |
| [xperf_types.h](../../inc/perf/xperf_types.h) | 91 | C 兼容 AuPerfScope、四个宏常量、C++ PerfScope 别名 |
| [xperf_config.h](../../inc/perf/xperf_config.h) | 205 | Config 单例与全局开关、调试模式、层级、根名称、聚合输出接口 |

这批照片全部为头文件，没有 `.cpp` 内容；`XPerfScoped` 与 `Config` 的成员实现已内联在头文件中。没有补写照片未提供的 C ABI 声明或实现，没有修改现有 `xtimer.cpp`／`xtracer.cpp` 或构建配置。

## 照片覆盖

顶部固定显示的作用域行不重复转写，重叠区段用于交叉核对。

| 照片 | 文件 | 主编辑区行号范围 |
| --- | --- | --- |
| perf1 | xperf.h | 83–116 |
| perf2 | xperf.h | 49–97 |
| perf3 | xperf.h | 1–49 |
| perf4 | xperf_types.h | 49–91 |
| perf5 | xperf_types.h | 1–50 |
| perf6 | xperf_config.h | 172–205 |
| perf7 | xperf_config.h | 140–184 |
| perf8 | xperf_config.h | 96–140 |
| perf9 | xperf_config.h | 49–97 |
| perf10 | xperf_config.h | 1–50 |

## 依赖与版本差异

- 照片版 `xperf_config.h` 包含 `perf/xtimer_api.h` 并调用十三个 `au_perf_*` 配置函数。当前仓库未提供该头文件或这些 C ABI 定义；照片注释提及的 `xtracer_api.h` 也未提供。
- 当前 `inc/perf/xtimer.h` 中另有 `au::perf::Config`，调用现有 C++ 自由函数，与照片版 Config 的 C ABI 转发不同。同时包含这两个 Config 定义会产生重定义；本次未将现有 Timer／Tracer 迁移至照片中的 C ABI 架构。
- 原 `au::perf::PerfScope` 是独立结构体；照片版改为全局 `AuPerfScope_` 结构体的别名。字段仍为八个 `uint64_t`，但 C++ 类型身份发生变化，依赖它的已有二进制不能视为 ABI 兼容。
- `XPerfScoped` 按照片先声明 `mTimer`、再声明 `mTracer`，析构顺序为先 Tracer 后 Timer。现有 Timer／Tracer 头文件提供其调用的构造函数和两个 `sub()` 重载。
- `Config::getRootName()` 保留照片中的 256 字节栈缓冲区和 `noexcept` 字符串返回。尚未提供的 C ABI 必须限制返回长度；若返回大于缓冲区可用长度的值，字符串构造会越界读取。字符串分配失败也不会由 `noexcept` 自动捕获。
- 照片中关于所有平台八字节对齐、底层无锁原子、默认阈值及吞掉底层失败的注释属于原文。仅凭这批头文件不能验证底层实现行为。

## 核对结果

三个头文件共 412 行，连续覆盖全部十张照片。已进行照片行号锚点、重叠片段、头文件保护、预处理条件、括号配对、十三个配置函数转发与组合作用域成员顺序核对；文本空白检查通过。

按用户要求未运行 CMake、编译器或性能测试。以上核对确认照片转写内容完整，不表示缺失的 C ABI 已补齐或完成运行集成。
