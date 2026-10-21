#if ENABLE_TEST_XFLOW
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "flow/xflow.h"
#include "gtest/gtest.h"
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif
namespace {
bool until(const std::function<bool()>& fn)
{
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fn()) {
        if (std::chrono::steady_clock::now() >= limit)
            return false;
        std::this_thread::yield();
    }
    return true;
}
struct Member
{
    int add(int& value, int amount) const
    {
        value += amount;
        return 0;
    }
};
}  // namespace
class XFlow : public ::testing::Test
{
protected:
    void TearDown() override { (void)au::flow::Flow::get().waitAll(); }
};
TEST_F(XFlow, LazyThreadSpawning)
{
    auto& flow = au::flow::Flow::get();
    ASSERT_EQ(flow.workerCount(), 0u);
    std::atomic<bool> entered{false}, release{false};
    auto              first = flow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return until([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_EQ(flow.workerCount(), 1u);
    std::vector<au::flow::Task> tasks;
    for (int i = 0; i < 64; ++i)
        tasks.push_back(flow.enqueue([] {}));
    EXPECT_LE(flow.workerCount(), 8u);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(first.wait(), 0);
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(flow.waitAll(), 0);
}
TEST_F(XFlow, AsyncEnqueueAndWait)
{
    auto& flow  = au::flow::Flow::get();
    int   value = 0;
    EXPECT_EQ(flow.enqueue([](int& v) { v = 4; }, std::ref(value)).wait(), 0);
    Member member;
    EXPECT_EQ(flow.enqueue(&Member::add, &member, std::ref(value), 3).wait(), 0);
    EXPECT_EQ(value, 7);
    std::unique_ptr<int> owned(new int(19));
    EXPECT_EQ(flow.enqueue([](std::unique_ptr<int> p) -> int { return *p == 19 ? 0 : 1; }, std::move(owned)).wait(), 0);
    EXPECT_FALSE(owned);
    EXPECT_EQ(flow.enqueue([]() -> int { return 71; }).wait(), 71);
    EXPECT_EQ(flow.enqueue([]() -> int { throw std::runtime_error("unexpected"); }).wait(), XFLOW_ERROR_EXCEPTION);
    au::flow::Task invalid;
    EXPECT_EQ(invalid.wait(), XFLOW_ERROR_INVALID_ARGUMENT);
    auto draft = flow.create([] {});
    EXPECT_EQ(draft.wait(), XFLOW_ERROR_NOT_READY);
    EXPECT_EQ(draft.result(), XFLOW_ERROR_NOT_READY);
    auto copy = draft;
    draft     = std::move(draft);
    copy      = copy;
    EXPECT_EQ(flow.enqueue(draft), 0);
    EXPECT_EQ(flow.enqueue(copy), 0);
    EXPECT_EQ(copy.wait(), 0);
    EXPECT_EQ(flow.waitAll(), 71);
    EXPECT_EQ(flow.waitAll(), 0);
    std::weak_ptr<int> released;
    {
        auto owner               = std::make_shared<int>(7);
        released                 = owner;
        auto            detached = flow.create([owner] {});
        au::flow::Tasks group;
        EXPECT_EQ(group.add(detached), 0);
        EXPECT_EQ(flow.enqueue(group), 0);
    }
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_TRUE(released.expired());
}
TEST_F(XFlow, DependencyChainAndDiamondDAG)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> stage{0};
    auto             a = flow.create([&] { stage.store(1, std::memory_order_relaxed); });
    auto             b = flow.create([&] {
        EXPECT_GE(stage.load(std::memory_order_relaxed), 1);
        stage.fetch_add(1, std::memory_order_relaxed);
    });
    auto             c = flow.create([&] {
        EXPECT_GE(stage.load(std::memory_order_relaxed), 1);
        stage.fetch_add(1, std::memory_order_relaxed);
    });
    auto             d = flow.create([&] { EXPECT_EQ(stage.load(std::memory_order_relaxed), 3); });
    EXPECT_EQ((a >> b >> d).code(), 0);
    EXPECT_EQ((a >> c >> d).code(), 0);
    EXPECT_EQ(a.precede(b), 0);  // Duplicate edges are idempotent.
    au::flow::Tasks graph;
    ASSERT_EQ(graph.add(a, b, c, d), 0);
    ASSERT_EQ(flow.enqueue(graph), 0);
    ASSERT_EQ(flow.wait(graph), 0);
    EXPECT_EQ(flow.enqueue(graph), 0);
    EXPECT_EQ(stage.load(std::memory_order_relaxed), 3);
}
TEST_F(XFlow, DynamicEnqueueAfter)
{
    auto& flow = au::flow::Flow::get();
    for (int round = 0; round < 40; ++round) {
        std::atomic<int>              total{0};
        auto                          pred = flow.enqueue([&] { total.fetch_add(1, std::memory_order_relaxed); });
        std::array<au::flow::Task, 4> after;
        std::array<std::thread, 4>    threads;
        for (std::size_t i = 0; i < threads.size(); ++i)
            threads[i] = std::thread([&, i] {
                after[i] = flow.enqueueAfter(pred, [&] {
                    EXPECT_GE(total.load(std::memory_order_relaxed), 1);
                    total.fetch_add(1, std::memory_order_relaxed);
                });
            });
        for (auto& t : threads)
            t.join();
        auto tail = flow.enqueueAfter({after[0], after[1], after[2], after[3]},
                                      [&] { EXPECT_EQ(total.load(std::memory_order_relaxed), 5); });
        EXPECT_EQ(tail.wait(), 0);
        EXPECT_EQ(flow.enqueueAfter(pred, [] {}).wait(), 0);  // Completed predecessor.
    }
    auto running = flow.enqueue([] {});
    ASSERT_EQ(running.wait(), 0);
    EXPECT_EQ((running >> running).code(), XFLOW_ERROR_CYCLE_DETECTED);
    EXPECT_EQ(running.result(), 0);
    EXPECT_EQ(flow.enqueueAfter(au::flow::Task(), [] {}).wait(), XFLOW_ERROR_INVALID_ARGUMENT);
}
TEST_F(XFlow, TasksClusterAndDotDump)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> count{0};
    auto             a = flow.createNamed("a\"\\\n", [&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             b = flow.createNamed("b", [&] {
        EXPECT_EQ(count.load(std::memory_order_relaxed), 1);
        count.fetch_add(1, std::memory_order_relaxed);
    });
    auto             c = flow.createNamed("c", [&] {
        EXPECT_EQ(count.load(std::memory_order_relaxed), 2);
        count.fetch_add(1, std::memory_order_relaxed);
    });
    auto             d = flow.createNamed("d", [&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 3); });
    au::flow::Tasks  first("first"), second("second");
    EXPECT_EQ(first.add(b), 0);
    EXPECT_EQ(second.add(c), 0);
    EXPECT_EQ((a >> first >> second >> d).code(), 0);
    EXPECT_EQ(flow.enqueue(a), 0);
    EXPECT_EQ(flow.enqueue(first), 0);
    EXPECT_EQ(flow.enqueue(second), 0);
    EXPECT_EQ(flow.enqueue(d), 0);
    auto tail = flow.enqueueAfter(second, [] {});
    EXPECT_EQ(tail.wait(), 0);
    EXPECT_EQ(d.wait(), 0);
    const char* dump = second.dumpDot();
    ASSERT_NE(dump, nullptr);
    const std::string dot(dump);
    EXPECT_NE(dot.find("digraph"), std::string::npos);
    EXPECT_NE(dot.find("first"), std::string::npos);
    EXPECT_NE(dot.find("state=done"), std::string::npos);
    EXPECT_NE(dot.find("cost_us="), std::string::npos);
    EXPECT_NE(dot.find(" -> "), std::string::npos);
    EXPECT_GE(a.costUs(), 0);
    au::flow::Tasks empty;
    EXPECT_EQ(flow.enqueue(empty), 0);
    EXPECT_EQ(empty.wait(), 0);
    auto copy = second;
    second    = au::flow::Tasks();
    EXPECT_EQ(copy.wait(), 0);
    auto            accepted = flow.enqueue([] {});
    auto            pending  = flow.create([] {});
    au::flow::Tasks mixed;
    EXPECT_EQ(mixed.add(accepted, pending), 0);
    EXPECT_EQ(flow.enqueue(mixed), 0);
    EXPECT_EQ(mixed.wait(), 0);
    auto incoming = flow.create([] {});
    EXPECT_EQ(incoming.precede(mixed), XFLOW_ERROR_ALREADY_ENQUEUED);
    EXPECT_EQ(accepted.wait(), 0);
    au::flow::Tasks rejected;
    EXPECT_EQ(rejected.add(au::flow::Task()), XFLOW_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(flow.enqueue(rejected), XFLOW_ERROR_INVALID_ARGUMENT);
}
TEST_F(XFlow, ErrorShortCircuiting)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> ran{0};
    auto             a = flow.create([]() -> int { return -123; });
    auto             b = flow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    auto             c = flow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ((a >> b >> c).code(), 0);
    au::flow::Tasks group;
    ASSERT_EQ(group.add(a, b, c), 0);
    ASSERT_EQ(flow.enqueue(group), 0);
    EXPECT_EQ(c.wait(), -123);
    EXPECT_EQ(group.wait(), -123);
    EXPECT_EQ(flow.waitAll(), -123);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(flow.enqueueAfter(a, [&] { ++ran; }).wait(), -123);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
}
TEST_F(XFlow, CycleDetectionAndDiscardedDraftLifetime)
{
    auto&              flow = au::flow::Flow::get();
    std::weak_ptr<int> observed;
    {
        auto owner = std::make_shared<int>(1);
        observed   = owner;
        auto a = flow.create([owner] {}), b = flow.create([] {});
        EXPECT_EQ((a >> b >> a).code(), XFLOW_ERROR_CYCLE_DETECTED);
        au::flow::Tasks group;
        (void)group.add(a, b);
        EXPECT_EQ(flow.enqueue(group), XFLOW_ERROR_CYCLE_DETECTED);
    }
    EXPECT_TRUE(observed.expired());
    auto self = flow.create([] {});
    EXPECT_EQ(self.precede(self), XFLOW_ERROR_CYCLE_DETECTED);
    EXPECT_EQ(flow.enqueue(self), XFLOW_ERROR_CYCLE_DETECTED);
    auto target = flow.enqueue([] {});
    EXPECT_EQ(target.wait(), 0);
    EXPECT_EQ(self.precede(target), XFLOW_ERROR_ALREADY_ENQUEUED);
    EXPECT_EQ(target.result(), 0);
    auto pred = flow.create([] {}), successor = flow.create([] {});
    EXPECT_EQ(pred.precede(successor), 0);
    EXPECT_EQ(flow.enqueue(successor), XFLOW_ERROR_INVALID_ARGUMENT);  // No permanently unsubmitted predecessor.
}
TEST_F(XFlow, CooperativeWaitAndWaitCycles)
{
    auto& flow  = au::flow::Flow::get();
    auto  outer = flow.enqueue([&] {
        auto child = flow.enqueue([]() -> int { return 23; });
        return child.wait();
    });
    EXPECT_EQ(outer.wait(), 23);
    EXPECT_EQ(flow.enqueue([&] { return flow.waitAll(); }).wait(), XFLOW_ERROR_WOULD_DEADLOCK);
    std::atomic<bool> go{false};
    au::flow::Task    a, b;
    a = flow.create([&] { return until([&] { return go.load(std::memory_order_acquire); }) ? b.wait() : 1; });
    b = flow.create([&] { return until([&] { return go.load(std::memory_order_acquire); }) ? a.wait() : 1; });
    EXPECT_EQ(flow.enqueue(a), 0);
    EXPECT_EQ(flow.enqueue(b), 0);
    go.store(true, std::memory_order_release);
    EXPECT_EQ(a.wait(), XFLOW_ERROR_WOULD_DEADLOCK);
    EXPECT_EQ(b.wait(), XFLOW_ERROR_WOULD_DEADLOCK);
}
TEST_F(XFlow, TiledParallelismAndNestedSafety)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> active{0}, maximum{0};
    for (size_t range : {size_t(0), size_t(3), size_t(257)}) {
        std::array<std::atomic<int>, 257> seen{};
        flow.parallelizeTiledTasks(range, 7, [&](size_t begin, size_t end) {
            const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
            int       old = maximum.load(std::memory_order_relaxed);
            while (old < now && !maximum.compare_exchange_weak(old, now, std::memory_order_relaxed)) {
            }
            for (size_t i = begin; i < end; ++i)
                seen[i].fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
            active.fetch_sub(1, std::memory_order_release);
        });
        for (size_t i = 0; i < seen.size(); ++i)
            EXPECT_EQ(seen[i].load(std::memory_order_relaxed), i < range ? 1 : 0);
    }
    EXPECT_LE(maximum.load(std::memory_order_relaxed), 4);
    std::array<int, 51> values{};
    XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(values.size(), 6) values[i] = static_cast<int>(i);
    XTHREAD_PARALLELIZE_END
    int* data = values.data();
    XTHREAD_PARALLELIZE_TILED_TASK_ASSIGN(values.size(), 8) data[i] += 1;
    XTHREAD_PARALLELIZE_END
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(values[i], static_cast<int>(i) + 1);
    std::atomic<int>              arrived{0};
    std::atomic<size_t>           total{0};
    std::array<au::flow::Task, 8> tasks;
    for (auto& task : tasks)
        task = flow.enqueue([&]() -> int {
            arrived.fetch_add(1, std::memory_order_release);
            if (!until([&] { return arrived.load(std::memory_order_acquire) == 8; }))
                return 1;
            flow.parallelizeTiledTasks(127, 11, [&](size_t b, size_t e) {
                flow.parallelizeTiledTasks(16, 3, [&](size_t ib, size_t ie) {
                    total.fetch_add((e - b) * (ie - ib), std::memory_order_relaxed);
                });
            });
            return 0;
        });
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 8u * 127 * 16);
    EXPECT_LE(flow.workerCount(), 8u);
    size_t end = 0;
    flow.parallelizeTiledTasks(SIZE_MAX, SIZE_MAX, [&](size_t b, size_t e) {
        EXPECT_EQ(b, 0u);
        end = e;
    });
    EXPECT_EQ(end, SIZE_MAX);
}
TEST_F(XFlow, CpuAffinity)
{
    auto& flow = au::flow::Flow::get();
    ASSERT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kDefault), 0);
    const uint64_t allowed = flow.getAffinityMask();
    if (allowed) {
        const auto selected = allowed & (~allowed + 1);
        ASSERT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kCustomMask, selected), 0);
        EXPECT_EQ(flow.getAffinityMask(), selected);
