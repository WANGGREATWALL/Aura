# au::flow 公共接口与迁移

适用于 Aura 1.5.0 / flow C ABI v1。C++11 外观入口为 `flow/xflow.h`，历史 `flow/xthread_flow.h` 仍可用。整改状态见 [清单](flow_remediation_plan.md)，可编译运行的完整例子见 [flow_migration.cpp](../test/flow_migration.cpp)；该例子使用生产 `aura`，不依赖故障钩子。

## 1 常用入口与依赖

普通任务函数必须返回 **恰好 `int`**，`0` 成功、任意非零（包括正数）失败。参数按 decay 后的类型复制/移动，`std::ref/std::cref` 显式借用；它们不深拷贝对象指向的数据。`makeTask()` 仅建立 Draft，`enqueue()` 接受任务而不等待主体完成。

已有异步任务之间的常见连接直接写为：

```cpp
au::flow::XFlow& flow = au::flow::XFlow::get();
auto a = flow.enqueue([]() -> int { return 0; });
auto b = flow.enqueueAfter(a, []() -> int { return 0; });
int code = b.wait();
```

`enqueueAfter({a, b}, f)` 支持汇合。前驱必须已经接受（可以已完成），重复前驱无害；空列表表示没有依赖，并不改变调用上下文的父子归属。无效、Draft 或拒绝前驱使新目标拒绝；失败前驱使已接受后继跳过主体，结果保留原始失败及根因。普通 Task 边等待前驱的**完整完成**，包括其结构化后代、GPU 及主机清理。

一次提交 Draft 图时使用 `Tasks`：

```cpp
auto a = flow.makeTask([]() -> int { return 0; });
auto b = flow.makeTask([]() -> int { return 0; });
int code = a.precede(b);
if (code) return code;
au::flow::Tasks graph{a, b};
return flow.enqueue(graph).wait();
```

不能分别 enqueue 两个仍互相依赖的 Draft，期望调度器自动收集整张图。`Tasks` 是 move-only builder；`handle()` 返回拥有引用的 Task，可用于等待或连接。提交后图冻结；编辑、提交、abandon 与销毁同一个 Draft builder 必须由调用方串行化。

**每次 `precede/add/setName` 都应检查返回值。** Draft 目标保存第一次构建错误，提交不会把残缺图当成功；这并不允许调用方忽略已接受目标上的错误。目标已接受时不能补边，失败只返回错误，不污染前驱，也不追溯拒绝正在执行或已完成的目标。一次错误操作返回它自己的错误，稍后提交 Draft 时报告保存的首错，两者可以不同。

本轮**不纳入 `operator>>` / Chain**，这是 R21 的明确取舍，不是遗漏：

| 最小候选 | 评价 |
| --- | --- |
| `a >> b` 返回 Task | 丢失错误，尤其无法安全处理已接受目标；不采用 |
| 独立 `Chain { last, firstError }` | 可以独立保留首错，但只保存 last 不能提交完整 Draft 图；仍需 Tasks 及最终错误检查 |
| 自动收集节点、分组并提交的 Chain | 需定义 group/临时对象/重复成员/部分成功规则与额外存储，超出薄语法糖范围 |
| 现有 `enqueueAfter` + 显式 Draft 图 | 已覆盖动态链/汇合和一次性建图；保留，补充失败矩阵与例子 |

若将来增加 Chain，首错必须由独立结果持有，最终提交必须检查该结果；禁止将错误写入前驱或已接受目标。当前没有 Chain API，因此链式专属测试不适用。

## 2 默认结构化完成及生命周期

任务体内调用 `enqueue/enqueueAfter` 时，新接受任务自动成为当前任务的子任务。忽略或销毁返回的 Task 句柄不会取消工作，也不会使它变为 detached。主体可以先返回，父任务直到所有后代与外部清理完成才进入终态，并按既有规则继承失败。外部线程调用则产生独立异步根。

