# au::flow 迭代整改与验收清单

创建日期：2026-10-20（北京时间）。源码起点：`aa3b3ae`（Aura dev）。

本文汇总本次 GPT 与 Claude Opus 评审的 28 项问题、后续 11 项补充讨论及实施中新增的 X12/X13/X14，是后续任务的追踪入口。现有实现和历史测试证据见 [flow_implementation.md](flow_implementation.md)。本文中的计划、设计结论、实际执行和设备证据分别记录，不以设计完成代替代码或验收完成。

## 1 范围、状态与关闭规则

目标负载为 Android 拍照算法 SDK：每帧几十个算子/模块、同步 tiled、异步任务与少量 DAG、OpenCL 衔接。CPU 基础能力继续兼顾 Linux/macOS/Windows，优先保证 Android ARM64 和 Linux。

状态定义：

- `TODO`：未实施。
- `IN_PROGRESS`：正在实施。
- `HOST_VERIFIED`：当前主机验证通过，尚不代表设备验收。
- `BLOCKED_EXTERNAL`：需要本环境未提供的外部源码或输入。
- `USER_VALIDATION`：用户已决定在交付后的目标环境执行，不阻塞本地主机实施；不得记为设备通过。
- `DECISION_RECORDED`：明确保留或不采用某建议，已记录理由；不声称实现了该建议。
- `CLOSED`：所需代码、文档、验证及适用设备证据全部齐备。

每项关闭必须填写：处理结论、代码/文档或不改动理由、复现命令与原始结果位置、对应提交、通过/失败/跳过/未运行、剩余限制。不把“未观察到问题”写成并发证明，不把 skipped 写成 passed。

性能目标不在看到结果后临时改变。B1 先登记主机基线方法与现有可用负载；目标设备、业务帧预算和设备回归阈值由用户后续验收时登记；候选版本超出阈值则修复、回退或以明确范围变更处理。不能仅以代码更短宣布完成。

每轮保持可独立审查的改动；格式、行为、性能优化分开提交。不得通过关闭校验、泄漏扫描、断言或静默缩小验收范围获取通过。未经用户要求，不 push，不改 master。

## 2 已收敛的设计约束

| 编号 | 约束 |
| --- | --- |
| D01 | 保留 C ABI 窄腰、C++11 外观、内部 C++17、每算法 `.so` 独立实例。 |
| D02 | 保留普通异步任务的结构化后代与 GPU/清理 reservation，Task 最终完成不能早于其已开始工作安全结束。普通任务继续要求返回 int；任意非零为失败。 |
| D03 | 保留 Draft 强引用环检测、提交拓扑校验和运行期等待环检查；优化实现，不把必要检查仅放 debug。 |
| D04 | 外部同步 parallel-for 可改为轻量 Job，不作为普通 Task 走通用 submit；Job 覆盖 caller、helper、错误和收尾。Job 接受与 Stopping 必须线性化。 |
| D05 | 新语义目标：waitAll 覆盖调用时异步根快照及其结构化后代；外部同步 Job 不进入快照。managed Task 内的同步循环仍由父 Task 完成覆盖。已在 B2 更新契约和旧测试后生效，见 B1/B2 验证报告。 |
| D06 | tile 回调禁止发布异步任务及异步 GPU 工作；允许创建 Draft/编辑图；嵌套同步 tiled 保持串行退化。合法的已有任务 wait 不默改为禁止，须保留既有等待安全边界。 |
| D07 | 所有 tile 路径（caller/helper/单参与者/深栈与嵌套串行退化）安装可恢复上下文，包含循环错误入口与 tile offset。gTileDepth 可判定，不能单独承担错误定位。协作执行无关任务时保存、清除、恢复完整 tile 上下文。 |
| D08 | 违规发布返回一致拒绝结果并登记到循环；void 回调忽略拒绝也不得使整个循环伪成功。使用 StateInvalid/NotSupported 等明确契约错误，WouldDeadlock 用于实际非法等待。 |
| D09 | 工作计数仅对应真实队列项；控制通知独立计数/世代，与工作使用统一等待入口和同一睡眠同步协议。控制变化广播，单项工作通知一个。shutdown 不再伪造工作许可。 |
| D10 | 仅一个空闲 worker 负责定时 GPU 查询；交接、公平查询及事件状态与睡眠关联。普通 worker 不做 1 ms 周期轮询，超预算 worker 不做 10 ms 复查。每次查询期限变化不广播全体。 |
| D11 | 默认空闲 2 秒退休、无工作时最终常驻 worker 为 0。存在需要推进的 GPU 工作时至少保留推进者；稳定槽位携带代际，安全处理 join、查询 pin 和最后 worker/新提交竞争。 |
| D12 | 不采用算法 `.so` 内原生 CL 完成回调来证明卸载安全。排空调度器不替代调用方确认全部 SDK 调用已返回后再 dlclose。 |
| D13 | 普通 Task 边仍要求完整完成；设备依赖必须是独立、受约束的提交契约，不通过 `.gpu()` 布尔提示偷偷改变普通依赖。 |
| D14 | 已接受目标建边失败只返回错误，不污染前驱或已接受节点。若增加 `>>`，首错由独立链式构建结果保存。 |
| D15 | 不把 XImage 浅借用改成隐式共享/深拷贝；移动借用视图仍是借用。完整保留合法 Task 拥有引用的生命周期。 |