#if defined(__linux__) || defined(__ANDROID__)
        std::atomic<uint64_t> actual{0};
        auto                  worker = flow.enqueue([&] {
            cpu_set_t set;
            uint64_t  mask = 0;
            if (!sched_getaffinity(0, sizeof(set), &set))
                for (unsigned i = 0; i < 64; ++i)
                    if (CPU_ISSET(i, &set))
                        mask |= UINT64_C(1) << i;
            actual.store(mask, std::memory_order_release);
        });
        EXPECT_TRUE(until([&] { return actual.load(std::memory_order_acquire) != 0; }));
        EXPECT_EQ(worker.wait(), 0);
        EXPECT_EQ(actual.load(std::memory_order_acquire), selected);
#endif
#if defined(__linux__) || defined(__ANDROID__)
        cpu_set_t before, after;
        ASSERT_EQ(sched_getaffinity(0, sizeof(before), &before), 0);
#endif
        EXPECT_EQ(flow.enqueue([] {}).wait(), 0);
#if defined(__linux__) || defined(__ANDROID__)
        ASSERT_EQ(sched_getaffinity(0, sizeof(after), &after), 0);
        EXPECT_TRUE(CPU_EQUAL(&before, &after));  // Helping callers must not inherit worker policy.
#endif
        EXPECT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kPerformance), 0);
        EXPECT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kPowerSaving), 0);
    }
    EXPECT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kCustomMask, 0), XFLOW_ERROR_AFFINITY_FAILED);
    EXPECT_EQ(flow.setAffinity(au::flow::AffinityPolicy::kDefault), 0);
}
#endif
