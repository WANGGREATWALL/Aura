# au::flow 私有状态、锁关系与调用边界

本表更新至 B6，前序 B4 起点 `f0f523c`、B6 起点 `e343789`。它描述当前实现的同步协议及审查结果，不把 TSan 通过当成并发证明。C ABI、C++11 外观和普通任务结构化完成语义不变。

## 状态所有者

| 实现 | 所有状态 / 职责 |
| --- | --- |
| `core.h`、`node.cpp` | Core 的共享协调状态、存储与寿命；每个模块有自己的实例，mm registry 先构造、后析构 |
| `graph.cpp`、`graph_search.*` | Draft 编辑、提交接受、拓扑校验、快照；借用的活动子边与临时遍历状态 |
| `task_completion.cpp`、`task_errors.*` | 预留计数归零后的完成传播；错误候选选择及其 mutex |
| `executor.cpp` | 就绪分派、协作等待、worker 槽位、策略、退休和 shutdown |
| `external_completion.cpp`、`external_token.h` | 外部 token、待查询列表、周期查询身份；不持提供方资源所有权以外的原生回调 |
| `tiled.cpp` | 栈上 Loop、轻量 Job、helper 接受/收尾；所有参与者返回后才能销毁 Loop |
| `execution_context.*` | 唯一的 flow/mm 集成桥；TLS 的保存/恢复、错误路由、cleanup 的不可等待范围 |
| `core_diagnostics.cpp`、`diagnostics.*`、`graph_history.cpp` | 值历史、调度快照与可裁剪时间线窗口；文件输出在调度锁外 |
| `testing.*`、`testing_internal.h` | 测试专用故障/时序 seam 与计数；正常 aura 不编译这些实现 |

Core 仍负责跨职责不变量的协调；没有新增虚接口、全局注册框架、共享所有权图或单独分配的 Pimpl。文件拆分不代表存在多个可以独立关闭的调度器。

## 允许的嵌套关系

这是一张偏序表，不要求互不相关的锁硬排成一个数字序列。

| 外层 → 内层 | 用途与限制 |
| --- | --- |
| `mCloseMutex` → graph 或 worker mutex | 串行化多个 shutdown 调用。等待 drain 时释放 graph mutex；join 时不持 graph/worker/gate/provider mutex。任务或 cleanup 重入 shutdown 在取得 close mutex 前拒绝 |
| `mWorkerMutex` → `mMutex` | 退休裁定：同时检查 eligible consumer、admission、accepted Task 与查询 pin。禁止反向 graph → worker；提交先取得/释放 admission 锁，再进入 graph 锁 |
| graph / worker / external mutex → `WorkSemaphore::mMutex` | 发布控制世代或真实工作许可。gate 谓词只检查自身计数/世代，不反向取得 Core 锁 |
| 多个 mm `mAccessMutex` | 按 allocation identity 升序取得、逆序释放；不是按地址或调用方传参顺序 |

其他 Core 锁组合原则上不嵌套。Node/CompletionState 的等待 mutex 在发送控制通知之前释放；TaskErrors mutex 不包围其他 Core 锁、其他节点错误 mutex、驱动或用户代码。

`mPolling`、Pending 的 `mQuerying`、就绪队列的 consumer 标记是 try-acquire 的原子独占协议，不是持锁等待的 mutex。周期查询/清理持有其原子独占身份时可以进入完成传播；反向路径不等待这些身份。若以后将它们改成阻塞锁，必须重新审查偏序，不能套用本表。

## 各类锁保护什么