## 3 原始 28 项问题逐条追踪

每项均有归属批次，不以整体重构笼统替代。B6 的实验项允许最终保留现实现，但必须完成实验与书面决策，不能无限期标为“以后再测”。

| ID | 问题与核实结论 | 应对方案 | 验收条件 | 批次 / 当前状态 |
| --- | --- | --- | --- | --- |
| R01 | tiled 根与 helper 重复走通用 submit；容器分配属实 | 轻量同步 Job、固定数量工作记录和专用接受路径，跳过不适用的通用 DAG 容器；保留关闭/错误/收尾约束 | 固定开销及分配测量；无 helper、并发关闭、栈借用和失败降级测试 | B2 行为 HOST_VERIFIED；IN_PROGRESS：多 tile 空循环退化见 X13，B3/B6 继续 |
| R02 | helper 逐 tile 回队；caller 自己处理的 tile 不入队 | 有界批量执行，按竞争/批量让出；批量值通过测量选定 | 区间恰好一次、最小失败 offset、参与者上限、多个循环及普通任务公平性；对比基线 | B2 / HOST_VERIFIED（见 B1/B2 报告） |
| R03 | WorkSemaphore、亲核检查与扩容存在多次锁操作；队列净收益未测 | 策略版本快速判断、按就绪发布/批量需求扩容；与 mutex+侵入式 FIFO 对照，禁止接受后因 deque 分配失败丢任务 | 记录锁/分配/队列等待与 p95；fan-out 无新 enqueue 仍能扩容 | B3 热路径 HOST_VERIFIED；队列对照留 B6 / TODO |
| R04 | 多个 worker 可进入 1 ms 查询，但不必然是所有 worker | 单查询者、统一等待、完整协作 wait 通知；固定间隔与退避对照，standalone drain 另测 | 无丢唤醒、无事件时无周期查询、GPU 查询公平；完成延迟/唤醒数据 | B3 / HOST_VERIFIED（见 B3 报告；设备性能 USER_VALIDATION） |
| R05 | worker 高水位保留，预算外 10 ms 唤醒 | 默认 2 s 退休到 0；GPU 在途保留推进者；查询/退休/重建代际一致 | 超时退休、GPU 保活、并发新提交、最后退出、亲核查询和 shutdown 测试；线程/PSS/首帧数据 | B3 / HOST_VERIFIED（见 B3 报告；设备性能 USER_VALIDATION） |
| R06 | reaches 每访问一个节点扫描 mActive，锁内多次遍历 | 活动 children 邻接关系，合并重复搜索，减少锁内分配；不削弱间接环检查 | 交叉 wait、协作栈、祖先、簇包含、深图等不回归；记录锁持有时间 | B4 / HOST_VERIFIED（见 B4 报告） |
| R07 | 多种环检测具有不同职责，不可按数量删除 | 保留 Draft、提交、结构化与 wait 检查；减少重复搜索；明确各自不变量 | 从未提交的引用环不泄漏；提交/等待环不执行半图或挂死 | B4 / HOST_VERIFIED（见 B4 报告）；删除三套检查不采纳 |
| R08 | Core 职责过多，diagnostics 已部分分离 | 按图/执行器/外部完成/tiled 职责拆分，共享私有状态边界清晰；不设 400 行硬指标，不引入无必要的动态抽象 | 状态所有者和锁边界可审查；行为测试一致；与格式改动分开 | B4 / HOST_VERIFIED（见 B4 报告） |
| R09 | 池收益与高水位驻留未量化；submit 尚有分配 | 普通分配/池化可比实验；记录池页/闭包档位与峰值，决定保留、缩减或移除 | 对齐、OOM 回滚、恰好一次析构、并发复用不变；吞吐/尾延迟/PSS 决策记录 | B6 / TODO |
| R10 | 多套错误候选增加阅读成本，但优先级有业务意义 | 封装内部合并规则，去重复；保留公开 domain/native/cause 和 ABI 布局，不退化为仅日志 | 主体/作用域/后代/Skipped 优先级，业务同码与异常/原生来源测试 | B4 / HOST_VERIFIED（见 B4 报告）；删公开字段不采纳 |
| R11 | 结构化 enqueue 默认行为不够醒目 | 在公开头/示例说明自动归属、非阻塞 enqueue 与最终 join；保留默认，不新增隐式 detached | 文档与嵌套/失败/快照测试吻合 | B5 / HOST_VERIFIED（见 B5 报告与迁移文档）；改默认为独立根不采纳 |
| R12 | 核心直接接入 mm 上下文及初始化 | 评估小型私有上下文桥，隔离集成职责；保留 registry/析构顺序，不新增通用插件注册框架 | CPU 任务路径、mm 错误回传、清理重入、退出及双 so 隔离测试 | B4 / HOST_VERIFIED（见 B4 报告） |
| R13 | trace 首次约 14 MB、高水位记录和诊断子边保留 | 保留 DOT；时间线可裁剪，生产默认不启用；动态历史用轻量快照，不用裸 ID 丢失元数据；是否移除时间线按实际需求作明确结论 | 关闭时无 trace 存储、捕获结束可释放；并发导出/UTF-8/动态 DAG；驻留测量 | B6 / TODO |
| R14 | 默认测试配置会把故障钩子编入 aura；发布开关易误用 | 正常 aura 无故障宏，单独 aura_testing 变体；子项目测试默认关闭并尊重父工程 | 默认/Release/子项目 production 编译命令和符号无测试钩子；测试变体故障用例真实执行 | B0 / CLOSED（主机证据见 B0 报告） |
| R15 | 深度、批量、重试和时间常量分散 | 按含义具名，说明边界和调优依据；不同含义不合并成通用 8 | 边界测试覆盖；查询/退休与调优文档同源 | B2/B3/B4 / HOST_VERIFIED（取值不变，见 constants.h 与 B4 报告） |
| R16 | 仅 Android/Linux 负载不等于取消 Aura 跨平台目标 | 保留 sys 平台隔离，明确支持与未验证范围，不删除 Apple/Windows 分支 | Linux/Android 必验；其他平台明确未测，不能伪称通过 | B5 / DECISION_RECORDED：不缩小平台目标，平台验证边界见迁移文档；Android 与其他平台 B7 / USER_VALIDATION |
| R17 | XImage 按值捕获仍借用，异步迁移有 UAF 风险 | 公开示例明确 owning move 与 borrowed move；资源身份/租约显式适配，不用结束时 registry 检查伪造保护 | 复制/移动/提前丢句柄/在途资源测试；无隐式深拷贝或改变借用语义 | B5 / HOST_VERIFIED（复制/借用移动/拥有型移动/提前丢句柄及访问租约回归；见 B5 报告） |
| R18 | 过期原始 C handle 可指向复用槽位，正常 Task 拥有引用不受此问题影响 | 文档声明 raw handle 生命周期；验证合法引用阻止复用。不承诺失效指针可安全探测；若将来强化需稳定表+generation+pin | 合法复制/移动/释放/复用、跨 owner 检查；不调用悬空句柄来宣称安全 | B5 / HOST_VERIFIED（合法引用/池复用/C11/C++11/双 so；见 B5 报告）；本轮不改句柄 ABI |
| R19 | 缺统一锁顺序/回调边界说明 | 记录锁层级、禁止同时持有关系、函数前置条件；用户代码/驱动/析构在锁外 | 锁审查清单、重入/故障测试、TSan 适用环境结果 | B4 / HOST_VERIFIED（见 B4 报告） |
| R20 | ARM64、QCOM/MTK 真机证据缺失 | 弱内存与进度模型加真机压力；可用时 TSan，无法运行说明替代证据，不能将 TSan 视为正确性证明 | 骁龙/天玑适用设备、长时负载、队列暂停/回收/唤醒记录和真实 GPU/cache 结果 | B1、B7 / USER_VALIDATION |
| R21 | 建图示例冗长；忽略已接受目标的错误不安全 | 优先 enqueueAfter 常见用法；评估最小 `>>` 独立 Chain 结果，必须保留首错且不污染已接受节点；最终作纳入或不纳入的明确决策 | Draft/已接受端点/空或拒绝句柄/临时对象/链式首错/原图不被修改 | B5 / DECISION_RECORDED + HOST_VERIFIED：本轮不纳入 Chain，保留 enqueueAfter/precede；首错与已接受目标隔离已验证，Chain 专属项不适用 |
| R22 | valid、接受、等待和结果容易混淆 | 保留必要状态维度，给出查询语义表和短示例；说明 C 操作状态与 C++ 任务码，不直接砍成四接口 | Invalid/Draft/Pending/Rejected/Failed/Skipped/Succeeded/超时的矩阵 | B5 / HOST_VERIFIED（查询矩阵与原始 C 操作码分离；见 B5 报告） |
| R23 | 普通任务 int、tiled void/int 是明确的现有契约 | 保留普通 int 约束，解释 tiled 历史宏；不以“不一致”自动扩展 void async | C++11 消费与普通非 int 拒绝、tiled void/int 测试 | B5 / DECISION_RECORDED + HOST_VERIFIED：不增加 void async，C++11 正/负编译验证及 tiled void/int 通过 |
| R24 | flow 专属错误码经需求允许，目前未发现通用码冲突 | 保留数值与兼容名字，登记 -5001~-5004 段及查询方法，完善一致性测试 | 名字/数值/未知码与业务同值 domain；不擅改 au::err::isError 全局语义 | B5 / HOST_VERIFIED（数值/名字/未知码/业务同值 domain；见 B5 报告） |
| R25 | parallelizeTiledTasks 过长 | 增加明确 range/tile/offset/count 语义的 parallelFor 薄别名；保留原名与旧宏，不默认扩张一整套宏族 | C++11、旧宏、新入口行为一致；不改变 offset/count 语义 | B5 / HOST_VERIFIED（新别名、旧方法/宏、非拷贝回调与 C++11 生产消费者） |
| R26 | 普通 Task GPU 边等待完整完成有 host gap；大小核类别非首版强制 | 文档明确取舍；测单任务/普通 DAG/native 链。按数据决定显式设备依赖，不改普通边。显式 CPU 集合保留；核类别探测按能力/收益作结论 | 同 context/跨 context、staging/直接导入、失败/清理/资源别名与 host/device 分域测量；缺设备不算完成 | B1、B6、B7 / USER_VALIDATION：设备决策 |
| R27 | Android 手写 pthread、缺 log 的链接问题已有对方复现 | Threads::Threads + CMAKE_DL_LIBS；Android 链接 android/log；移除测试目标重复 pthread，不用桩作为修法 | 本机构建通过；NDK r25c/API28 arm64 默认及 OpenCL 目标真实链接，无 pthread 桩，记录实际结果 | B0 / HOST_VERIFIED；NDK 验证 USER_VALIDATION |
| R28 | 密集多语句与自命名空间限定影响阅读 | 按 .clang-format 整理相关文件；不引入 using；行为与格式分开审查 | diff --check、格式检查及相应构建；不全仓格式化 | B4/B5 / HOST_VERIFIED（实现、公开界面及相关示例；机械格式补丁单独留证） |

