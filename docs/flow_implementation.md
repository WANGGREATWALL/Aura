# au::flow 当前实现

更新：2026-10-21（Asia/Shanghai），B7 工作树基于 dev `f08c09b`。当前接口为 C ABI **v2** / C++11 facade，私有实现 C++17。完整问题、批次和验收状态以 [整改清单](flow_remediation_plan.md) 为准；用法与迁移见 [公开接口](flow_migration.md)，同步约束见 [锁关系](flow_locking.md)。

## 1 范围与依赖

flow 是 Aura 的任务调度模块，可以使用 au::log/perf/file/sys 等基础能力，不单独建立产品包或通用 host 层。核心不包含 au::gpu 实现头、OpenCL 类型或驱动调用；`completion.h` 的 token/query/finalize/release 协议支持外部完成，由 GPU 提供方适配。错误来源枚举中的 OpenCL 标签只是结果数据，不构成实现依赖。

`execution_context.*` 保留小型 mm 错误桥、mayWait 上下文和 registry 寿命顺序。它服务既有访问安全契约，不为了“独立”新增运行时注入框架。公开 flow 头不包含 mm/GPU/perf/file 实现头。

B7 按用户决定删除 flow trace、DOT 导出、图历史、任务命名、CPU slice 及自动 GPU profiling 上报，包括实现、API、配置和专用测试。实际 DAG 调度照常保留。诊断接口删除是兼容性变更，ABI 升为 2，需重编消费者；无保留空实现。业务仍可使用 au::perf 和外部工具。

## 2 状态与职责

| 文件 | 职责 |
| --- | --- |
| xflow.cpp / 公开头 | 窄 C ABI、异常边界、C++11 callable/句柄外观 |
| node.* / slab.h / payload_pool.h | 侵入式引用、迭代回收、Node/Edge/External 记录池与一档小闭包存储 |
| graph.cpp / graph_search.* | Draft 编辑、原子接受、拓扑校验、borrowed children 与无分配遍历 |
| task_completion.cpp / task_errors.* | 完成传播、reservation、失败来源与确定性优先级 |
| executor.cpp / sys/flow_runtime.* | 工作队列、统一睡眠、协作等待、worker/亲核策略、退休与关闭 |
| external_completion.cpp / external_token.h | 通用外部查询与正常线程清理、单周期查询者 |
| tiled.cpp / execution_context.* | 轻量同步 Job、caller/helper、错误归属与上下文恢复 |

Core 保留协调共享不变量的状态，没有新增虚分派、Pimpl 或各自关闭的子调度器。测试 seam 只编入 aura_testing，生产 aura 不编译 testing.cpp。

## 3 执行与生命周期

- 普通 callable 恰好返回 int，0 成功、任何非零失败。提交只负责接受，任务内新接受任务默认归属当前父任务；最终完成等待主体、后代和外部清理，不延长主体 C++ 局部变量寿命。
- Task 复制/移动维持拥有引用；已 release 的原始 C 指针不是可安全探测的输入。owner 检查不承诺检测悬空句柄 ABA。
- Draft 强引用拒环防止从未提交的图泄漏；批量提交 Kahn 验证、结构化祖先与运行期等待环各自保留。活动 children 只借用 accepted 节点，图锁内安装/摘除；等待搜索一次匹配整个协作栈，结束前 RAII 清除 scratch。
- 等待环/深度/非法上下文检查在所有构建生效。C wait 返回操作状态，结果错误在 out；C++ wait 返回任务错误，超时等不改 out。waitAll 快照调用时的异步根及后代，不汇总历史业务错误。
- 外部同步 tiled 不进 waitAll 快照。shutdown 的 Stopping 裁定与 Job/工作接受线性化；排空 caller、helper、任务、查询/清理后 join。排空不代替 SDK 调用方确认外部调用栈全部返回再 dlclose。

## 4 tiled 与等待协议

同步循环使用栈上 Loop 和专用固定 helper 接受路径，caller 参与，最多四参与者；嵌套/深栈/单参与者串行退化。helper 每次最多处理八个 tile 后让出，最小失败 offset 决定错误；专用 join 不因普通 wait 检查失败而提前释放 Loop。

所有 tile 路径安装 TileContext，禁止异步任务/GPU 发布，允许 Draft 编辑和嵌套同步循环；违规进入循环错误，void 回调忽略拒绝也不能伪成功。协作执行其他任务或清理时保存、清除并恢复 tile 上下文。B7 删除无逻辑读取的 gTileDepth。

工作许可只对应真实项，控制世代独立；同一 mutex/CV 的谓词覆盖工作、控制和期限。工作 notify_one，控制 notify_all，关闭不伪造许可。单周期查询者只在有在途工作时按 200–2000 µs 退避，交接/预算变化事件通知；预算外 worker 不周期复查。默认两秒空闲退休到零，稳定槽位/代际与 query pin 防止 TID 复用竞争，在途工作保留推进者。

B7 将 helper 项全部发布后一次 post/grow；同时修复 ready-pop 到 busy 登记的漏扩容窗口。ready/busy 增加与需求读取用 seq_cst，worker 登记 busy 后再检查 grow，已有容量走原子快速判断；减少仍 relaxed。该协议不代替队列数据发布或槽位寿命同步。旧版确定性失败与修复证据见 B7 报告 X15。

## 5 外部完成与 GPU 边界

提供方在 native enqueue 前 prepare，registrar 最后离开后才允许释放 payload。poll 失败不证明设备已停；只有确定终态才 finalize/release，parent reservation 最后释放。提供方代码、用户析构及驱动调用均在调度锁外。没有原生 CL 回调用于证明卸载安全。

GPU backend 和 pending registry 自行持有事件、资源租约、staging 与 fence。flow 只看到不透明 payload 和函数表；不记录 GPU 时间戳或额外保留 profiling event。原生 queue profiling 开关独立保留。

普通 Task 边等待完整完成，A 设备完成到 B 入队有查询、主机清理与调度间隙；不包含 A 自身执行时长。同一任务内连续 native enqueue 可减少该间隙，是否能走原生事件链仍受 context/staging/资源别名约束。本版不新增设备边、`.gpu()` 提示或自动大小核分类，继续显式 CPU 集合。

## 6 验证与测量

`ENABLE_FLOW_BENCHMARKS=ON` 构建生产测量工具：microbenchmark、idle、mechanism、gpu_gap、payload。测试开启时额外提供 instrumented graph_benchmark，测内部图搜索与锁成本，不输出 DAG 图。没有 flow trace/DOT 专用工具。

B0–B6 报告保留为历史证据，其诊断 API、ABI v1 和当时的实现描述已被 B7 范围调整取代。最新主机结果、净源码量、产物体积、原始性能与 43 项状态见 [B7 报告](flow_validation/B7-2026-10-21.md)。R01/X13 性能仍需受控复测；目标 NDK、QCOM/MTK、真实 GPU、FlashPE 帧 p50/p95 与卸载由用户后续验收，未运行项不记通过。

算法参考：Taskflow 3.11.0 固定 `816b4ad53b44196c88f409eb7b4a25a0e3bfdf42` 的 sequence ring 与 dependent-async 思路，以及 Vyukov/gRPC MPSC。未链接 Taskflow/gRPC；许可见 third_party/flow_queue_references。B6 可比 FIFO 对照后保留 ring/MPSC 与记录池，不声称整套调度器 wait-free，也不据 x86 TSan 推导 ARM64 正确。
