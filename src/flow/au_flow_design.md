# au::flow：纯控制流调度器

调度核心基于 master `182b4789c0616fd6bdc62aeac6f9c2e6130ebebb` 重写，范围进一步收敛到 `e6ab9b85f537d2151ff04a4f85bc341637a4ad4c` 之后的纯 flow 变更。目标是每帧几十个 ms 级任务、动态 DAG 与同步 CPU tiled，不引入内存池、外部 token、隐藏结构化子任务或通用插件框架。

## 文件与依赖

- `inc/flow/xflow_api.h`：纯 C99/C++11 的不透明句柄、函数指针和整数 ABI，版本 **3**。
- `inc/flow/xflow.h`：C++11 header-only `Flow/Task/Tasks`、int/void 归一化、参数所有权与链式构建。
- `src/flow/xflow_core.h`：私有数据与声明；`xflow.cpp`：全部 C++17 调度实现与 C 入口。

两个内部文件按仓库 `.clang-format` 排版后合计 **778 行**，包括声明、注释、空行和平台分支。原先 497 行的紧凑排版按规范展开，没有新增调度逻辑，也不将逻辑搬到其他模块。没有 slab、闭包池、无锁队列、故障桩或额外执行器文件。flow 唯一的 Aura 运行时依赖是 `au::perf`；`sys/xvisibility.h` 只提供 C 兼容可见性宏。亲核原生调用按本次要求内聚在实现文件。

flow 与 mm/gpu/cv 的实现**双向无依赖**。GPU/MM/CV、既有 SYS 实现及 test_xmemory.cpp 恢复为 `e6ab9b8` 的原文件，不替这些模块引入新语义。原 gpu_helper 仍按该基线保留，不加入 Aura 的默认构建。资源访问、设备完成、内存释放由业务自行协调。

此前 `4438d46` 为 flow 引入内存租约，`64f43ab` 又扩展了 OpenCL、配对 CPU 访问和外部内存注册。`ccf8107` 虽已解除 flow 的代码依赖，但保留了这些独立功能；它们不是纯调度所必需。本次撤回这些扩展及配套测试，而不是只删头文件留下半套实现：

- 删除 inc/gpu、src/gpu、mm/xaccess、cv/ximage_access、MM access/memory_control 及其 SYS 平台支持，共 33 个新增文件。
- 恢复 26 个既有文件，包括 MM registry/backend、CV image、gpu_helper 与其加载器生成脚本，避免转发头指向已删除 API。
- 保留 sys/xvisibility.h：纯 C ABI 无法包含带 STL 的 xsystem.h；它只提供符号可见性宏，不引入 SYS 运行时。
- 公开头仅保留 xflow_api.h 与 xflow.h，不提供旧 include 路径。测试仅保留 test_xflow.cpp，不单列语言消费端目标。CMake 保留 Threads::Threads、Android log、测试开关、目标语言标准及 sanitizer 修复。
- 移除 ENABLE_OPENCL、GPU/MM 测试桩及 aura_testing 副本，单测直接链接生产 aura。

这是范围回退，不宣称旧 GPU/MM 已具备撤回的新功能；需要新访问/完成 API 的调用方须迁回原接口。这些功能如需重新引入，应作为独立 GPU/MM 迭代评审与验收。

## 执行、所有权与错误

`Flow::get()` 是无状态外观；调度 Core 在第一次使用时初始化，初始 worker 数为 0。仅创建 callable/Draft 不启动线程。C++ 参数 decay 后复制或移动，`std::ref/cref` 明确借用；支持普通函数、成员函数、int/void、move-only 参数。存储的 callable 和参数只调用一次，以移动值传递。C++11 消费端拒绝超过 max_align_t 的过对齐闭包，C++17 消费端可使用标准对齐 new。

