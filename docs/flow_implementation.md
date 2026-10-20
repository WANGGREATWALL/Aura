# au::flow 实现进度与验证

## 0 范围

权威设计为 [Obsidian 设计文档](</srv/data/file/code/Obsidian/7 项目/7 taskflow/3 设计文档.md>). 本文件记录代码交付与验收边界, 不缩减或替换该设计. CPU 调度, mm 访问协议, 两套 OpenCL wrapper, cache 与设备资源生命周期均已接入同一完成契约. 接入新 wrapper 的异步 GPU 提交现在由 Task reservation 跟踪至设备和主机清理完成. 绕过 wrapper 的原生调用仍须自行接入协议.

实现起点为 dev 的 `4438d46`, 后续改动在此之上继续实现. 原有 `src/flow/xthread_flow.h` 保留. 新代码从公开 `inc/flow/xflow.h` 进入, `inc/flow/xthread_flow.h` 提供历史 include 路径. 旧线程池和归档 TaskFlow 不参与新调度器.

## 1 代码入口

| 入口 | 内容 |
| --- | --- |
| `inc/flow/xflow_api.h` | C 兼容窄腰, ABI 版本, opaque handle, 结果与容量查询 |
| `inc/flow/xflow.h` | C++11 Task/Tasks/XFlow 外观, callable 绑定与旧宏 |
| `inc/flow/xflow_types.h` | 轻量状态/结果/策略类型, 完成桥不引入公开任务模板 |
| `src/flow/xflow.cpp` | C++17 状态机, 原子接受, 依赖, 结构化完成, worker 与等待 |
| `src/flow/diagnostics.*` | 无调度器指针的图快照, DOT/trace 格式化与文件导出, 可选 trace 存储 |
| `src/flow/ready_queue.h` | 256 槽 MPMC 快路径和侵入式 MPSC 溢出 |
| `src/flow/slab.h` | 稳定地址页, Task/edge/external 初始页分别为 128/256/64 |
| `src/flow/completion.h` | 外部完成与 GPU profiling 协议, 无 OpenCL 类型 |
| `src/flow/payload_pool.h` | 64/256/1024/4096 字节闭包复用槽, 大对象及过对齐慢路径 |
| `inc/gpu/xopencl*`, `ximage_args.h` | 两套 C++11 wrapper/独立 Launch 和 C 窄腰 |
| `src/gpu/pending.*`, `opencl.cpp` | 查询完成, standalone registry, 稳定资源缓存, program/kernel 复用与传输 |
| `src/gpu/program_cache.*`, `src/sys/atomic_file.*` | 可选设备 binary 缓存, 完整 metadata 校验, 原子替换 |
| `src/sys/opencl_memory.*` | 能力探测及 ARM DMA-BUF 导入, 不猜测厂商 |
| `src/flow/trace_buffer.h` | 按需初始化的固定 trace 存储, 导出时不覆盖正在写入的记录 |
| `src/sys/flow_runtime.*` | 工作许可, CPU 集合, 原生线程 ID 和亲核性 |
| `inc/mm/xaccess*`, `inc/cv/ximage_access.h` | C/C++11 访问守卫, 外部内存注册与图像描述适配 |
| `src/mm/access.*`, `memory_control.*`, `registry.*` | 分配身份, 集合 reservation, 别名合并, fence 快照, 租约与延迟释放 |
| `src/sys/memory_runtime.*`, `memory_allocation.cpp` | DMA START/END, CLOEXEC fd, 原生身份, PSS 与 dma-heap 分配 |
| `src/flow/testing.h` | 私有故障注入与快照边界测试接口, 非业务 API |

## 2 已实现的主机协议