## 4 补充问题清单

| ID | 补充约束/问题 | 处理与验收 | 批次 / 状态 |
| --- | --- | --- | --- |
| X01 | 控制与工作共用等待入口但不共用许可 | 独立 control epoch；控制变化 notify_all；工作项才增减 workCount；shutdown 无伪许可；休眠前竞态模型 | B3 / HOST_VERIFIED（见 B3 报告；设备性能 USER_VALIDATION） |
| X02 | 查询者交接通知与休眠原子性 | 交接/退出/预算变化唤醒；普通 poll 不广播；过期代际不能接管新槽；无事件零查询 | B3 / HOST_VERIFIED（见 B3 报告；设备性能 USER_VALIDATION） |
| X03 | 深栈串行 tiled 绕过 Loop::step | 所有回调入口统一 TileContext RAII；保存 Loop 错误入口/offset；串行/嵌套/异常一致 | B2 / HOST_VERIFIED（见 B1/B2 报告） |
| X04 | 违规提交错误在外部 caller 的空 gFrame 丢失 | 用循环错误接收入口，兼顾公开适配的失败早退；错误按最小执行失败 offset 汇总 | B2 / HOST_VERIFIED（见 B1/B2 报告） |
| X05 | 协作执行无关任务被误认为 tile | 保存/清除/恢复 depth 与 TileContext；无关任务可正常提交，其错误不污染外层循环 | B2/B3 / HOST_VERIFIED（上下文隔离与通知等待回归） |
| X06 | shutdown 只等 helper 会漏 caller；新 Job 与关闭竞态 | Job 接受与 Stopping 同一线性化协议；RAII 排空整个调用；无 helper 与最后 helper 提前结束均测试 | B2 / HOST_VERIFIED（见 B1/B2 报告） |
| X07 | 最后 worker 退休与新提交、GPU 进度、TID 复用 | 创建中/运行/退休/退出/已回收槽明确状态；锁外 join；保留完成推进者；2 秒后零驻留 | B3 / HOST_VERIFIED（见 B3 报告；设备性能 USER_VALIDATION） |
| X08 | Aura 栈不能用替换 flow 文件方式移入旧 caddies | 三引擎独立微基准；只做原始 FlashPE 与最终候选端到端对照；记录集成与数据路径变化 | B1、B7 / USER_VALIDATION |
| X09 | 候选必须有原始业务对照，GPU 传输差异不能伪称调度收益 | 固定输入/算法/内存/NDRange，记录所有不可统一之处；验输出；设备/主机时钟不直接相减 | B1、B7 / USER_VALIDATION |
| X10 | 旧源码已从 dev 删除、另外两个版本与 SDK 不在本环境 | 原实现通过固定历史提交读取，不把旧头恢复进生产；提供 GitLab/FlashPE refs 与访问；NDK/ADB/设备只走支持通道 | B1 / BLOCKED_EXTERNAL：未提供外部源码 |
| X11 | 默认测试构建不应改变生产库 | 单独编译 instrumented 变体；生产 benchmark/普通消费者链接 aura，故障测试链接 aura_testing；BUILD_TESTING OFF 不生成变体 | B0 / CLOSED（主机证据见 B0 报告） |
| X12 | helper 通用 wait 若提前失败，忽略结果会让栈上 Loop 提前释放 | 专用不可提前失败 join；接受时已有 parent→helper 完成依赖，回调等待仍走环检查；祖先等待/协作/关闭及 sanitizers 回归 | B2 / HOST_VERIFIED |
| X13 | B2 去分配后多 tile 空循环 p50 仍高于原实现，部分行循环/扇出分位数波动 | 保存全部正反结果；K=1/K=8/逐个发布实测，保留 K=8；后续定位接受、唤醒、join 成本，B3/B4/B6 同基准回测，不以分配替代延迟验收 | B2、B3、B4、B6 / IN_PROGRESS；尚未通过性能收口 |
| X14 | B2 预算下调后可能丢失周期查询推进，剩余挂起事件不再完成 | 控制广播与领导权交接；协作 wait/fence 返回用户 CPU 代码前释放身份；交接与退休裁定串行化，必要时补消费者。保留旧版失败样本，加入预算/CPU/退休交接测试 | B3 / HOST_VERIFIED；设备进度/功耗 USER_VALIDATION |