C 句柄使用小型原子引用计数包装，内部 Node 用标准 shared_ptr：C 句柄、已接受工作和 DAG 前驱共同拥有节点。successors 为 weak_ptr，predecessors 为强引用；这样既保留完成后的 DOT 元数据，也不会形成双向强引用环。未提交图丢弃后可回收。payload 使用标准 unique_ptr 和调用方 deleter，在主体或短路结束后、发布 Done 前于锁外释放，不等句柄析构。原始 C 句柄必须有存活引用，不承诺识别伪造或已 release 的地址。

普通任务返回恰好 int 或 void；void 归一化为 0，任何非零整数都失败。前驱失败时，后继依赖计数归零后进入 ready 队列，由消费者直接跳过主体，继承原错误并继续广播，不递归执行长链。多个失败取最先观察到的非零值，不承诺并行失败的固定优先级。

`wait(task/group)` 返回目标的错误；Draft 返回 NotReady 或已保存的构建错误。`result()` 不阻塞，未终结返回 NotReady。`waitAll()` 等待已接受任务全局排空，返回自上次排空屏障以来的首个完成错误并清零；持续提交者可以延长屏障，不是旧版根快照语义。没有接受的工作时返回尚未消费的错误或 0。

**不再隐式 join 子任务、GPU 或任意外部事件。** 任务内 enqueue 的新任务是独立任务；父主体返回不保活局部变量。调用方应显式等待或转移真实 owner。GPU callable 若需要把设备完成作为 DAG 边的条件，应显式完成并返回错误，例如：

```cpp
// gpu 已在 SDK 的资源管理层初始化。
auto task = flow.enqueue([&]() -> int {
    const int code = launchGpuWork();
    return code ? code : finishGpuWork();
});
```

CPU/内存访问错误同样由业务显式返回，不自动写入 Task。调用方必须在 SDK 卸载/资源 deinit 前等待相关任务、设备工作和所有外部调用返回。

## DAG 与簇

每个 Node 有 Created/Pending/Ready/Running/Done 状态、依赖计数、构建首错、依赖错误、结果、名称和微秒耗时。

建边先取得 Core 图协调 mutex，再用 scoped_lock 同时取得前驱/后继 mutex。后继必须 Created。对前驱的 predecessors 做迭代 DFS；若能到达后继则拒绝，等价于在正向图中检查后继可达前驱。分配在修改前完成；失败保留构建错误。重复边无害。

- 前驱未完成：安装 weak successor，后继计数增加；前驱与后继的双节点锁消除检查与挂载之间的完成竞态。
- 前驱已完成：不安装等待边，立即继承其非零错误。
- 完成：在节点锁内设置结果/Done 并移出 successor 列表；逐个减少后继计数，Pending 且归零时发布真实 ready 项。

图协调 mutex 还序列化拓扑与接受操作，防止多个互不共享端点的建边同时通过 DFS 后形成环。Node mutex 保护公开状态查询；统一 CV 在同一协调锁下检查任务完成、就绪工作和 tile，避免跨两个 CV 拼接丢通知。不额外保留一个无人等待的 node CV。

Task 可以直接 enqueue，也可 create 后编排。簇以两个普通内部 gate 节点表示入口和完成，成员边为 entry→member→exit，因此空簇、task→group、group→task 和 group→group 都复用同一完成协议。成员只属于一个 Draft 簇；已接受任务可加入一个或多个新簇作汇聚，但不能追溯增加它的执行前置条件，含这类成员的簇拒绝新增 incoming 边。

enqueue 簇先检查全部 Created 成员及前驱、预分配接受记录并确保消费者可启动，再一次安装状态。Created 的外部前驱必须已接受或同批提交，不能留下永远无人提交的 Pending。重复 enqueue 已接受/完成的 Task 或簇幂等成功，不重复计数或执行。入队返回值必须检查；未接受的图仍是 Draft。

```cpp
auto& flow = au::flow::Flow::get();
auto a = flow.createNamed("decode", decode);
auto b = flow.createNamed("filter", filter);
int code = (a >> b).code();
if (code) return code;
au::flow::Tasks graph("frame");
code = graph.add(a, b);
if (code) return code;
code = flow.enqueue(graph);
return code ? code : graph.wait();
```