- 获取 XFlow facade 不建线程. 接受工作时先保证一个 worker 可用, 后续按就绪需求扩展和复用, 最多 8 个. 无空闲自动销毁.
- 普通任务只接受实际调用结果为 int 的 callable. 参数默认复制/移动, `std::ref/std::cref` 显式借用. 任意非零返回值失败, 不改变 XImage 的借用契约.
- Task 的共享身份使用侵入式引用计数; 接受记录, 依赖边, 运行栈, 父作用域和外部记录持有各自引用. 引用归零后的连锁回收用迭代链, 不按图深度递归析构.
- Tasks 具有入口 gate 和汇总身份, 支持嵌套. 接受前取得记录与边, 验证 Draft 外部前驱和组合环, 接受后冻结目标依赖. 提交 latch 阻止注册期间提前执行.
- 前驱后继链只入不出, 最终结果发布后原子关闭链表. 注册者与关闭者竞争时, 每条边只解析一次.
- 主体 reservation 覆盖嵌套 enqueue 和外部完成记录. 主体先返回时进入 Waiting, 已开始工作全部清理后才最终完成. 子提交失败可登记到父作用域.
- worker 的 wait 协作推进其他就绪工作, 检测自身, 祖先, 簇包含, 已知等待环与协作栈依赖. waitAll 建立根接受序号快照并覆盖后续结构化后代. worker 内拒绝 waitAll/shutdown.
- tiled 最多 4 个参与者, 包含调用线程. 外部调用建立受跟踪的内联根作用域, 调用者参与计算, helper 最多 3 个; 快照和 shutdown 覆盖调用者及其后代. helper 每次完成一个 tile 后重新排队, 不逐 tile 建 Task/future. helper 准备或提交失败时退化为调用者计算, 不污染父任务结果; 必要根记录失败仍返回真实错误. 内层 tiled 和过深协作栈串行退化, 返回前等待所有 helper.
- 结果保留状态, 来源, 原始 int/native code 和根因 Task ID. 主体返回值与已登记访问/外部错误相同时保留其详细来源; 不同业务返回值仍优先. 返回业务值 -5004 与实际未处理异常的来源可以区分. 失败依赖跳过节点, 独立兄弟继续收尾.
- shutdown 拒绝新的外部根, 允许已接受作用域继续提交后代, 排空后 join. 已完成句柄在实例存活期间继续可查询, 不支持关闭后重启.
- DOT 输出结构; 主机 trace 输出 Perfetto 可读 JSON. trace 默认关闭且不分配记录存储, 开启后固定容量记录, 满时统计丢失轨迹而不丢任务. GPU profiling 已接入独立设备时钟字段; 主机 callable wall 包含等待, 不冒充 CPU 利用率.

## 2.1 内存访问协议的当前实现

```cpp
#include "cv/ximage_access.h"
#include "flow/xflow.h"

au::mm::CpuAccessScope access;
int ret = access.begin({au::cv::readAccess(src), au::cv::writeAccess(dst)});
if (ret != 0) return ret;
ret = au::flow::XFlow::get().parallelizeTiledTasks(rows, tile, processRows);
return access.finish(ret);
```

- begin 先校验和解析所有范围, 保留实际存储及各别名映射 owner, 再按 allocation ID 排序合并 Read/Write 为 ReadWrite. v1 按整个分配管理冲突.
- CPU Read 只共享已完成 START 的只读 session, 最后一个 reader 执行 END. 写冲突或 START/END 过渡状态返回 Busy. 集合 reservation 成功后才调用 sys, 任一步失败逆序 rollback.
- 等待设备 fence 时不持有资源锁或 CPU reservation. 等待后检查版本, 若有后续设备提交则重新等待新 fence, 最多 8 次竞争重试后返回 Busy. 这不是业务任务队列容量限制.
- 外部注册要求精确基址与容量. DMA fd 以 CLOEXEC 副本和文件对象身份管理; 相同 fd 对象的不同完整映射必须有相同容量与类型. ROI 使用该已登记映射的内部指针. 借用 mapping 仍由调用方保活, 拥有模式通过 retain/release 保留 owner.
- free 只退休 Aura 自有分配; 外部注册用 ExternalMemory::reset/au_mm_unregister. 新范围发现立即失效, 在途 lease 仍然有效. 观察到的延迟物理释放错误会登记到当前执行上下文.
- legacy syncDeviceToCpu/syncCpuToDevice 在调用期间保活并排斥活动 guard; 它们只覆盖该次操作. 使用 legacy 入口的调用方必须自己串行化整个 CPU 区间, 不能把两个方向函数当作自动访问守卫.
- mm 通过线程局部 ErrorSink 报告 cache 或设备 fence 错误, 不包含 flow/OpenCL 头. flow 为 callable 和清理阶段安装并恢复 ErrorSink, 保留 native code 与可用的根因 Task ID.
- worker 与主机 fence 等待触发的清理统一进入原始任务的 CleanupExtent, 对 parent 单独 pin. standalone 清理隐藏调用者的 Task 上下文. wait/waitAll/shutdown/新提交/tiled/后端 finish/deinit 拒绝重入; mm 只允许直接可用的访问, 不在清理中等待未完成 fence. 错误不归到碰巧执行清理的其他 Task.

