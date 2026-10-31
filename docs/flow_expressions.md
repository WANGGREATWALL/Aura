# au::flow 表达式接口（dev，C++11）

`Task` 表示一次工作及其结果，`Tasks` 表示共享任务节点组成的片段，`Flow` 创建和提交工作。
所有 C++ `enqueue` 重载均返回 `Task`，通过 `wait()`、`code()` 和 `done()` 观察结果。
本次为 1.7.0 之后的 C/C++ 接口不兼容变更；已发布的 1.7.0 标签不变。

```cpp
auto& flow = au::flow::Flow::get();
auto a = flow.create(prepare);
auto b = flow.create(processB);
auto c = flow.create(processC);
auto done = flow.create(finish);
int code = flow.enqueue(a >> (b | c) >> done).wait();

// 静态列表只创建一个汇合任务，元素也可以是串行片段。
auto branches = au::flow::Tasks{left, middle1 >> middle2, right};
auto completion = flow.enqueue(prefix >> branches >> suffix);
code = completion.wait();

// 大量运行期分支：一次遍历，不反复二元组合。
auto dynamic = au::flow::Tasks::parallel(tasks.begin(), tasks.end());
code = flow.enqueue(dynamic).wait();
```

日常优先等待提交返回的完成 Task；`waitAll()`、`releaseWorkers()` 影响整个共享运行时，
不是单个算法实例的专属屏障。少量分支用 `|`，多个静态分支优先用 `Tasks{...}`，动态数量用区间工厂。

## 构图规则

- `Tasks` 保存入口集合 `heads`、单个出口 `last` 和首个构建错误。
  入口是“为整个片段添加前驱时需要连接的节点”，不要求节点在运行时图中没有其他前驱。
- `A >> B` 立即建立 `A.last → B.heads`，不创建任务；结果保留 A 的入口、B 的出口。
- 多分支并行立即建立 `各分支.last → 新汇合任务`，入口取并集；没有公共入口任务。
  汇合任务是普通空任务，照常排队和执行。
- 空 `Tasks` 是两种组合的恒等元；空分支会忽略，单分支不增加任务。
  单独 `enqueue(空 Tasks)` 创建并提交一个成功的空任务。无效的 `Task()` 是错误，不是恒等元。
- `Tasks{...}` 和 `parallel(first,last)` 使用同一扁平实现，支持 Task 或 Tasks 元素。
  区间只遍历一次，收集入口后统一去重，不循环调用二元 `|`；建边仍包含环检测，不承诺总耗时 O(m)。
- 每条新边的后继必须尚未提交，前驱可以已经接受或完成。
  因此已提交的 t1、t2 可以出现在 `(t1 | t2) >> fresh` 中；`prefix >> (t1 | t2)` 会失败。
  空片段不建边，故不会额外要求另一侧未提交。
- `>>` 的优先级高于 `|`：`a >> b | c >> d` 是 `(a >> b) | (c >> d)`。
  并行段应加括号。`a >> (b,c)` 是逗号表达式，只连接 c。

## 共享、错误和完成

复制句柄或片段共享原节点，不复制任务，也不是可重复执行的图模板。
例如 `p = b | c; a >> p; d >> p;` 会让 b、c 同时依赖 a、d，提交一条链也可能连带提交另一条前缀。
单独构造 `b | c` 不会给 b、c 添加入口前驱；串行已建立的依赖会保留，丢弃表达式不表示回滚。

构建错误由表达式保存，提交失败由返回的 Task 保存，不覆盖原句柄的执行结果。
创建、构建或提交失败均返回不持有节点的错误 Task：`valid()==false`、`done()==true`，
`nativeHandle()==nullptr`，通过 `wait()/code()` 读取错误。原始 Draft 句柄不因提交失败而失效。
任务已执行但返回业务错误时仍持有节点，`valid()==true`；默认空 Task 则 `done()==false`。
低层 `Task::precede()` 直接返回保存的端点错误。当前驱保存错误、后继有效时，
它会尝试让未提交后继记录 `kErrorInvalidParam`，自身仍返回前驱原始错误；后继保存错误时不调用 C 层。
该失效处理不替代检查返回值，也不修改已接受任务的结果。表达式仍由 Tasks 保留原始构建错误。
分配失败同样通过错误句柄报告，不为报告失败再分配节点。未完成任务的 `kErrorNotReady`
不是构建错误；已经完成的业务错误仍通过依赖传播。
校验失败不接受任何新节点；成功后原子提交全部尚未提交的上游。重复提交不会重复执行。

