# au::flow 实现进度与验证

## 0 范围

权威设计为 [Obsidian 设计文档](</srv/data/file/code/Obsidian/7 项目/7 taskflow/3 设计文档.md>). 本文件记录代码交付与剩余验收, 不缩减或替换该设计. 完整目标仍包括 mm 访问协议, 两套 OpenCL wrapper, cache 与设备资源生命周期. 当前实现尚不能把原有异步 GPU 函数的返回等同于 Task 完成.

原有 `src/flow/xthread_flow.h` 的用户修改保留. 新代码从公开 `inc/flow/xflow.h` 进入, `inc/flow/xthread_flow.h` 提供历史 include 路径. 旧线程池和归档 TaskFlow 不参与新调度器.

## 1 代码入口

| 入口 | 内容 |
| --- | --- |
| `inc/flow/xflow_api.h` | C 兼容窄腰, ABI 版本, opaque handle, 结果与容量查询 |
| `inc/flow/xflow.h` | C++11 Task/Tasks/XFlow 外观, callable 绑定与旧宏 |
| `src/flow/xflow.cpp` | C++17 状态机, 原子接受, 依赖, 结构化完成, worker 与等待 |
| `src/flow/ready_queue.h` | 256 槽 MPMC 快路径和侵入式 MPSC 溢出 |
| `src/flow/slab.h` | 稳定地址页, Task/edge/external 初始页分别为 128/256/64 |
| `src/flow/completion.h` | 尚待 wrapper 接入的外部完成协议, 无 OpenCL 类型 |
| `src/flow/trace_buffer.h` | 按需初始化的固定 trace 存储, 导出时不覆盖正在写入的记录 |
| `src/sys/flow_runtime.*` | 工作许可, CPU 集合, 原生线程 ID 和亲核性 |
| `src/mm/memory_control.*`, `registry.*` | 分配身份, 内部视图解析, 租约与延迟释放基础 |
| `src/flow/testing.h` | 私有故障注入与快照边界测试接口, 非业务 API |

## 2 已实现的主机协议

- 获取 XFlow facade 不建线程. 接受工作时先保证一个 worker 可用, 后续按就绪需求扩展和复用, 最多 8 个. 无空闲自动销毁.
- 普通任务只接受实际调用结果为 int 的 callable. 参数默认复制/移动, `std::ref/std::cref` 显式借用. 任意非零返回值失败, 不改变 XImage 的借用契约.
- Task 的共享身份使用侵入式引用计数; 接受记录, 依赖边, 运行栈, 父作用域和外部记录持有各自引用. 引用归零后的连锁回收用迭代链, 不按图深度递归析构.
- Tasks 具有入口 gate 和汇总身份, 支持嵌套. 接受前取得记录与边, 验证 Draft 外部前驱和组合环, 接受后冻结目标依赖. 提交 latch 阻止注册期间提前执行.
- 前驱后继链只入不出, 最终结果发布后原子关闭链表. 注册者与关闭者竞争时, 每条边只解析一次.
- 主体 reservation 覆盖嵌套 enqueue 和外部完成记录. 主体先返回时进入 Waiting, 已开始工作全部清理后才最终完成. 子提交失败可登记到父作用域.
- worker 的 wait 协作推进其他就绪工作, 检测自身, 祖先, 簇包含, 已知等待环与协作栈依赖. waitAll 建立根接受序号快照并覆盖后续结构化后代. worker 内拒绝 waitAll/shutdown.
- tiled 最多 4 个参与者, 含父 worker. helper 每次完成一个 tile 后重新排队, 不逐 tile 建 Task/future. 内层 tiled 和过深协作栈串行退化, 父线程能独立领取全部区间. 返回前等待所有 helper.
- 结果保留状态, 来源, 原始 int/native code 和根因 Task ID. 返回业务值 -5004 与实际未处理异常的来源可以区分. 失败依赖跳过节点, 独立兄弟继续收尾.
- shutdown 拒绝新的外部根, 允许已接受作用域继续提交后代, 排空后 join. 已完成句柄在实例存活期间继续可查询, 不支持关闭后重启.
- DOT 输出结构; 主机 trace 输出 Perfetto 可读 JSON. trace 默认关闭且不分配记录存储, 开启后固定容量记录, 满时统计丢失轨迹而不丢任务. GPU profiling 尚未接入.

## 3 外部完成接入契约

`completion.h` 是私有适配协议. 当前已用模型 provider 验证, 不代表两套 wrapper 已经改造完成.

1. native enqueue 前调用 prepareExternal. 成功且 token 非空才转移 payload, 增加当前 Task reservation. token 为空表示 standalone, 仍由 provider 自己管理在途资源.
2. 登记 native callback 前 arm callback pin. 即使回调同步进入, registrar pin 也阻止提前清理.
3. 回调只 notify 和离开 callback pin. 重复通知通过原子 once 协议抑制. payload 清理和后继传播留给正常 worker.
4. 回调注册失败时撤销不会被调用的 callback pin, 启用预备记录的 fallback polling. 不立即释放 event 或资源.
5. registrar 返回前释放其 pin. 查询错误不能证明设备已停止, provider 只有确认终结才能设置 done. 已发布状态且两类 pin 均退出后, 才执行 finalize/release 和递减 Task reservation.
6. 延迟记录按 FIFO 轮询, 驱动查询与 payload 释放在锁外. 无额外 dispatcher 线程; 仅存在延迟记录时使用短超时复查.