## 3 查询完成与 wrapper 接入

2026-10-20 用户确认 v1 采用查询完成. 原生 OpenCL 完成回调没有可 join 的返回栈边界, 不用双 pin 归零推断卸载安全. `completion.h` 只保留查询、发布与清理协议, 已裁剪无生产调用者的 callback pin 支持. 两套 OpenCL wrapper 均不调用 clSetEventCallback.

1. native enqueue 前准备 Pending, 完成状态, mm fence, staging, 资源/事件引用和 flow token. 任一准备失败不发送设备命令.
2. commit mm fence 后将最后一个有效 event 发布到预备侵入式 pending 链, 再结束 registrar. 无论调用方是否需要 event, 内部都持有它.
3. 现有 worker, CPU fence 等待和 wrapper 普通入口公平查询. atomic once/finalizing 所有权防止重复清理, registry 锁内不查询驱动或运行资源析构.
4. `clGetEventInfo` 只确认终结状态; 其后调用 clWaitForEvents 建立主机同步, 再消费 staging/readback 数据. query 失败保留记录, 不伪造终结.
5. 部分 enqueue 或 flush 失败仍提交最后一个有效 event, 保留租约直到设备终结. 完成清理先释放资源, 再公布 fence 安全状态, 最后由 flow 消费 token 和递减 Task reservation.
6. fence 只强持轻量状态及 weak Pending, 不把资源租约循环持回 MemoryControl. 即使 Task/事件句柄提前丢弃, registry/token 仍保活工作.
7. 管理 worker 在资源等待时有界协作推进就绪工作, 单 worker 模型验证另一个任务可以解开完成条件. 外部线程不执行调度工作, 清理阶段不帮助执行任务.
8. flow 外通过提交, CPU 访问, finish/deinit 推进 pending; 没有隐藏 dispatcher. 没有在途事件时不定时唤醒. 最后一批 standalone 记录可以保留到下次入口.

两套 wrapper 独立持有 context/queue, 只共享实现. CLWrapper 支持 Buffer/Image2D; OpenCLWrapper 接受 DMA buffer, 按实际扩展决定 ARM 直接导入或标准 staging. 不把 dma-heap fd 直接假定为 QCOM ION. 图像 Image/XImage 仍按原有契约借用; 提交后由 mm 内部租约保护存储.

每个 Launch 独立保存 NDRange. 相同源码/选项共享 compiled program, 各 kernel 名称保持独立实例池和不可变名称绑定, kernel 从 setArg 到命令终结独占并复用; 每次校验 CL_KERNEL_NUM_ARGS, 防止漏参数复用旧绑定. Buffer 绑定整个 allocation, ROI/plane 的字节 offset 由 bufferOffset 查询后显式传给 kernel. 同一提交的相同分配合并; 不兼容的 Buffer/Image, 不同 Image 视图或 CL1.2 可写 Image 多参数别名明确拒绝. 只读 Image 别名可共享对象. 默认 local 为空由 runtime 选择; 显式 local 必须整除 global, 不隐式扩展用户范围.

staging buffer 写入前保留整个 allocation, 防止 ROI/padding 被未初始化字节覆盖; Image 使用紧密排列的 staging, 按有效行复制并保留调用者的 stride padding; 覆盖最后一行不含 padding 的最小合法范围. 覆盖所有字节的 host copy 前不重复清零 staging. DMA staging 的 CPU 读写分别成对 START/END, END 失败污染该分配的后续访问. ARM 导入无冗余 host copy, CPU guard 等待 fence 后执行相应 START/END.

clEnqueue 报成功却丢失请求 event 时, 记录不会被当作未提交而释放. 故障路径以 queue finish 取得独立完成证明, 未获证明则保留资源. 失败资源向后续 CPU/GPU 访问传播原 GPU 根因.

可选 nativeContext/nativeMemory/nativeEvent 属于底层互操作; 原生内存被保活并校验 context, CPU cache 和直接原生提交仍由互操作调用方负责. nativeEvent 只代表设备完成, Task/fence/finish 包含 host 清理. 返回的事件用对应 wrapper 的 releaseEvent 释放.