前驱失败时后继跳过函数体，传播原始错误码；并行兄弟仍独立运行，多错误不保证选择顺序。
等待返回的出口 Task 覆盖片段中的工作；其 `costUs()` 只表示出口节点耗时，不是整图耗时。
节点完成（包括被跳过）后释放上游引用，保留自身结果、名称和耗时。
仅持有尾 Task 不会保留整条已完成历史链；调用方仍持有的 Task/Tasks 按各自所有权保留节点。
`enqueueAfter` 保留：它直接连接前驱与新任务，适合动态追加，多个前驱不需要额外汇合节点。
等待错误不代表其他工作已停止，清理共享资源前须确认相关工作完成。

任务完成仍只表示函数返回或被跳过，且 payload 已销毁。借用参数必须存活到完成，
`std::ref` 和移动借用视图都不延长底层存储寿命。flow 不跟踪 OpenCL event；
GPU→CPU 边界的设备同步由任务函数自行完成。线程、tiled、亲核与回收契约不变。
tile 回调返回值不收集；需要错误码时写入调用方管理的状态，在同步循环返回后读取。

## 从 1.7.0 迁移

| 旧用法 | 新用法 |
| --- | --- |
| `int code = flow.enqueue(task/chain/group)` | `Task completion = flow.enqueue(...); int code = completion.wait();` |
| `chain.last().wait()`、`Tasks::wait()` | 等待提交返回的 Task |
| `Tasks group; group.add(task);` | 静态用 `Tasks{...}`；动态收集 Task/Tasks 后用区间 `parallel` |
| `Tasks("name")` | 片段不命名；实际任务继续用 `createNamed/enqueueNamed` |
| `Tasks::precede`、`Task::precede(Tasks)` | 使用 `>>`；低层 `Task::precede(Task)` 仍保留 |
| `Tasks::nativeHandle()/valid()`、`detail::Link` | 不再提供；提交返回的 Task 提供有效性、结果和原始任务句柄 |
| 提交失败的返回 Task 仍可能 `valid()==true` | 失败结果不再持有原节点；`valid()==false`、`done()==true`，从 `wait()/code()` 读错 |
| `XFlowGroupHandle` 及 `au_flow_*group*` | C++ 用 `Tasks` 表达式；C 用普通任务和依赖边构图，等待汇合任务 |

Task 不隐式转换为 int。使用 `auto` 保存旧提交返回值的调用点也要检查，不能只依赖编译错误定位。
保留的 C 函数仍返回原来的接受状态码。旧 Group C ABI 已完整移除，不保留兼容桩。
C++ `Tasks` 使用普通任务和依赖边构图，不需要组归属状态。

## 当前基线与验证摘要

公开层为 17 个 C 函数和 C++11 外观，内部使用 C++17。旧 Group C 接口及其实现已移除，
任务调度与 GPU、内存模块解耦。运行时保留以下约束：

- worker 惰性创建，最多 8 个，通过 `releaseWorkers()` 排空并回收；没有空闲自动退休。
  外部等待线程也可能协助执行普通任务，因此普通任务总并发数可能超过 8。
- 单次 tiled 使用调用线程和最多 3 个 helper；嵌套 tiled 串行执行。
- 建图和调度状态由 Core 锁保护；任务公开查询与可见状态写入使用节点锁。
  建边只有记录构建错误时才额外获取后继节点锁。用户回调和最终所有者释放在锁外执行。
- payload 支持两阶段构造、64 字节内联存储和超对齐回退；完成传播不新增分配。
- 亲核只作用于 worker。首次初始化保存调用线程的完整 CPU 集合，`setAffinity(0)` 恢复该集合；
  自定义 mask 覆盖 CPU0～63，配置由 worker 在下一次调度入口应用。

最近一次验证环境为 Linux x86_64、GCC 14.2，允许 CPU 集合为 0～4：

| 验证项 | 结果 |
| --- | --- |
| Release / ASan / TSan | 各 129 项单元测试通过 |
| flow 随机顺序回归 | 28 项测试，20 轮通过 |
| C ABI / C++ 外观 | 严格 C99 / C++11 编译、链接和运行通过 |
| 分配失败检查 | 36 个建边/闭包提交预算及 32 个表达式构造预算通过 |

回归覆盖十万级成功/短路流式链、15000 次随机环对照、并发追加后继、共享节点、
原子闭包提交、payload 析构和完整默认亲核恢复。已完成任务重复提交不分配。

代码使用完整 CPU 列表恢复默认亲核，支持编号大于 63 的 CPU；上述环境未实测这种拓扑。
本轮没有重新测量性能，不把历史提交数据作为当前基准。Android 真机的功能、帧耗时、
功耗及尾延迟尚未验证，由调用方后续在目标环境完成；不承诺当前版本的端到端性能提升。
