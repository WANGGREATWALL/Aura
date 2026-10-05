# ximagef 照片归档

按照片整理，保留原接口、实现、默认参数、注释及 `au::cv` 命名空间；未编译。

| 文件 | 照片来源 | 内容 |
| --- | --- | --- |
| `ximagef.h` | ximagef-h1–h8.jpg | 316 行；`Image` 的构造、复制与移动、释放、缓存管理、谓词、诊断及类型转换 |
| `ximagef_api.h` | ximagef-h9–h14.jpg | 244 行；4 个默认常量及 18 个自由函数声明 |
| `../../src/cv/ximagef.cpp` | ximagef-cpp1–cpp13.jpg | 560 行；43 项格式描述、9 个内部辅助函数及全部 18 个自由函数实现 |
| `../vivo_comdef.h` | comdef1–comdef3.jpg | 128 行；3 个枚举、`VImageF` 及其指针别名 `PVImageF` |

两份头文件及源文件的可见代码已完整归档，重叠片段已合并，关键行号、函数清单、默认参数、格式描述及括号闭合已核对。源文件的 18 个公开函数签名与 `ximagef_api.h` 一致；本次源文件整理核对了 66 个照片行号锚点，格式表与诊断名称均覆盖同一组 43 个格式。

`ximagef_api.h` 引用的 `vivo_comdef.h` 已根据新增的 3 张照片归档到 `inc/vivo_comdef.h`，包含 `VImageColorSpace`、`VImageFormatF`、`VImageF`、`PVImageF` 和 `VAlgoModelType`。枚举的显式数值、按位或表达式、被注释的候选格式及结构体字段顺序均按照片保留。

`vivo_comdef.h` 还引用 `vivo_comdef_v1.h`，该文件未出现在照片中，当前 Aura 仓库也没有该文件。`VImage`、`VImageEx`、`VImageExV1`、旧版图像格式及 `VDKResult` 等依赖定义仍未提供；归档保留引用，不猜测其结构布局或数值。

照片原代码中有以下行为与头文件注释的差异，归档保留原代码：

- `isSameSizeWith()` 在格式相同时仅比较 `stride[0]`，头文件注释描述的是逐平面比较步长。
- `createVImageF()` 的 `tag` 参数在实现中被注释为未使用，没有传递给内存分配接口。
- `createVImageF()` 在布局规划或内存分配失败时可能保留宽、高、格式及颜色空间字段，返回值仍然无效，但并非头文件注释所述的全部清零。

当前完成的是照片代码归档和文本核对，未编译，未验证链接或运行行为；未补造外部依赖，也未修改构建配置。