## 5 分轮任务与退出条件

### B0：最小构建修复与产物隔离

- [x] Android 删除手写 pthread、补 log，保留工具链管理的 Threads 和动态库依赖。
- [x] 正常 aura 在默认、Debug、Release 和带测试构建中均无故障注入宏。
- [x] 故障测试使用 aura_testing，公共生产消费者/benchmark 使用 aura。
- [x] 子工程使用 AURA_BUILD_TESTS 控制，不强制覆盖父工程 BUILD_TESTING，默认不生成测试目标。
- [x] 默认 CPU-only、OpenCL 模型、生产 Release 和父工程集成验证；编译命令/符号检查。
- [ ] NDK r25c/API28 arm64 链接验证（USER_VALIDATION，由用户交付后执行，不阻塞本地实施）。

退出：构建修复可审查、主机检查完成；Android 实际链接由用户在目标环境验证。本轮不改变调度状态机、队列、线程和任务语义。

### B1：固定输入、版本和可用主机基线

- [x] 固定 Aura 起点 aa3b3ae、旧源码历史 0405cc2 和 B0 构建补丁；微基准源码与两端 runtime/二进制哈希归档。
- [ ] GitLab/FlashPE 固定 ref、三引擎适配和目标环境基线（外部源码未提供、设备 USER_VALIDATION）。
- [x] 保存当前可用生产配置的主机测量、编译器/CPU/affinity、输入与采样方案。
- [ ] 用户后续确认设备连接、NDK/API/ABI、GPU 驱动/扩展与温控/频率（USER_VALIDATION）。
- [x] 当前 Aura 的空 tile、单 tile、1080p 行循环、tile16/64、合成三路 CPU 扇出汇合和空闲窗口；101 样本分位数及独立分配窗口。
- [ ] 其他引擎适配、真实 FilterGuideGrayU14 算子、五段 GPU 链和 GPU 在途窗口（USER_VALIDATION/外部输入）。
- [ ] GPU 单任务连续提交、普通任务 DAG、可用 native 链分开统计；记录 DMA/staging/Image/Buffer、传输字节和同步，不混为调度收益。
- [ ] 记录 p50/p95（必要时 p99）、固定开销、分配次数/字节、唤醒/查询次数、worker/进程线程数、PSS、首次/预热差异。
- [ ] 原始 FlashPE 端到端帧耗时及输出正确性；登记回归阈值和统计方法。
- [x] 当前 Aura 主机结果、命令、版本、哈希及原始输出已保存；设备 manifest 和外部版本待补。