**结构化完成不延长 C++ 局部变量的生命周期。** 父任务主体的栈变量、按值参数可以在主体返回时销毁，早于子任务结束。子任务需要的数据应显式转移给子任务、共享一个真实 owner，或在局部数据离开作用域前等待子任务。不要让子任务引用已结束主体的局部变量。完整例子中的 `result` 定义在外部，并保活到 `parent.wait()` 返回。

- `waitAll()` 等待调用时已接受异步根的快照及其结构化后代，不等待之后创建的独立根；不是持续全局屏障。
- 外部同步 tiled Job 不在上述快照中；managed Task 内的同步循环由该 Task 的完成覆盖。
- `shutdown()` 拒绝新的外部工作，允许已接受 Task 排空合法后代，并等待同步 Job、设备/主机清理及 worker join；关闭后不支持重启。
- 任务内部 `wait()` 会协作推进工作，并检查等待环/祖先/深度。`waitAll/shutdown` 不可从 managed task 或 tile 调用。
- `shutdown()` 排空不等于 SDK 外部调用栈已经返回；调用方在 `dlclose` 前须确认所有 SDK 调用返回并销毁所有 Task/资源句柄。不用 CL 回调退出计数推断 `.so` 已无返回栈。

## 3 查询语义

`valid()` 表示外观中保存了任务身份**或拒绝结果**，不表示成功接受。默认/移走后的 Task 为 false；分配失败但保存了拒绝码的 Task 仍为 true，此时 `id()` 为 0。有节点的拒绝任务可以有非零 ID。`ready()` 只表示终态，失败、跳过、拒绝都为 true。

以下矩阵假定无非法等待、参数和 ABI 大小错误；单独查询会随并发执行变化，不是跨多个调用的原子快照。

| 状态 | valid | submissionCode | ready | code | result(out) 操作码 | waitFor(0) / wait() |
| --- | --- | --- | --- | --- | --- | --- |
| Invalid / moved-from | false | InvalidHandle | false | InvalidHandle | InvalidHandle | InvalidHandle |
| Draft | true | NotReady | false | NotReady | NotReady | NotReady（不自动提交） |
| Pending / Ready / Running / Waiting | true | 0 | false | NotReady | NotReady | 未完成时 Timeout / 等终态后给任务码 |
| Succeeded | true | 0 | true | 0 | 0 | 0 |
| Failed | true | 0 | true | 原始非零码 | 0 | 原始非零码 |
| Skipped | true | 0 | true | 继承失败码 | 0 | 继承失败码 |
| Rejected（含无节点拒绝） | true | 拒绝码 | true | 拒绝码 | 0 | 拒绝码 |

`TaskResult::mState/mDomain/mCode/mNativeCode/mCauseTaskId` 分别描述最终状态、来源、任务码、原生码与根因身份，`mStructSize` 保持版本布局。C++ `result(out)` 返回查询操作码，只有成功取到终态结果才写 out。C++ `wait/waitFor` 在等到终态后返回任务码并填结果；超时、非法等待等操作错误保留 out 原值。超时不取消任务。多个任务可能返回与 Timeout 等相同的业务数值，要结合已填的终态结果判断，不能仅凭一个 int 推断来源。

C 窄腰故意保留操作码与任务码两个维度：

```c
au_flow_result result = {0};
result.mStructSize = sizeof(result);
int operation = au_flow_wait(handle, UINT64_MAX, &result);
if (operation != 0) return operation;  /* 等待/句柄/版本错误，result 未更新 */
return result.mCode;                  /* 任务可能失败，operation 仍是 0 */
```

`au_flow_result_get` 也返回操作码。C 结果结构须按当前 `sizeof` 初始化 `mStructSize`；C++ facade 自动处理。C++ `Tasks` 的 wait/ready/code 与 Task 相同。保留各查询入口，因为接受、执行完成、等待错误和失败来源是不同信息。

## 4 XImage、借用和资源租约