| 锁 | 保护内容 | 锁内禁止内容 |
| --- | --- | --- |
| Core `mMutex`（graph） | Phase、accepted/Job/root/order、活动链表、Node parent/children、wait/executing 边、接受时安装的诊断记录、遍历 scratch | 分派、协作等待、native join、GPU/mm/provider 调用、Closure/最后一个 owning handle 的用户析构 |
| Core `mWorkerMutex` | worker 槽位状态、代际、TID pin、admission/joiner、策略快照和退休裁定 | join、驱动、用户代码；`startWorker` 显式解锁再 join，随后重新检查预算/关闭 |
| Core `mExternalMutex` | 待查询 token 链表及长度更新 | poll/finalize/release；先摘链，解锁后才调用提供方 |
| TaskErrors `mMutex` | body/scoped/inherited/Skipped 候选和选择规则 | 日志、资源释放、回调、其他节点错误合并；每次只锁一个节点 |
| Node `mWaitMutex` | 终态发布与协作等待者登记，配合原子的 TaskState | 协作执行、graph 检查、native join、control 通知 |
| WorkSemaphore `mMutex` | 真实队列项许可、控制世代、进入睡眠的谓词检查 | 任何反向调用 Core 或提供方的等待谓词 |
| Node/Edge/External 三类 slab、一类 64 字节 payload slab 的 mutex | 各自空闲槽位链表与页安装 | 对象构造/析构、用户 deleter；Slab 先解锁再 placement new，先析构再回收槽位 |
| Diagnostics `mMutex` | 串行化 enable/disable/reset；首次准备和原子安装窗口 | 文件 I/O、调度、驱动；record/dump 以 atomic shared_ptr 操作 pin 窗口，旧窗口析构在控制锁外 |
| GraphHistory `mMutex` | 链头发布、每条值记录的状态/code/cause、ID 查询 | Core 锁、用户代码、I/O；snapshot 在锁内只取得不可变链头/可变完成字段，字符串/容器复制在锁外 |
| GPU PendingRegistry `mMutex` | 在途记录链表、drain 领导者、revision、累计失败 | query/finalize/原生资源 release；`pollSome` 持有 shared pin 后解锁，drain 在锁外查询 |
| GPU CompletionState `mMutex` | done 发布与 fence waiter 登记 | 驱动/资源销毁、Core 协作分派或通知；通知在解锁后发送 |
| mm Registry `mMutex` | 地址/身份表与 lease 快照 | 用户资源释放、fence wait、调用调度器；移出的记录和旧 lease 在锁外释放 |
| mm `mAccessMutex` | 资源访问版本、CPU/device 预留和 fence 快照 | 驱动 cache START/END、fence wait、调度器错误 sink；先解锁，再等待或报告 |

Draft 的 `precede/add` 现在也取得 graph mutex，因为遍历 scratch 可经过共享的 accepted 前驱。调用方仍须串行化**同一个 Draft** 的编辑、命名、提交、abandon 和 handle 销毁；这次改造没有把整个 builder API 变成可以无条件并发使用的对象。

冷路径仍有分配，例如 Draft 边容器增长、DOT 快照和策略复制。本表没有宣称所有锁内代码都无分配；B4 消除的是环/等待搜索的临时分配与全局活动扫描。图的所有权边、OOM 回滚和提交检查依然保留。

## 函数前置条件与释放边界

| 函数 / 范围 | 调用约束与取得的锁 |
| --- | --- |
| `make/recycle`、NodeRef 最后释放 | 入口不持 Core mutex；池内部自行同步，回收迭代栈避免深递归析构 |
| `builderError`、`reject` | Draft 由调用者串行使用；reject 必须在调度锁外，因为会清理 Closure。提交失败在 graph guard 析构后才 reject |
| `precede/add`、`submit` 接受阶段 | 内部取得 graph mutex；所有 GraphTargets/GraphSearch 都在同一 guard 内构造和销毁，不允许嵌套搜索 |
| `GraphSearch`、`GraphTargets`、`linkChild/unlinkChild` | 调用者必须持 graph mutex。搜索不得执行回调/释放引用；child 链只借用 accepted 节点，不增加强引用 |
| `wait` 的检查阶段 | graph 锁内一次遍历同时检测当前协作栈目标；祖先直接等待检查保留。解除 guard 后才进入 gate/Node CV 或协作执行 |
| `waitAll` | graph 锁内取得水位与 root pin；reserve 在锁外；等待 root 在锁外。调用时的根快照语义不变 |
| `finish/bodyEnd/resolve` | 不持 Core 锁进入。错误选择、终态发布、后继传播、活动/子链摘除是分离的阶段；最后释放与析构在 graph 锁外 |
| `prepareExternal` | slab 分配在 graph 锁外；graph 只裁定 reservation/order。失败时 token/parent ref 的销毁也在 graph 锁外 |
| `consumeExternal/pollPendingExternal` | 提供方 poll/finalize/release 在 pending-list 锁外；cleanup 上下文禁止发布新工作或等待未完成对象 |
| `helpOne/run`、`joinHelper` | 入口不持 graph/worker/provider mutex；可以执行任意用户函数。helper join 不会因检查/分配失败提前释放栈上 Loop |
| `WorkerAdmission::accept` | 内部取得 worker mutex；扩大存活资格覆盖接受与发布。时序钩子在解锁之后调用 |
| `eligibleWorkers/updateRetirementPending` | 调用方已持 worker mutex；不自行再锁 |
| `startWorker` | 传入已持有 worker mutex 的 unique_lock；join 时暂时释放，恢复后重新裁定状态 |
| `retire`、`releasePoller` | 退休内部 worker → graph；释放查询者身份与退休通过 worker mutex 串行化，通知在释放 worker mutex 后发送 |
| `applyPolicy/affinity/workerAffinity` | worker 锁内复制策略或 pin 槽位，原生亲核读写在锁外。query pin 退出前不得退休/复用该代际 |
| `prepareCapture`、history publish/update | 预分配和结构复制在 graph 锁外；接受时安装 CapturedNode，提交 latch 释放前发布批次。History 锁不嵌套 Core/TaskErrors/wait 锁，不保留执行 Node 引用 |
| `dot/trace/tracing/resetTrace/dumpTrace` | DOT pin 当前合法句柄与共享值历史；静态依赖在接受后冻结，Draft 仍由调用者串行化。导出/I/O 在 Core 锁外；时间线 reset 需关闭，旧读写通过共享 pin 收尾 |
| `ExecutionErrors`、`TileExtent/CleanupExtent` | 保存/恢复 TLS；mm sink 按上下文写本线程 tile 错误或取得该 TaskErrors mutex，不取得全局 graph mutex |
| 测试 `runtimePoint/runtimeHook` | hook 调用不持 Core 或 seam mutex；注销等待已进入 hook 的读者退出。hook 不能从自身同步注销自己 |