退出：先保存当前可用的主机基线，允许 B2/B3 继续。三个版本设备微基准与原始 FlashPE 端到端基线由用户后续在目标环境补齐；外部实现源码未提供时不伪称完成三方对比，也不阻塞本地可完成的代码整改。

### B2：同步 Job、TileContext 与 tiled 快路径

- [x] 更新 D04-D08、waitAll 与 shutdown 边界及迁移说明。
- [x] Job 接受/关闭、caller/helper/收尾 RAII；外部 Job 不再通用 submit，managed 循环仍被父任务覆盖。
- [x] TileContext 全路径安装；禁止异步发布，违规进入循环首错；协作执行上下文隔离。
- [x] 固定数量 helper 的专用工作/接受路径，无逐 tile Task/future 和通用 DAG 容器分配。
- [x] 有界批量领取、参与者 <=4、预算不超限、失败/异常与串行退化正确。
- [x] 回归当前相应场景并按新契约修改旧预期；新增针对改变的故障/并发测试。
- [x] 与 B1 当前主机固定开销、C++ 分配比较，保留五轮原始输出及正反结果；没有设备/FlashPE 帧数据。
- [ ] X13 多 tile 空循环性能退化收口；帧/设备性能 USER_VALIDATION。

状态：B2 代码/契约/主机正确性与并发验证完成（CPU 159/159、OpenCL 172/172、ASan/LSan 和 TSan 通过）；性能已完成对比，但 X13 尚未收口，不宣称整批或 R01 CLOSED。设备性能 USER_VALIDATION。后续 B3/B6 须继续同基准回测。