资源缓存使用 `(owner, allocationId, allocationGeneration, contextGeneration, nativeView)` 键, Buffer 对整个 allocation 复用, Image 还包含 offset/尺寸/格式. 缓存只弱引用 MemoryControl, DMA 导入对象独立持有 CLOEXEC fd 副本. 每次普通入口和完成阶段有界检查 16 个条目, 在缓存锁外释放已失效对象; deinit 释放本代全部缓存. 不用缓存长期保活主机 allocation, 不通过 fd 数字判断分配相同.

同一 context 的纯 DMA 直接导入链用 event wait-list 提交, 不先等待 host completion. 目前这一快路径用于 OpenCLWrapper/ARM 导入; 普通 staging, Image 和跨 context 仍在取得任何 reservation 前桥接. fence 的原生事件租约不含 mm 存储租约. 后继自己的设备终结还不够: 查询需观察所有前驱记录已安全清理, 才公布完成并保留原始根因. 公平查询推进记录, 不递归沿整条 fence 链等待.

`init(binaryDirectory, profiling)` 的目录参数现在启用可选磁盘缓存, 空目录禁用. 键包含平台/设备/驱动/OpenCL C 版本, vendor/address bits, 完整源码和编译选项. 文件包含版本, 精确 metadata, 长度和校验和; hash 只用于文件定位, 不是身份或认证依据. 驱动拒绝 binary, 内容损坏, 截断, metadata 不符或磁盘不可用均回到源码路径. 通过 au::file 读取, sys 在同目录独占创建临时文件并原子替换; 不承诺掉电持久化/fsync, 目录应由应用管理. 冷编译与文件 I/O 不持有缓存锁. 并发冷 miss 允许重复编译, 发布时只保留一个 immutable program.

GPU trace 在终结后查询 kernel event 的 queued/submit/start/end, 记录独立设备时钟和 host 提交/观察边界. 未启用或查询失败显式 unavailable, 不影响计算结果. 没有设备/主机校准时不生成精确对齐的 GPU 时间段.

## 4 实现覆盖与外部验收

| 要求 | 实现与证据 |
| --- | --- |
| callable 存储 | 尺寸池复用, 大/过对齐闭包与构造失败回收; 提交冷路径仍可能分配 |
| 弹性队列 | 8-bit 缩小模型覆盖票号耗尽, CAS 前跨完整票号空间的暂停, ring/overflow 发布间隙, 消费门控和立即复用; 不宣称 wait-free |
| 等待与关闭 | 交叉 wait 环, 4096 节点深图, 全 worker 嵌套 tiled, 快照后代, 关闭并发提交/后代排空, reservation/order 溢出注入 |
| MemoryControl | 范围视图, owner/id/generation, 集合别名合并, 外部注册和 fd 复用; GPU 缓存消费同一稳定身份 |
| CPU access | Read/Write/ReadWrite, 有序 reservation, 只读共享, Busy 冲突, START/END 配对, 逆序 rollback 和错误登记 |
| 内存生命周期 | 租约延迟释放, 外部 retain/release, alias mapping 独立保活; Native 后端无常驻堆单例, 分配平台差异统一在 sys |
| cv 适配 | readAccess/writeAccess/readWriteAccess, 格式/stride 与真实容量校验; tiled 借用外层 session, 保留 XImage 浅借用契约 |
| 两套 wrapper | C++11/C ABI, 独立 Launch, 查询完成, Buffer/Image 或 DMA Buffer, kernel 池, 源码共享和 binary 缓存 |
| 自动 cache | staging 与 ARM DMA 导入; 同函数 GPU -> CPU, 同 context 原生链, 跨 context 主机桥接及原始根因 |
| standalone GPU | intrusive pending registry, CPU fence 等待, finish/deinit 排空和重入拒绝, 正常退出顺序; 无新增常驻线程 |
| GPU 构建 | ENABLE_OPENCL 可选动态加载 CL1.2, OS/厂商代码在 sys; CPU-only 返回明确 NotSupported |
| profiling/DOT | 名称/父子, 嵌套簇边界, 动态子工作保留, GPU 独立时钟和 unavailable, 已知等待分段, 丢失计数 |
| 平台/性能 | Linux 主机功能, ABI, sanitizer 和成本采样已运行; Android QCOM/MTK, macOS, Windows 编译/真机, cache 一致性及能耗待外部设备验收 |