- 按尺寸构造且分配成功的 XImage 拥有存储；`Image` 是普通描述符，`view()` 返回借用。
- 复制 XImage（包括 lambda `[img]`、`enqueue(f, img)` 的参数复制）只借用；不增加存储所有权。
- `std::move(owningImage)` 转移已有所有权；`std::move(borrowedImage)` 仍然只移动借用，不能补出 owner。
- C++11 使用 `enqueue(f, std::move(owner))` 将拥有型 XImage 作为参数交给任务；不要使用 C++14 lambda init-capture 当作 C++11 示例。
- 借用模式下，owner/外部 mapping 保活到所有消费者结束。任务开始前尚无 CPU/GPU 租约；不能 enqueue 后立即 reset owner，再寄希望于任务未来 acquire。
- `CpuAccessScope::begin` 或 wrapper 提交成功时取得的内部租约，在**已接受的访问期间**保活实际存储并处理 fence/cache；`readAccess(image)` 只是描述访问，创建它本身不保活。新发现范围在 owner 退休后可失效，在途租约仍按协议排空。
- 外部内存用 `ExternalMemory` 明确注册精确容量；借用注册并不保活外部 mapping。需要共享外部 owner 时，通过注册协议的 retain/release 回调或业务 owner 管理，不能把 registry 存活检查当成 ownership。

不增加“任务结束时检查借用存活”来冒充防护：它发现不了中途 UAF，也不能关闭检查与使用间的竞争。异步参数在任务主体调用时按 move 传入；普通任务主体的局部 owning XImage 也不会因结构化子任务存在而自动延长生命。GPU 设备及主机清理保活由成功提交的租约负责。

## 5 原始 C handle 与引用

`au_flow_make/group` 成功返回一份拥有引用。`au_flow_handle` 赋值只复制指针；需要另一份拥有引用时显式 `au_flow_retain`，每份最终 `au_flow_release`。跨线程传递须在原拥有者仍保活时 retain，并自行同步发布。合法拥有引用阻止节点槽位被复用；C++ Task 的复制/移动/析构自动执行该协议。

除 API 定义的空句柄行为外，查询/操作需要对应 Aura 实例中的活句柄。各算法 `.so` 实例隔离；已知存活但属另一实例的句柄不允许参与本实例图。**任意伪造、已最终 release 或所属 `.so` 已卸载的指针不属于可安全探测的输入。** 即使 owner 相同，过期地址也可能已被新节点复用。本轮保持 ABI v1，不增加伪安全的“读指针内 ID”检查；未来若强化需稳定句柄表、generation 及访问 pin。

接受后的任务不因调用方丢句柄而取消。未提交 Draft 的最后引用释放会回收其闭包及依赖引用；Draft 环检测继续防止强引用环。不要通过调用悬空句柄来验证安全性。

## 6 parallelFor 与旧接口

新增 `XFlow::parallelFor(range, tile, f)` 是 `parallelizeTiledTasks` 的 header-only 薄别名，未新增 C 符号、未改 ABI v1，也没有新增整套宏。旧方法、历史 include、`XTHREAD_PARALLELIZE_TILED_TASK_QUOTE/ASSIGN/END` 均保留。

```cpp
int code = flow.parallelFor(height, 16, [&](std::size_t offset, std::size_t count) {
    for (std::size_t row = offset; row < offset + count; ++row) {
        // 只处理本 tile 的行；共享可变状态须自行同步。
    }
});
```

- 范围是 `[0, range)`；`tile > 0`，即使 `range == 0` 也必须如此。零 range 不调用回调。
- 回调收到元素 offset 和 count，不是 tile 序号或 end；尾块可小于 tile。旧宏 `_i/_ti` 分别是 offset/count。
- 同一个可借用、可不拷贝的回调可被并发调用，最多四个参与者（含 caller）；不保证分块顺序、固定线程数或执行线程身份。
- 回调允许 void 或恰好 int。非零停止领取新 tile，等待在途计算结束，报告已执行回调中的最小失败 offset 对应错误；没有回滚已完成输出。
- 回调禁止发布异步 Task 和异步 GPU 工作，调用返回 StateInvalid 且登记到循环，忽略返回值或 void 回调也不能使循环伪成功。允许创建 Draft/编辑图；嵌套同步 tiled 串行退化。等待已有合法任务仍适用通常的环/深度检查。
- 方法是同步的，返回前回调和 helper 全部结束；这不允许另一个线程提前释放借用的输入/回调。shutdown 登记覆盖整个同步 Job；waitAll 不包含外部同步 Job。