wrapper 仍须提供 event/fence 访问, 资源租约, flush 失败处理, 设备错误与模块卸载协议. 当前模型测试不证明原生 OpenCL callback 返回栈或 Android cache 行为.

## 4 完整目标的剩余工作

| 要求 | 当前状态与下一步 |
| --- | --- |
| callable 存储池 | Node/edge/event 已池化, 闭包仍由 C++11 适配层构造和释放; 需加入按尺寸/对齐复用, 保留大闭包弹性支持 |
| 原子进度证明 | 基本压力与 TSan 已运行; 仍需暂停发布者, 缩小位宽回绕, 门控竞争和回收模型测试 |
| 更多等待故障 | 增补交叉 wait 环, 深图/深协作, 关闭与同时提交压力, 完成计数溢出注入 |
| MemoryControl | Aura 分配已使用稳定 allocation ID/generation, 内部范围视图共享同一身份; 访问集合合并, 外部注册和 fd 重用防护仍待实现 |
| CPU access | 尚未实现 Read/Write/ReadWrite 守卫, 有序集合 reservation, START/END 配对, rollback 与访问错误登记 |
| 内存生命周期 | mm 分配已支持内部租约延迟物理释放, 同步兼容入口在调用期间保活; GPU 导入/fence, 外部 mapping/池租约仍待接入, XImage 复制继续借用 |
| cv 适配 | 尚未实现 readAccess/writeAccess, plane/ROI/stride/容量校验与 tiled 访问能力 |
| 两套 wrapper | 尚未接入外部完成; 需迁移旧头/错误码, 独立 Launch/NDRange, kernel 租用, program 缓存与上下文 generation |
| 自动 cache | 尚未实现 DMA START/END, 普通 Buffer/Image 的传输/map 协议, 同函数 GPU -> CPU 等待和跨 context 桥接 |
| standalone GPU | 尚未实现新资源 registry 与 finish/deinit 排空协议; 不因有 flow 核心就自动安全 |
| GPU 构建 | 当前主机构建不依赖 CL; 可选 GPU 目标, 动态 loader 与 CL1.2 能力路径仍待集成 |
| profiling | 主机 DOT/trace 已有基础; GPU 时间, 完整任务元数据, 时间对齐边界与追踪成本仍需验收 |
| 平台与性能 | Linux 主机已有功能/ABI 检查; Android QCOM/MTK, macOS, Windows 及真实性能/能耗尚未验证 |

## 5 验证入口

```sh
cmake -S . -B /tmp/aura-flow-build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TEST_XFLOW=ON
cmake --build /tmp/aura-flow-build -j 8
ctest --test-dir /tmp/aura-flow-build --output-on-failure
/tmp/aura-flow-build/aura_test --gtest_brief=1
```

CTest 分别验证 flow 单测, mm 租约与分配测试, 严格 `-std=c++11` 消费端, 首 worker 建立失败, C 消费端, 两个算法 `.so` 的实例/句柄隔离. 隔离测试使用 RTLD_GLOBAL, 另检查本地符号表确认 Aura 状态/TLS 没有作为可抢占动态符号导出.

`ENABLE_ASAN`/`ENABLE_TSAN` 现在覆盖 aura 库本体及 flow 测试消费端, 不能同时开启. ASan 的 leak 扫描需要允许检查线程的运行环境; 不应通过关闭 leak 检测来标记通过. 新增的外部模型和后续修改仍须逐次重跑, 以最新日志为准.

当前测试是主机证据, 不是 OpenCL 真机, cache 一致性或性能测量结果. 完整目标保持进行中.

## 6 算法参考

- Taskflow 3.11.0, 固定提交 `816b4ad53b44196c88f409eb7b4a25a0e3bfdf42`, `taskflow/utility/mpmc.hpp` 与 core dependent-async 协议. 没有链接 Taskflow. MIT 通知见 `third_party/flow_queue_references/`.
- [gRPC MPSC](https://github.com/grpc/grpc/blob/master/src/core/util/mpscq.cc) 和 Dmitry Vyukov 的侵入式 MPSC/sequence ring 算法. Aura 自行组织有界尝试, 溢出消费门控和工作许可, 不引入 gRPC/Abseil. 原作者许可一并保留, 后续直接移植应固定对应来源提交.

ReadyQueue 的短暂 pop 失败不证明为空; 一个线程暂停在预留或 exchange/link 之间仍能延迟相应路径的进度. 不宣称整套调度器 wait-free, 不以“无锁”推导图像算子一定更快.