支持 `enqueueAfter(task/group/initializer_list, fn, args...)`，包括正在执行和已完成的前驱。C++ 便利入口保存提交失败，使返回 Task 的 wait/result 可报告拒绝。

为修复给定示例忽略返回码的问题，`precede()` 和 `Tasks::add()` 返回 int；`>>` 返回轻量临时 Link，其 `.code()` 保存整条链首错并支持混合 Task/Tasks 链接。Draft 上的构建错误也会阻止提交；已经接受目标的建边错误只属于本次构建结果，不污染已执行任务。不能无条件丢弃链式结果。

## 单池双轨与同步 tiled

池只有一组最多 8 个 std::thread。普通 ready 项使用受 mutex 保护的简单侵入式 FIFO，链只借用 accepted list 拥有的 Node；接受后入队无需分配、无需第三方队列算法。普通提交在无 idle worker 时最多新建一个；分派后若仍有 backlog，会继续按需补一个，避免批量通知尚未被处理时低估需求。线程创建失败不发布半个提交。

高优先级是栈上 TileJob 链表，不创建 Task、future 或每 tile 队列项：

1. range=0 不调用回调；tile=0 归一化为 1；num_tiles<=1 直接在当前线程执行。
2. 多 tile 登记 Job，最多唤醒/创建三个 helper；总 worker 数始终不超过 8。
3. caller 立即抢 tile 执行。空闲 worker 优先协助 Job，每 Job 的 helper 数最多 3，故每循环最多 caller+3=4 个参与者。
4. 所有 worker 忙其他任务时，caller 可独立处理全部 tile。嵌套 tiled 在同一线程串行退化，不需要占用新 worker。
5. caller 完成后在协调锁下摘掉 Job，等待全部已登记 helper 退出，随后才允许栈和回调引用销毁。Core 同时登记整个同步调用的生命周期，退出时排空 Job 和普通任务再 join workers。

原子游标用 relaxed CAS 领取唯一票号，替代无边界 fetch_add，防止 SIZE_MAX 范围的终止领取溢出。区间为 `[begin,end)`，用 range-begin 计算尾块，避免加法溢出。helper 计数与 Job 链由协调 mutex 共同保护；结束时同一锁/CV 发布 callback 写入，无需把游标当内存发布屏障。

tile 回调必须可并发重入并使用 CPU 计算；普通任务与 tiled 均可嵌套调用同步 tiled。tile 内不允许等待其他 Task/全局屏障，返回 WouldDeadlock；不禁止异步 enqueue，但它独立存在、不会被同步循环隐式 join。C tile 回调与 deleter 必须不抛异常，违反此契约终止，不能静默报告完成了部分计算。tile 的 void ABI 不收集 int 返回值；需要计算错误时由业务收集并由外层 Task 返回。

新宏按给定接口内置 `for (size_t i = start; i < end; ++i)`，QUOTE 引用捕获，ASSIGN 值捕获。旧 offset/count 回调与 `_i/_ti` 宏体需迁移，纯转发 include 不代表旧行为兼容。

## 协作等待与锁边界

等待线程（包括外部调用者）可以主动处理 ready 工作或协助 tile。任务进入 wait 时发布临时 waiting 边；协作执行期间保留 executing 边。检查目标的依赖/wait/executing 可达性，拒绝自身、间接等待环与协作栈环；异常退出由 RAII 恢复等待边。协作深度上限 128，超限返回 WouldDeadlock。

任务内部的 waitAll 会包含调用任务自身，必须拒绝，不能靠 work helping 宣称绝不死锁。用户 mutex、阻塞 I/O、外部事件和不可重入 callable 仍可能造成应用死锁；调度器只保证合法调度依赖和纯 CPU 嵌套 tiled 的进度。外部 wait 可能执行别的 callable，调用时不可持有这些 callable 需要的锁。