### B3：统一等待、单查询者、亲核与 2 秒退休

- [x] WorkGate/控制世代与工作计数统一睡眠协议；关闭迁移控制通知。
- [x] 协作 wait 同时感知目标完成、就绪工作、控制与用户超时，去掉依赖固定 1 ms 才取得进度的路径。
- [x] 单查询者及领导权交接、查询公平；只在在途工作时计时；生产构建对照固定 1 ms 与 200–2000 µs 退避，记录延迟取舍。
- [x] 原子策略版本快速判断；就绪 fan-out 仍扩容；预算外 worker 事件停车。
- [x] 默认 2 秒退休、零空闲常驻；槽位 generation、查询 pin、锁外 join、最后 worker 竞争与 GPU 推进。
- [x] 可控休眠/交接/退出竞争测试；2,000 根任务、5 次真实退休/重建及并发策略/亲核查询的主机压力，ASan/LSan、TSan 回归。
- [x] 主机生产基准记录查询、context switch、线程、PSS 和退休后首次任务；同一 CPU 基准回测 R01/X13。
- [ ] 设备长时压力、真实 GPU/唤醒、PSS、首帧与尾延迟阈值验收（USER_VALIDATION）。

退出：无丢工作/丢控制通知/永久等待；GPU 在途持续推进；空闲实际退休；主机数据与 B1 可比，设备阈值由用户后续验证。

状态：B3 代码、主机回归及生产基准已完成，详见 [B3 报告](flow_validation/B3-2026-10-20.md)。R03 的队列对照、R19 的全局锁审计仍分别归 B6/B4；R01/X13 的延迟收口继续追踪，设备指标仍由用户验收。已提交并推送 dev：`f0f523c`；报告中的未提交描述是测量时点。

### B4：图与核心职责整理

- [x] children 邻接与等待图遍历改进，不降低环检测覆盖。
- [x] 图、执行器、外部完成、tiled 与上下文桥职责拆分；不引入不必要运行时分派。
- [x] 锁层级与回调/析构/驱动边界说明；错误合并封装，优先级与公开 ABI 不变。
- [x] 具名常量与 .clang-format；格式单独审查，不以文件行数作为拆分目标。
- [x] 深图/交叉 wait/簇/动态注册/资源错误/双 so/关闭与 sanitizer 回归。

退出：行为一致、同步关系可审查；图遍历成本和原基线比较，性能不因“整理”失控。

状态：B4 代码与主机回归完成：[B4 报告](flow_validation/B4-2026-10-21.md)、[锁关系](flow_locking.md)、[五轮数据](flow_validation/B4-2026-10-21-data.json)。CPU 175/175、OpenCL 模型 188/188；ASan/LSan、TSan 通过。图搜索锁内时间下降，但部分生产 CPU 指标变差，R01/X13 保持 IN_PROGRESS；设备性能仍为 USER_VALIDATION。B4 已提交并推送 `2342731`。

### B5：公开接口与迁移文档

- [x] parallelFor 薄别名、旧名/宏兼容；range/tile/offset/count 明确。
- [x] 查询状态表、结构化默认、XImage 借用/owning move、原始 handle 生命周期和 flow 错误码说明。
- [x] `>>` 最小 Chain 方案评估并明确纳入或不纳入；若实现，首错独立保存、不污染已接受节点。
- [x] 不扩展 void async、不删除错误来源字段、不改默认结构化语义、不删除平台分支的结论写入。
- [x] C11/C++11 消费者、ABI、旧宏、提交失败与借用安全示例验证。
- [ ] **USER_VALIDATION**：FlashPE 调用点实查；当前未提供源码，不能确认“5 宏/3 addPipeline”数量。迁移文档提供检索与逐项审查要求，按用户决定交付后在目标环境验证，不阻塞主机部分。

退出：API、设计、README、示例与真实行为一致；改变与保留均有明确迁移说明。

状态：B5 主机实现与文档完成，证据见 [B5 报告](flow_validation/B5-2026-10-21.md)、[迁移文档](flow_migration.md)。公开薄别名之外不改变调度语义；不纳入 Chain/void async。FlashPE 源码实查和目标环境验证保持 USER_VALIDATION，不将本批或整轮标为全部验收。本批未 commit/push；下一批 B6。

### B6：有数据的机制取舍

