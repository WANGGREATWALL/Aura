# au::flow 表达式接口（dev，C++11）

`Task` 表示一次工作及其结果，`Tasks` 表示共享任务节点组成的片段，`Flow` 创建和提交工作。
所有 C++ `enqueue` 重载均返回 `Task`，通过 `wait()`、`code()` 和 `done()` 观察结果。
本次为 1.7.0 之后的 C++ 源码兼容变更；已发布的 1.7.0 标签不变。

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

## 构图规则

- `Tasks` 保存入口集合 `heads`、单个出口 `last` 和首个构建错误。
  入口是“为整个片段添加前驱时需要连接的节点”，不要求节点在运行时图中没有其他前驱。
- `A >> B` 立即建立 `A.last → B.heads`，不创建任务；结果保留 A 的入口、B 的出口。
- 多分支并行立即建立 `各分支.last → 新汇合任务`，入口取并集；没有公共入口任务。
  汇合任务是普通空任务，照常排队和执行，首版不做原地完成优化。
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

Task 不隐式转换为 int。使用 `auto` 保存旧提交返回值的调用点也要检查，不能只依赖编译错误定位。
C 函数仍返回原来的接受状态码；Group C ABI、内部组归属校验和兼容行为保留。
新的 C++ Tasks 不使用旧 Group。没有新增 C 符号，也没有修改 GPU/MM/CV。

## 云主机微基准

以下为表达式版本 `0bedf73` 的历史基线，不代表后续核心优化后的性能。

2026-10-26，Linux x86_64，GCC 14.2.0 `-O3 -std=c++11`，Release 库，同一调度核心，未手动固定亲核。
旧外观固定为 `0ab5b63`。旧、新程序交替运行 7 轮，每种构图 10 次预热后采样 101 次；
下表取各轮 p50 的中位数，单位 μs。输入任务提前创建，构图计时不含其创建、执行和析构。
运行阶段另测提交至等待完成；等待线程可能协作执行。

| 分支数 | 1.7.0 Group 构图 | 连续二元 `|` | `parallel` 构图 | 旧 / 新扁平提交至完成 |
| --- | ---: | ---: | ---: | ---: |
| 4 | 1.51 | 2.28 | 0.98 | 27.23 / 22.40 |
| 16 | 9.31 | 30.61 | 4.66 | 77.25 / 62.56 |
| 64 | 29.05 | 403.16 | 18.02 | 177.03 / 141.07 |
| 256 | 134.26 | 6643.39 | 105.55 | 369.21 / 286.92 |

4 分支初始化列表构图 p50 为 0.89 μs，与区间使用同一实现。
预创建单任务 enqueue→wait 的 p50 为旧 0.19 / 新 0.35 μs；新 Task→Tasks 转换可能分配入口 vector。
直接 `enqueue(fn,args...)` 不构造该入口集合。二元组合每增加一个分支增加一个汇合任务，
入口复制和环检测也随图增长；大量分支应使用扁平列表或区间。

相邻普通任务的“前驱函数结束前打点→后继函数开始打点”p50 为旧 0.54 / 新 0.33 μs，
p95 为 19.97 / 22.25 μs。该探针包含完成传播和任务交接，不是自动汇合节点的纯队列延迟；
两版调度核心相同，不能据此声称核心变快。空任务提交等待的 p95 也约 37–41 μs，主机调度噪声明显。
这些数据用于识别拓扑和构图成本，不代替 Android 真机功耗、帧耗时和尾延迟验收，
目前不足以支持增加汇合原地完成机制。临时基准与分配失败探针未编入产品库。