公开接口以 `inc/` 的实际声明为准. 设计中的 GPU read/write 草案在 v1 落为 `au::gpu::buffer(au::mm::readAccess(...))`, `writeAccess/readWriteAccess` 同理; Image 显式提供 format/尺寸/stride, `au::gpu::buf(Image/XImage, AccessMode)` 是便捷适配. Buffer 视图 offset 显式通过 `bufferOffset` 传给对应 kernel. 不把设计示例的任意 kernel 签名标成已经编译的业务算子.

trace 默认关闭. 开启时保留动态子节点诊断边, 后代继承该结构捕获选择; 不由诊断分配失败拒绝计算, 丢失数体现在 DOT. 已完成子节点清除 parent 引用后由汇总句柄保留必要图/结果, 闭包先释放. DOT 为逐节点截取的快照, 不承诺整个动态图的单一线性化时刻.

轨迹中的 `callable wall` 包含函数等待; `CPU slice` 在已知 flow wait, GPU fence 等待和协作执行边界分段, 无法识别任意用户 I/O/锁等待或 OS 抢占. 这不是 OS on-CPU 利用率测量. 标签最多复制 63 字节, 不截断有效 UTF-8 码点, 导出标记 truncated; DOT 保留完整名称. 切换采集的边界可能形成部分片段.

本机 x86_64 实测 `sizeof(TraceSlot)=192`, 9 个 8192 记录 buffer 合计 14,155,920 字节, 仅首次启用 trace 时分配并保持到 scheduler 销毁. 满时丢记录而不丢任务. pool 扩页/闭包绑定, GPU pending 与 staging 准备, 导入/编译/磁盘缓存, 建线程, cache ioctl 与 native 提交均有实际成本, 不宣称整个 enqueue 稳态零分配或固定延迟.

## 5 验证入口

```sh
cmake -S . -B /tmp/aura-flow-build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TEST_XFLOW=ON
cmake --build /tmp/aura-flow-build -j 8
ctest --test-dir /tmp/aura-flow-build --output-on-failure
/tmp/aura-flow-build/aura_test --gtest_brief=1
```

CTest 分别验证 flow 单测, mm 租约与分配测试, 严格 `-std=c++11` 消费端, 首 worker 建立失败, C 消费端, 两个算法 `.so` 的实例/句柄隔离. 隔离测试使用 RTLD_GLOBAL, 覆盖外来 retain/release 不修改原实例引用计数, 另检查本地符号表确认 Aura 状态/TLS 没有作为可抢占动态符号导出.

`ENABLE_ASAN`/`ENABLE_TSAN` 现在覆盖 aura 库本体及 flow 测试消费端, 不能同时开启. ASan 的 leak 扫描需要允许检查线程的运行环境; 不应通过关闭 leak 检测来标记通过. 新增的外部模型和后续修改仍须逐次重跑, 以最新日志为准.

2026-10-20, Linux OpenCL 构建已通过 158 个单测 (17 个 suite), 11 项 CTest 通过, 1 项真实 GPU 验收明确 skipped (无 platform). CPU-only 8/8 CTest, TSan 和 ASan/LSan 均通过 11/11 CTest; LSan 在沙箱外运行且未关闭 leak 扫描. Release BUILD_TESTING=OFF 已构建. 最终重跑包含 fd 复用/跨 context 桥接, UTF-8 标签截短和外来 retain/release. 不把已通过模型等同真机结论.

严格 C/C++11 消费端及 RTLD_GLOBAL 双 .so 模型覆盖 facade, worker TLS, mm 外来句柄拒绝, 原生分配后端释放和独立 GPU backend 身份. GPU 模型覆盖 Buffer/Image/stride, 同函数 GPU -> CPU, 单 worker 协作, kernel 参数隔离, cache START/END 故障, 部分提交/flush/query/丢失 event, 原始根因, 清理重入和退出顺序. 8-bit 队列测试覆盖完整票号空间跨越前的暂停, 耗尽后继续提交及节点复用.

当前功能证据来自 Linux 主机与 native API 模型. 没有可用 OpenCL platform; 不能据此宣称真实设备 cache 一致性, Android 能耗, macOS/Windows 可编译性或真机 .so 卸载已验证. 仓库仍需在拥有这些环境的 SDK/设备上执行验收矩阵.

### 5.1 主机成本测量入口