## 与 mm / OpenCL 的审查边界

审查了 `mm/access.cpp` 中 CPU begin/finish、DeviceAccess begin 和 legacySync：资源预留/快照之后先 unlock，再等待 fence、执行 native cache 同步或报告错误。`reportAccessError → ExecutionErrors → scopedError` 不带入资源锁；原先担心的 mm → TaskErrors 路径因此没有对应的 TaskErrors → mm 锁反向边。

`mm/registry.cpp` 的 acquire 将旧输出 lease 的替换放在 registry 锁外；retire 先 extract 再释放控制块。`gpu/pending.cpp` 的查询、清理和记录退休先后通过自己的原子 query gate 串行，registry mutex 不跨越驱动或 cleanup。CompletionState 的通知在销毁 payload 后发布。

OpenCL 的 backend lifecycle、缓存和 kernel 空闲链锁也纳入边界检查：entry 只保留存活资格，不持 lifecycle mutex 进入 Core；`au_gpu_enqueue` 在缓存/资源准备函数返回后才 prepare token。backend stop 等待 entrants 后解锁，再 flush/drain；共享原生资源的最后释放靠锁外 pin 保证。这里不改 OpenCL 的设备排序语义，也不声称审计了第三方驱动内部的锁。

B4 同时把 `prepareExternal` 的溢出回滚移到 graph guard 之外，即使该 parent 当前仍由调用栈/accepted 引用保活，也不依赖“这次大概不会触发最后析构”来满足边界。

## 必须继续保持的检查

- Draft 强引用图的拒环检查防止从未提交的对象泄漏；提交仍用 Kahn 检测包括簇/入口/父完成关系的批量环。
- 提交检查外部前驱是否到达结构化祖先；等待检查显式 wait、协作 executing、活动 children 及强引用图的间接环。没有改成仅自身/直接祖先或 debug-only。
- child 邻接在接受的 graph 临界区接入，在 finish 的同一临界区摘除后才移走 parent ref；accepted 引用和 parent reservation 是借用链可用的依据。
- 遍历借用每个 Node 的 next/queued/target 字段；无论提前找到环、失败还是正常完成，RAII 在解锁前恢复。它不是并行图搜索 API，也不能从搜索内重入。
- 完成态的 release/acquire、计数归零的 acq_rel、队列/控制发布协议、slot generation/pin 与 dlclose 外部调用返回边界均保持 B3 契约。

验证覆盖图/组/交叉 wait/协作栈、失败与资源来源、深链回收、动态注册、双 so、关闭、ASan/LSan 与 TSan。命令、实际通过/跳过结果、图临界区测量及限制见 B4 验证报告；未运行的目标设备不能由本锁表替代。


B6 的历史批次在发布后不再移动或修改结构字段，发布前分配的 CapturedNode 借用其中稳定记录；GraphHistory 所有者覆盖该借用生命周期。历史不含 NodeRef，执行 Node 的释放不会使 DOT 元数据悬空。快照在短锁下取得 head 后由调用者 pin 整个 history，链表只增加头且已发布 next 不变；状态/code/cause 逐条锁读，容器分配与字符串复制在锁外。共享历史可能包含祖先/兄弟，最终导出按请求句柄可达范围过滤。

时间线采用不可覆盖的窗口；atomic shared_ptr 操作提供跨 reset/record/export 的存储 pin，而非声称无锁。关闭热路径不取 pin。reset 只摘下当前窗口，不等待所有旧 pin 或强制回收 OS 页；用户配置调用需要顺序语义时自行串行化。并发回归与 TSan 的通过支持实现检查，不替代上述生命周期协议。
