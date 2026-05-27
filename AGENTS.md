目标: 我需要创建一个高性能基础组件库 Aura, 你作为一位经验丰富的 C++ 开发工程师, 需要遵循以下原则:

## 1 技术栈 (Technology Stack):
- 语言标准: 公开头文件 (Public API) 严格降级至 C++11 以保障极端的编译器兼容性. 内部实现全面拥抱 C++17;
- 构建系统: CMake ≥ 3.10, 使用现代 cmake 语法, 所有 Target 必须通过 target_* 指令管理依赖, 禁止使用全局 include_directories/add_definitions;
- 工具链: CMake (>=3.10, 使用现代 cmake 语法, 所有 Target 必须通过 target_* 指令管理依赖, 禁止使用全局 include_directories / add_definitions);
- GPU 计算：OpenCL / OpenGL 版本选取 macOS 支持的最新版本;

## 2 性能 (Performance):
### 2.1 热路径函数:
- 热路径 (Hot Path) 禁止: 虚函数调用、动态内存分配、类型擦除分配器、无界锁、系统调用 (除非异步);

### 2.2 缓存友好性:
- 容器预分配: 在已知上界的场景下, 调用 reserve() 或使用定长容器;
- 核心数据结构优先采用 AoS (array of struct) -> SoA (struct of array) 转换, 保证同类字段内存连续;

### 2.3 SIMD 与向量化:
- 优先使用编译器自动向量化, 仅在用户明确提出需求后引入手动 SIMD Intrinsics;
- 手动 SIMD 代码必须提供标量回退路径 (Scalar Fallback), 通过编译期宏隔离;

### 2.4 并发安全:
- 互斥锁: 使用 std::lock_guard 或 std::unique_lock (RAII) 管理 std::mutex;
- 原子操作: 计数器等简单变量必须使用 std::atomic<T>;
- 优先使用无锁数据结构 (SPSC/MPMC ring buffer, hazard pointer 等), 所有原子操作须指定 std::memory_order, 注释理由;
- 锁粒度必须最小化, 禁止在持有锁时调用外部代码或 I/O;
- 线程间传递的所有数据必须明确所有权或共享语义, 提倡 std::unique_ptr 转移;

### 2.5 编译期优化
- 必要时采用模板元编程实现编译期优化, 但需写明注释;

## 3 内存 (Memory):
- 生命周期与 RAII: 摒弃裸指针管理 (new/delete), 全面依靠 std::unique_ptr 等智能指针及析构函数接管资源;
- std::shared_ptr 仅用于异步、跨线程共享且生命周期模糊的场景, 且须注释缘由;
- 每个资源类必须实现移动构造／赋值, 禁用或明确删除拷贝;
- 非拥有引用: 优先使用裸指针 (T*) 或引用 (T&) 表达借用语义, 生命周期由调用方保证;

## 4 设计 (Design):
### 4.1 命名空间与模块:
- 组件模块存放到命名空间 au 中, 比如: au::log, au::math, au::cv;
- 优先复用已有功能模块, 减少对底层标准库的直接依赖:
  - au::file: 文件读写和路径操作;
  - au::cv: 图像操作和算子计算;
  - au::json: json;
  - au::math: 数学运算;
  - au::nn: 深度神经网络推理;
  - au::perf: 计时;
  - au::re: 正则;
  - au::log: 日志;
  - au::err: 错误码;
  - au::sys: 平台属性与动态库加载;
  - au::flow: 线程池与 task tile 并发;
  - au::gpu: OpenCL 和 OpenGL 封装;
  - au::gm: 几何 Rect / Coord 相关;

### 4.2 接口设计:
- 最小接口原则: 对外公开 API 只暴露调用方必须知道的内容, 对外接口采用纯函数 + std 依赖、纯虚接口 + Pimpl、沙漏模式 (Hourglass Pattern) 保障 ABI 兼容性, 严禁直接导出带有成员变量的 C++ 实体类;
- 避免过度设计: 抽象引入标准 (满足任一即引入接口):
  1. 存在 ≥ 2 个具体实现 (如 OpenCL / CUDA 双后端);
  2. 需要隔离 I/O、GPU、系统调用等副作用以支持测试;
  3. 跨模块边界需要解耦;
- 跨端物理隔离: 针对不同底层架构 (Qualcomm、MediaTek 等) 及操作系统 API 的特化代码, 必须隔离在 au::sys 中;

### 4.3 设计模式:
- 算子层／核心计算: 使用 Data-Oriented Design, 优先结构体与自由函数, 避免深层继承;
- 组件间优先通过依赖注入或消息队列解耦;
- 优先组合, 减少复杂继承;

## 5 安全与正确性 (Security & Correctness):
- 成员函数默认可加 const，修改状态时提供非 const 重载或 mutable 必须注释;
- 边界与溢出防御: 所有的图像/张量运算在执行前, 必须完成严格的步长 (Stride)、通道数与内存边界校验, 阻断任何形式的越界读写;
- 无异常跨界: 内部错误通过错误码 (au::log & au::err) 传递, 严禁任何 C++ 异常跨越 Aura SDK 的 ABI 边界抛出;

## 6 测试 (Testing):
- 添加单元测试时, 按模块功能分门别类, 要尽可能多的考虑边缘 case, 覆盖要全面但是 test 个数尽量减少;

## 7 规范 (Conventions):
- 成员变量名以 m 开头;
- 公开头文件路径: inc/;