`ENABLE_FLOW_BENCHMARKS=ON` 构建 `flow_benchmark`, 不加入 CTest 性能断言. Release/BUILD_TESTING=OFF, GCC 15.2.0, Intel i7-8550U Linux 主机, 沿用系统 affinity. 每组 7 次/512 个任务, 分别报告 min/median/max 和首次批次; 另覆盖 1920x1080 内存循环的 1/16/64 行 tile. 关闭独立 perf 日志和 trace_marker 输出以观察二进制 trace 成本.

2026-10-20 的一组参考样本 (us, 除注明外为每任务):

| 项目 | min | median | max |
| --- | ---: | ---: | ---: |
| 预热, trace off | 3.665 | 3.962 | 4.465 |
| 预热, trace on | 4.523 | 4.790 | 4.903 |

首次 512 个任务合计 3.051 ms; 简单内存循环的 1/16/64 行 tile 分别约 730/716/602 us 每帧. 这组数据包含调度, 等待和句柄释放, 与真实图像算子及平台差异分开解释. 7 次样本不能推导 p95/p99 或性能 SLA.

本次没有锁定 CPU 频率/温控, 后台构建会影响结果. 原始样本在 `/tmp/aura-flow-host-measurements.log`; 数值只用于确认测量入口与成本量级, 不比较为产品性能结论. 尚未完成 Taskflow/旧实现对比, p95/p99, 多帧图像/GPU 与能耗实验; 这些需要目标环境和真实算子负载.

## 6 算法参考

