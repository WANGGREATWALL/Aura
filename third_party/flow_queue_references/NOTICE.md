# Queue algorithm references

Aura's ready queue uses sequence slots and an intrusive exchange/link overflow.
It is an Aura-specific implementation, with bounded fast-path attempts, an
overflow consumer gate and separate counting work permits. No third-party
scheduler runtime is linked.

- Taskflow: Copyright (c) 2018-2025 Dr. Tsung-Wei Huang, The University of
  Wisconsin at Madison. Source baseline: 816b4ad53b44196c88f409eb7b4a25a0e3bfdf42,
  taskflow/utility/mpmc.hpp. MIT notice: TASKFLOW-MIT.txt.
- gRPC: Copyright 2016 gRPC authors. Algorithm reference:
  https://github.com/grpc/grpc/blob/master/src/core/util/mpscq.cc
  (reviewed 2026-10-20). Apache-2.0 notice: APACHE-2.0.txt. The gRPC code credits
  Dmitry Vyukov's intrusive MPSC queue:
  https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
- Sequence ring background:
  https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

Reference algorithms do not provide a proof of the combined Aura queue.
Publication gaps, transient pop failures, lifetime and wake permits require
separate validation. Future direct source imports must record the exact commit
and keep all corresponding copyright and license notices.
