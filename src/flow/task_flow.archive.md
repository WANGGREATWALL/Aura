# TaskFlow 照片归档说明

按用户选择忠实归档 15 张照片，保留原文件名、`iac::vaif` 命名空间、接口、实现及依赖引用。此处的代码是原项目源码归档，未改写成 Aura 的 `au::flow` 接口，未加入构建，也未编译。

| 归档文件 | 照片来源 | 内容 |
| --- | --- | --- |
| `iac_task_flow.h` | taskflow12–14.jpg | 完整头文件，52 行；照片 13、14 为重复内容 |
| `task_flow.cpp` | taskflow1–11.jpg | 完整可见源码，491 行；按照片行号拼接重叠片段 |
| `iac_vaif_def.h` | taskflow15.jpg | `NS_BEG`、`NS_END` 命名空间宏，4 行 |

## 原始资料中的缺口

- `TaskFlow::pair(const Task*, const Task*)` 和 `TaskFlow::bind(...)` 仅出现在头文件声明中，源码照片没有对应定义。归档保留声明，不补写或猜测实现。
- `iac_default_defs.h` 被头文件与源码引用，但照片未提供其内容，Aura 仓库中也没有该文件。`VDKResultSuccess`、`VDKResultEInvalidParam`、`VDKResultEExpired`、`VDKResultEBadState` 以及 `LOGE`、`LOGV` 的原始定义来源需要另行补齐；未假定其数值或宏实现。
- 原图使用 `gettid()`、`pthread_self()`、`pthread_setname_np()` 和 `<stdatomic.h>`；相关原项目平台声明、兼容层及工具链配置未提供。原图中的标准库引用与 include 列表也按原样保留，未补写可能来自外部头文件的声明。

已核对照片重叠部分、关键函数行号、方法清单、命名空间与括号闭合。这里的“完整”指照片可见代码完整归档；上述原始资料缺口仍然存在，未经编译或运行验证。