这沿用 B2 已生效的 tile 发布契约，不重新引入任意异步提交。普通 void async 本轮不纳入：业务需求统一 int；历史 tiled 宏的 void 是同步回调兼容行为。

## 7 错误码与平台边界

flow 专属错误段保持原数值与名字，不迁移进通用错误枚举：

| 名称 | 数值 | 含义 |
| --- | --- | --- |
| kErrorDependencyFailed | -5001 | 保留的依赖失败码；已有具体根因时不强制用它覆盖 |
| kErrorCycleDetected | -5002 | 构建/提交的图依赖成环 |
| kErrorWouldDeadlock | -5003 | 不允许的等待/重入及深度边界 |
| kErrorUnhandledException | -5004 | 未处理异常被边界捕获；同值业务返回仍属 Callable |

字符串用 `au::flow::getErrorStr(code)`，flow 码之外转给 `au::err::getErrorStr`，未知值返回 UnknownErrorCode。查字符串不确定来源；保留 TaskResult 的 domain/native/cause。通用 `au::err::isError` 仍只判断负数，不能用于 flow 的成功判断，应使用 `code != 0`。

优先 Android ARM64/Linux；不删除 sys 的 Apple/Windows 分支或擅自缩小 Aura 目标。当前本轮只在 Linux x86_64 验证；Android NDK/真机、Apple、Windows 未运行。Android CMake 已使用 Threads::Threads 和 log，无 pthread 桩；`std::thread` 不免除工具链的链接配置。亲核性保留显式 CPU 集合，未承诺自动 Big/Mid/Little 分类。

GPU 普通 Task 依赖等待完整完成。因此跨 GPU Task 的 DAG 边存在从 A 设备完成到查询观察、主机清理、B 调度并入队的 host gap；不把 A 自身执行时长计入这段开销。把连续提交放入一个任务可减少任务边间隙，但 native 链仅在具体 context/导入/fence 协议允许时走设备事件链；staging/Image/跨 context 可能仍需主机桥接。没有 `.gpu()` 提示来偷偷改变普通 Task 的完成语义。设备依赖及大小核的最终取舍留 B6/B7 的明确决策与用户设备验收。

## 8 FlashPE 整合验收

当前环境仅有 Aura、Obsidian、taskflow，未提供 FlashPE 或 GitLab 调度器源码。**本轮没有实查 FlashPE 调用点，也不确认“5 处宏、3 处 addPipeline”这个数量。** 此项标为 USER_VALIDATION，沿用用户自行目标环境验证的决定，不阻塞 B5 主机交付。

接入时在实际固定版本上执行以下检索，记录文件/行号、宏是否忽略 int、图像 owner 和同步边界，并编译完整业务；不要只做文本替换：

```sh
rg -n 'XTHREAD_PARALLELIZE|parallelizeTiledTasks|addPipeline|\.get\(' <FlashPE源码目录>
rg -n 'XImage|enqueue|std::move|decoderedA0' <相关算法目录>
```

对每个 addPipeline/get 核对旧 future 的返回/异常/生命周期语义，再选择 enqueue/wait；`get()` 也可能属于完全不同的类型。对每个 tiled 宏核对 `_i/_ti`、异步发布、共享状态与错误处理。Aura flow 与 mm/gpu 完成协议有关，不能仅拷贝 flow 文件就假设仍是同一内存路径。候选接入后需测业务输出、错误路径、帧耗时 p50/p95；这不由主机例子或微基准替代。