锁顺序统一为 Core mutex→一个或多个 Node mutex；状态读取可以只取 Node mutex，不得反向调用 Core。用户函数、payload deleter、最后 owning 引用释放、原生亲核调用和 worker join 都在协调锁外。队列项通过 mutex 发布；handle retain 是已有引用上的 relaxed 增计数，最后 release 用 acq_rel 收集先前 owner 写入。全部 C 入口 noexcept，普通任务意外异常映射为 Exception/NoMemory，异常不越过 ABI。

## 亲核性与可观测性

Linux/Android 下捕获初始允许 CPU 集合；Default 恢复完整集合，包括编号>=64 的 CPU。Performance/PowerSaving 从可表示的 0–63 号允许 CPU 读取 cpu_capacity，选最大/最小组；缺少容量时使用统一容量，不猜 CPU 编号。CustomMask 非空且必须是初始允许集合的子集；uint64 接口无法表达>=64 的自定义 CPU。

setter 发布 mask/policy 后 release 增加 affinity_gen；worker 在每次分派（含协作执行与 tile）前 acquire 检查并锁外 sched_setaffinity。配置在应用期间换代则重查，避免首次任务使用旧策略。原生失败在下次分派重试；getAffinityMask 是请求掩码，不是所有 worker 已应用的同步承诺。外部帮助线程从不套用 worker 亲核策略。非支持平台 Default 成功降级，非默认策略返回 AffinityFailed。

命名的真实 callable 自动配对 au::perf::traceBegin/traceEnd，使用现有 XTracer 实现及开关，没有 flow trace 缓冲。costUs 是 callable 墙钟时间，包含内部等待，不含排队时间和 payload 析构，不等于 OS CPU 时间。

Tasks::dumpDot 返回包含名称、微秒耗时、状态、错误及依赖边的 Graphviz 文本，包含所需前驱闭包。字符串正确转义，节点以进程内地址区分同名；它不是跨进程稳定 ID。结果存于每线程字符串，直到该线程下次 dump；无需文件 I/O、历史捕获缓存或额外保活执行对象的诊断树。

## 迁移、取舍与验收

这是明确的 ABI/API 重写：删除 au_flow_*、XFlow 外观、TaskResult/ErrorDomain、worker 预算/退休/查询、隐式后代与 GPU 完成契约；改用 XFlow_* ABI 和 Flow。不能与 ABI 2 头/二进制混用。默认 2 秒线程退休不再保留，本版仅承诺初始 0、惰性增长及退出 join；无后台定时轮询。

flow 测试收敛为 test_xflow.cpp 的十组功能/并发测试；保留基线全部 Aura 单测。随扩展引入的 GPU/MM 测试与功能一并撤回，未通过关闭仍存在的模块获取通过。CTest 仅注册 aura_unit，运行整个 Aura 单测，而不只是 flow 过滤子集。

验证命令：

```sh
cmake -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
# 可选：-DENABLE_ASAN=ON 或 -DENABLE_TSAN=ON
```

最终主机验收（2026-10-21）：

| 配置 / 检查 | 结果 |
| --- | --- |
| 本轮 Debug：configure / build | 通过，编译日志无 warning/error；并行数按环境设为 4 |
| 全部 Aura 单测 / CTest | 110/110；1/1，含恢复的基线内存测试 |
| ASan/LSan | 1/1，detect_leaks=1:halt_on_error=1 |
| TSan | 1/1，halt_on_error=1 |
| 严格语言边界 | C99 / C++11 / 内部 C++17，-Wall -Wextra -Werror -pedantic 通过 |
| 核心体积 | 按 .clang-format 排版：xflow.cpp 675 行 + xflow_core.h 103 行 = 778 行 |
| 文件与依赖 | src/flow 仅设计文档及两个内部文件；无旧 flow 宏/头/运行时依赖 |
| 范围隔离 | GPU/MM/CV、既有 SYS 实现及内存测试与 e6ab9b8 无差异；仅新增 SYS 可见性头 |

Android/其他平台与业务端性能未在本机测量；主机 sanitizer 通过不代替设备验收。OpenCL 扩展已撤回，原扩展的 5/5 测试结果不作为当前树的验收结果。本轮不恢复过程基准和大批原始验证档案。