- Taskflow 3.11.0, 固定提交 `816b4ad53b44196c88f409eb7b4a25a0e3bfdf42`, `taskflow/utility/mpmc.hpp` 与 core dependent-async 协议. 没有链接 Taskflow. MIT 通知见 `third_party/flow_queue_references/`.
- [gRPC MPSC](https://github.com/grpc/grpc/blob/master/src/core/util/mpscq.cc) 和 Dmitry Vyukov 的侵入式 MPSC/sequence ring 算法. Aura 自行组织有界尝试, 溢出消费门控和工作许可, 不引入 gRPC/Abseil. 原作者许可一并保留, 后续直接移植应固定对应来源提交.

ReadyQueue 的短暂 pop 失败不证明为空; 一个线程暂停在预留或 exchange/link 之间仍能延迟相应路径的进度. 不宣称整套调度器 wait-free, 不以“无锁”推导图像算子一定更快.

DMA 协议依据: [Linux DMA-BUF CPU access](https://docs.kernel.org/driver-api/dma-buf.html#cpu-access-to-dma-buffer-objects). 完成与主机同步依据: [clGetEventInfo](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clGetEventInfo.html). 查询方案已获用户确认, 不使用原生 CL 回调作为卸载协议.

## 7 对比评审后的采纳 (2026-10-20)

参考 Claude/Sonnet 方案的设计思想, 保留原有完成与资源契约, 采用以下三项改进:

- `Task/Tasks` 增加 `ready()/code()/wait()/waitFor()`. `code()` 不阻塞, 未完成返回 NotReady; 句柄 `wait*` 在等待成功后返回任务的原始错误码, 可同时取得 TaskResult. 等待超时/非法等待不改变 out. 后续第 8 节将 XFlow 的 C++ 等待语义一并统一. Draft、空句柄与拒绝结果各自保留原有错误语义.
- 外部 tiled 调用者参与计算, 每次仍最多 4 个参与者, 包含 caller. caller 不套用 worker 的亲核性策略. 内联根记录保证只用 caller 的循环也参与 waitAll 快照、shutdown 和结构化后代完成; 不把调用者执行藏在仅统计 helper 的屏障外. 关闭后不接受新的外部循环.
- worker 在完成任务并仅释放一个就绪后继时, 可迭代直接续跑该工作, 避免一次入队和许可通知. 每次 dispatch 最多连续 8 个任务; 已排队工作、挂起设备查询、tiled helper 均不进入该快路径. fan-out 保持正常排队, 每个直接后继仍遵循亲核性预算、完整清理和错误传播. 未引入工作窃取或 GPU DAG 提前提交.

空闲 worker 退休暂不采用: 现有 worker ID/TID、亲核性查询与 trace 使用稳定槽位. 引入退休需要独立处理接受提交与最后 worker 退出的竞争, 锁外 join、代际及查询 pin; 应先测量 Android 驻留与首帧收益. 同样不移植原生 GPU callback、`.gpu()` 提前提交或方向式 cache hook.

新增验证覆盖句柄结果/超时、外部 caller 实际参与、首 helper 分配/建线程失败、managed 父任务串行退化、无 helper 的快照/关闭、关闭期间内联根后代提交, 以及连续后继让出队列和失败传播. 首次内联根登记异步完成前确保存在完成消费者, 不因尚无 worker 而停滞; 外部 caller 不计入忙碌 worker 的扩容估计.

最终版本验证 (2026-10-20 | 14:53): 全量 162/162 单测, 17 个 suite; CPU-only CTest 8/8; OpenCL CTest 11 项通过、1 项真实 GPU skipped; TSan 11/11; ASan/LSan 11/11, 未关闭泄漏扫描. 另通过 C/C++11 消费端、首次 worker 失败、关闭及双 .so 隔离. git diff --check 通过. 当前功能验证使用 Linux 主机与模型环境, 没有目标 GPU platform 或 Android 真机验收, 不据此声称设备性能提升.

## 8 接口与内部职责整理

- 所有 C++ `Task/Tasks/XFlow::wait/waitFor` 统一返回任务错误码; 超时或非法等待返回等待错误, 不更新 out. 获得终态时, 无论成功或失败都填写完整 TaskResult. `waitAll()` 继续作为根快照屏障, 不汇总历史业务错误.
- **行为迁移**: 原来检查 `flow.wait(task) == 0` 只表示等到终态, 现在同时表示任务成功. 需要错误来源时传 TaskResult. 底层 C ABI `au_flow_wait` 保持等待状态与 `out.mCode` 分离的既有语义, ABI 版本不变; 新 C++ 行为需重新编译消费端.
- `Tasks{a, b, ...}` 复用原有 builder. 任一成员无效、重复、构建失败或已经归属其他簇, 都令整个 builder 失败, 不运行半个簇. 不接受用该构造器收集已经提交的任务; 嵌套 Tasks 继续用 add(std::move(child)) 明确转移 builder 所有权.
- `enqueueAfter(taskOrGroup, callable, args...)` 封装创建、建边和提交, 支持参数复制/移动/显式借用. 前驱必须已接受; 未提交的前驱应与目标一起加入 Tasks. 建边失败记入目标 builder, 前驱执行失败仍按依赖规则跳过目标并保留根因.
- 公开基础类型移至 xflow_types.h. GPU pending 仅依赖轻量完成协议; GPU 关闭路径显式包含 C ABI 头, 不再经任务模板的传递 include 获得声明.
- `Core` 负责采集图状态, 交给 diagnostics 的是独立值快照, 不含 Node 指针或 NodeRef. DOT/JSON 格式化、文件 I/O 和 trace 存储归 diagnostics, 不在图锁内执行. trace 仍默认关闭, 热路径不增加诊断分配.
- 查询路径移除 callback 计数、arm/leave 接口及重复的 polling 标志. 已发布查询始终经同一完成路径处理, registrar 保证准备者最后一次访问之前不会清理记录. 非终态查询失败仍保活, 不改变资源与卸载契约.

常见用法:

```cpp
auto& flow = au::flow::XFlow::get();
auto a = flow.makeTask(first);
auto b = flow.makeTask(second);
int ret = a.precede(b);
if (ret) return ret;
au::flow::Tasks group{a, b};
auto submitted = flow.enqueue(group);
if (submitted.submissionCode()) return submitted.submissionCode();
auto tail = flow.enqueueAfter(submitted, last);
const int tailCode = tail.wait();
const int groupCode = submitted.wait(); // Also join the group if tail submission failed.
return tailCode ? tailCode : groupCode;
```

调用者仍须保证借用对象存活到所有已接受的工作结束. 例如最后一个任务提交失败时, `enqueueAfter` 不撤销前驱, 持有本地借用资源的代码需等待 `submitted`, 再返回主要提交错误.

本轮整理验收 (2026-10-20 | 15:28): 全量 163/163 单测, 17 个 suite; CPU-only 8/8; OpenCL 11 项通过、1 项真实 GPU skipped; TSan 11/11; ASan/LSan 11/11 (含泄漏扫描). 严格 C++11/C11 的 -Wall -Wextra -Werror -pedantic 语法检查通过, C ABI 失败任务的等待状态保持兼容. 最终编译依赖确认 GPU 不再包含 xflow.h 任务模板. 未进行新性能对测或 Android 真机验收.