- [ ] mutex+侵入式 FIFO 对照现有 ring/MPSC：接受后无分配丢任务；溢出/暂停/复用测试相同。
- [ ] 普通分配对照 Node/edge/external/payload 池；记录内存高水位与释放策略。
- [ ] DOT 与 trace 时间线分离；可裁剪生产 trace；动态图元数据不保活不必要的执行树。
- [ ] GPU 普通完成边的 host gap 报告；决定是否本轮实现显式设备依赖，不能默改 precede。
- [ ] CPU 核类别能力/需求评估，缺 cpu_capacity 时回退；不假定固定大核编号或数量。
- [ ] 每项产出“保留/替换/缩减/不纳入本版”的最终决策与数据，记录未实现功能边界。

退出：R03/R09/R13/R26 不再以“待测量”无限挂起；实验结果足以支撑决定。若数据证明保留更合理，也可关闭对应建议，不等于实现了原建议。

### B7：最终集成与全量收敛

- [ ] 最终候选接入 FlashPE，与原始业务版本端到端对照、输出正确性及错误路径验证。
- [ ] 骁龙与天玑设备适用的 cache/导入/资源寿命、长时压力、线程退休及 GPU 完成验证。
- [ ] 默认/生产/CPU-only/OpenCL/C/C++11/双 so/关闭/故障测试，ASan/LSan 与可用环境 TSan；未支持项单列。
- [ ] 正常退出与 SDK 调用全部返回后卸载；驱动挂起不伪造完成或强制释放。
- [ ] 更新本表全部状态、链接提交与原始证据，区分保留决策、主机通过和设备通过。
- [ ] 最终报告逐项交代 全部 R/X 项（含新增 X12/X13/X14），没有无解释的 TODO；外部阻塞存在时不宣称整体完成。

## 6 验收矩阵

| 编号 | 必验内容 | 证据要求 |
| --- | --- | --- |
| V01 | 构建及故障钩子隔离 | 默认、Release、CPU-only、OpenCL、子工程；compile_commands 与库符号 |
| V02 | Android 构建 | NDK r25c/API28/arm64 实际链接，无 pthread 桩；按设备要求补 NDK 版本 |
| V03 | 公共边界 | C11/C++11、结果布局、错误来源、旧入口、跨 owner/双 so |
| V04 | 接受/图/等待 | OOM/环/Draft/重复/动态竞争/间接 wait/深图/快照 |
| V05 | tiled 与 Job | 所有退化路径、一次覆盖、offset 错误、参与者预算、caller/关闭与借用 |
| V06 | 等待与退休 | 睡眠发布间隙、控制广播、领导权、最后 worker、代际、查询 pin、2秒退休 |
| V07 | 外部完成与资源 | 单/多事件、同/跨 context、cache START/END、staging、失败/清理、延迟释放 |
| V08 | host sanitizers | 适用主机 ASan/LSan/TSan；不关闭检测；平台失败单独诊断 |
| V09 | 真机性能/功耗代理 | 固定版本/输入/设备/生产选项；首次/预热、p50/p95、分配/唤醒/线程/PSS |
| V10 | 业务及卸载 | 原始与候选 FlashPE 对照、输出一致、全部外部 SDK 调用退出后卸载 |

## 7 当前执行与阻塞记录

2026-10-20 初始检查：当前 dev 干净，HEAD aa3b3ae；当前环境仅有 Aura、Obsidian、taskflow。未发现 NDK、ADB 或已连接设备，未提供 FlashPE/GitLab 实现。已通过异步问题请求相应访问路径/固定版本及设备连接方式，不要求在聊天中提供凭据值。

本轮先保存完整计划，完成 B0 的代码修复与主机验证。用户随后明确回复：先不考虑 NDK、ADB 和目标设备，任务完成后由用户自行在目标环境验证。因此取消设备基线对本地代码整改的阻塞：保留主机基线和逐轮回归，设备验收标为 USER_VALIDATION，不伪称通过。后续可以按 B1-B7 分轮推进。

后续每次任务先读本文和 git 状态，更新对应 ID、批次勾选、验证证据和阻塞，不另起脱离本表的待办清单。外部条件到位后，从尚未完成的门槛继续，不重复已验证步骤，除非代码/配置变化使其失效。

本轮 B0 主机验证已完成：[构建修复与隔离报告](flow_validation/B0-2026-10-20.md)。CPU-only 154/154 单测和 10/10 CTest；OpenCL 167/167 单测、13 项 CTest 通过、1 项真实 GPU skipped。R14/X11 已关闭；R27 实现及主机检查完成，Android 实际链接由用户验证。该记录是 B0 时点证据，后续进度如下。


2026-10-20 B1/B2：[主机基准与同步 Job 报告](flow_validation/B1-B2-2026-10-20.md)，[原始测量/manifest](flow_validation/B1-B2-2026-10-20-data.json)。B1 可用主机基准完成；B2 契约、Job 接受/关闭、所有 tile 路径、专用 helper/8-tile 批量与不可提前失败 join 已实现并验证。CPU 159/159、OpenCL 172/172；CPU/OpenCL CTest 分别 10 通过、13 通过+1真实硬件 skipped；ASan/LSan 13/13、TSan 10/10 通过。B0–B2 随后按用户要求提交并推送到 dev：`900a738`。该提交不包含 B3。

