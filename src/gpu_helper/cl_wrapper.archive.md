# CLWrapper 照片恢复说明

来源为 `cl-wrapper1.jpg` 至 `cl-wrapper35.jpg`（`/Users/wangchen/Downloads/aura_code_images/`）。已完整替换本目录的 `cl_wrapper.h`、`cl_wrapper.cpp`，保持照片的接口、命名空间、依赖、默认值及实现逻辑，仅整理空格与对齐。头文件 272 行、实现 1244 行；关键函数行号保留，便于与照片核对。

照片版位于 `au::gpu`，通过 `CLWrapper::get()` 获取单例，使用 `init(folderBinary, enableProfiling)` / `deinit()` 管理状态。原仓库的带参数构造、`build()`、`createKernels()` 和 `.key` 缓存流程已由照片中的源码按需编译、内核缓存、设备与源码哈希二进制缓存取代。DMA 导入、基于 Buffer 创建 Image2D、行距查询、工作尺寸向上取整、两个 `enqueue()` 重载及性能事件查询均已恢复。

## 照片覆盖

以下为照片主编辑区的行号范围，顶部固定显示的父级作用域行不重复列出。照片间的重叠用于交叉核对；范围包含画面底部部分显示、由下一张补全的行。

| 照片 | 文件 | 行号范围 |
| --- | --- | --- |
| 1 | cl_wrapper.h | 50–94 |
| 2 | cl_wrapper.h | 135–179 |
| 3 | cl_wrapper.h | 1–50 |
| 4 | cl_wrapper.h | 239–273 |
| 5 | cl_wrapper.h | 221–264 |
| 6 | cl_wrapper.h | 179–223 |
| 7 | cl_wrapper.cpp | 50–95 |
| 8 | cl_wrapper.h | 92–136 |
| 9 | cl_wrapper.cpp | 1–50 |
| 10 | cl_wrapper.cpp | 93–138 |
| 11 | cl_wrapper.cpp | 137–182 |
| 12 | cl_wrapper.cpp | 180–224 |
| 13 | cl_wrapper.cpp | 224–270 |
| 14 | cl_wrapper.cpp | 259–306 |
| 15 | cl_wrapper.cpp | 304–351 |
| 16 | cl_wrapper.cpp | 395–441 |
| 17 | cl_wrapper.cpp | 573–619 |
| 18 | cl_wrapper.cpp | 351–396 |
| 19 | cl_wrapper.cpp | 440–485 |
| 20 | cl_wrapper.cpp | 483–529 |
| 21 | cl_wrapper.cpp | 528–574 |
| 22 | cl_wrapper.cpp | 663–709 |
| 23 | cl_wrapper.cpp | 618–664 |
| 24 | cl_wrapper.cpp | 708–754 |
| 25 | cl_wrapper.cpp | 798–844 |
| 26 | cl_wrapper.cpp | 753–799 |
| 27 | cl_wrapper.cpp | 1020–1066 |
| 28 | cl_wrapper.cpp | 1153–1199 |
| 29 | cl_wrapper.cpp | 1109–1153 |
| 30 | cl_wrapper.cpp | 932–978 |
| 31 | cl_wrapper.cpp | 886–932 |
| 32 | cl_wrapper.cpp | 1199–1244 |
| 33 | cl_wrapper.cpp | 1065–1111 |
| 34 | cl_wrapper.cpp | 975–1021 |
| 35 | cl_wrapper.cpp | 843–887 |

## 当前仓库的依赖与接口缺口

- `cv/ximage_algo.h`、`vivo_comdef_v1.h` 缺失。已有 `inc/vivo_comdef.h` 本身依赖后者，尚未提供这里使用的 `VImage`、`VDKResult*`、`NTI_*` 等旧类型及常量。未猜测其值或添加占位实现。
- 当前 `src/cv/ximage.h` 定义的是基于 `au::cv::Image` / `ImageRaw` 的 `XImage`，未提供照片使用的 `VImage` 自由函数重载和 `cv::dataptr<uint8_t>(VImage&, ...)`。恢复 `ximagef` 并不能替代这些旧接口。
- 照片只包含 `sys/xsystem.h`，当前仓库的 `isMediaTekPlatform()` / `isQualcommPlatform()` 声明位于 `sys/xsystem_vivo.h`。保留照片的 include 列表。
- 照片使用 OpenCL 2.0 的 SVM 与 ARM / Qualcomm 扩展；当前 `cl_symbols.h` 在 Apple 平台选择 OpenCL 1.2。此份源码还在 `init()` 中要求 MediaTek / Qualcomm 平台，尚不是 macOS 的通用运行实现。
- `src/gpu_helper/cl_hpp_test.cpp` 仍使用旧的 `gpu::CLWrapper(folder, name)` 和 `init()` 调用方式。此次没有改写该测试；当前 CMake 也未把这套 GPU helper 加入库目标。

这些是源代码依赖和仓库版本的差异，不是照片内容缺失；本次没有扩展到依赖适配或构建集成。

## 保留的原代码问题

为使恢复结果可与照片直接比较，下列原有行为未悄悄修改：

- 实现第 734 行，第二个平面调用 `enqueueUnmapMemObject(plane1, data0, ...)`，而其映射结果是 `data1`。
- 第 970 行行距检查使用 `retGetImagePitchAlignment == CL_SUCCESS || pitch_alignment == 0`，不能阻止零对齐值进入计算；该函数的部分错误路径还将错误码作为 `size_t` 返回。
- 第 880 行支持格式查询固定使用 `CL_MEM_OBJECT_IMAGE2D`，没有使用传入的 `object` 参数。
- 第 1104–1114 行二进制 build 失败后只记录日志，未清空 `program`，所以后续 `program.get() == nullptr` 的源码编译分支不一定执行。
- 第 1059–1062 行内存内核缓存仅按名字查找；同名但源码不同的再次提交会命中旧内核。头文件第 170 行也未检查 `flush()` 的返回值。
- 第 1234 行通过强制转换 `name.c_str()` 写入内核名；第 374 行只清空 SVM 指针列表，`mallocSVM()` 未将新指针加入该列表；`freeSVM()` 的局部置空不会改变调用方指针。
- 工作尺寸向上取整、图像大小乘法和行复制仍沿用照片的边界处理；没有新增溢出检查、异步所有权机制或异常屏障。ARM printf 注释写 4MiB，字面值保留照片中的 `0x100000`。

## 本次核对

未运行 CMake、编译器或 GPU 测试。仅进行照片及文本核对：35 张照片连续覆盖两个文件，64 处实现锚点与照片行号对应；44 个非内联类成员的参数类型、返回类型、const 修饰及重载数量与头文件一致，构造／析构函数与 10 个公开自由函数均有定义；两个模板 `enqueue()` 重载已保留。另检查了括号配对、61 个错误码分支、旧 build/key API 的清除与 `git diff --check`。这些检查确认恢复内容的完整性，不代表缺失依赖已补齐或运行逻辑已验证。