新增 X12 已修正内部等待提前失败的生命周期风险。新增 X13 记录多 tile 空循环 p50 退化和部分负载分位数变差；完整保留数据，不能据单 tile 更快/去分配宣布性能验收完成。B3 开始时先保留该基准与 X13，等待/唤醒/查询者/退休改造后回测；B6 最终决定机制取舍并收口。B4-B7 的未实施事项继续维持 TODO。


2026-10-21 B3：[统一等待与 worker 生命周期报告](flow_validation/B3-2026-10-20.md)，[五轮原始数据与 manifest](flow_validation/B3-2026-10-20-data.json)。完成工作/控制分离、协作 wait/fence 通知、单周期查询者与交接、策略版本快速判断、预算停车、2 秒退休和槽位/query pin/join 协议。CPU 172/172、OpenCL 模型 185/185；CTest 分别 11 通过、14 通过+1硬件 skipped；ASan/LSan 14/14、TSan 11/11，最终测试时序屏障补跑 OpenCL ASan 1/1；正常 Release 符号与严格 C11/C++11 检查通过。本批代码和文档均未 commit/push。

生产模型五轮：B3 在途 200 ms 查询次数中位数 97，固定单查询者 1 ms 实验 185，B2 590；这不是实际设备唤醒计数。B3 2.3 秒空闲后 worker 0 / 进程线程 1，未观察到 PSS 下降；重建和完成延迟代价均记录。新增 X14 追踪预算下调后的查询停滞：B2 最终 5 轮 1 次、早期 5 轮 4 次失败均保留；B3 两组最终均 5/5 正常完成，相关交接/退休竞争测试通过。

R01/X13 尚未收口：B3 CPU 回测同时包含改善和退化，不以主机有限样本宣布全面性能提升。后续顺序仍为 B4 → B5 → B6 → B7；设备长时稳定性、真实 GPU/功耗与 FlashPE 用户验收不阻塞本地代码推进，也不被记为已完成。


2026-10-21 B3 随后已按用户要求提交并推送至 dev：`f0f523c`。B4 从该提交继续，没有重做 B0–B3，也未改变其已收敛契约。

2026-10-21 B4：[图与核心职责报告](flow_validation/B4-2026-10-21.md)、[源码/五轮原始数据与格式阶段差异](flow_validation/B4-2026-10-21-data.json)、[锁关系与状态所有权](flow_locking.md)。实现 borrowed child 邻接、无分配多目标搜索、Core 实现拆分、TaskErrors 封装、私有 mm RAII 桥、具名常量和限定格式整理。保留全部必要环检查、公开 ABI 与错误来源；token 溢出回滚在 graph 锁外销毁。

CPU 175/175、OpenCL 模型 188/188；CPU/OpenCL CTest 分别 11 通过、14 通过+1硬件 skipped；ASan/LSan 14/14、TSan 11/11。最终仅补充私有计时后，相关 CPU 6/6、ASan/OpenCL 4/4、TSan 3/3 再检查通过。正常 Release 符号、严格 C11/C++11、flow 测试关闭时的 benchmark 构建及格式检查通过。

图基准的全局活动扫描降为 0，协作栈多次检查合为一次；Node 从 584 增至 624 字节，内存取舍纳入 R09/B6。生产 CPU 五轮包含退化：managed 空循环和合成 fan-out 的 p50 约增加 7.5% / 7.8%，不能据图微基准关闭 R01/X13。B3/B4 的事件/退休模型均 5/5 正常完成；真实设备/FlashPE 指标仍由用户验证。B4 当时未 commit/push，后已推送 `2342731`；后续 B5/B6/B7 按各批状态跟踪。

## 11 B5 实际执行记录（2026-10-21，北京时间）

以已推送 B4 `2342731` 为基线，新增 parallelFor 薄别名、迁移文档与 C++11 生产例子；其余公开语义及 Core 调度算法保持。R11/R17/R18/R22/R24/R25/R28 主机验证，R16/R21/R23 的保留/不纳入决定明确，R26 的 host gap 与平台能力边界补充说明但不关闭设备项。

CPU 180/180、OpenCL 模型 193/193；CTest 分别 13/13 和 16 通过+1 硬件跳过；ASan/LSan 16/16、TSan 13/13；最终接口子集 8/8，C11/C++11/C++17 严格编译、B4 消费者及 ABI v1 对照通过。详见 [B5 报告](flow_validation/B5-2026-10-21.md)与[数据](flow_validation/B5-2026-10-21-data.json)。

FlashPE 源码调用点实查保持 USER_VALIDATION，迁移文档未确认对方报告的数量；Android/真机按用户决定稍后验证。B6/B7、R01/X13 与目标设备最终验收保持未完成。本批未 commit/push。
