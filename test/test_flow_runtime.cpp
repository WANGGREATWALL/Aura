#include "flow/xflow.h"
#include "flow/ready_queue.h"
#include "flow/testing.h"
#include "flow/completion.h"
#include "sys/flow_runtime.h"
#include "gtest/gtest.h"
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {
template<class F> bool await(F&& ready, unsigned int milliseconds = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Test observation only.
    }
    return true;
}
class RuntimeBlock {
public:
    explicit RuntimeBlock(au::flow::testing::RuntimePoint point, std::uint64_t task = 0) noexcept
        : mPoint(point), mTask(task), mHook{&RuntimeBlock::invoke, this}
    { au::flow::testing::runtimeHook(&mHook); }
    ~RuntimeBlock()
    {
        mRelease.store(true, std::memory_order_release);
        au::flow::testing::runtimeHook(nullptr); // Wait for readers before destroying this payload.
    }
    std::atomic<bool> mEntered{false}, mRelease{false}, mTimedOut{false};
    std::atomic<std::size_t> mSlot{8};
private:
    static void invoke(void* payload, au::flow::testing::RuntimePoint point, std::size_t slot) noexcept
    {
        auto& self = *static_cast<RuntimeBlock*>(payload);
        if (point != self.mPoint || (self.mTask && au::flow::detail::currentTaskId() != self.mTask)) return;
        bool first = false;
        if (!self.mEntered.compare_exchange_strong(first, true, std::memory_order_acq_rel)) return;
        self.mSlot.store(slot, std::memory_order_release);
        if (!await([&] { return self.mRelease.load(std::memory_order_acquire); }))
            self.mTimedOut.store(true, std::memory_order_release);
    }
    au::flow::testing::RuntimePoint mPoint;
    std::uint64_t mTask;
    au::flow::testing::RuntimeHook mHook;
};
class XFlowRuntime : public ::testing::Test {
protected:
    void SetUp() override
    {
        au::flow::testing::runtimeHook(nullptr);
        ASSERT_EQ(au::flow::XFlow::get().waitAll(), 0);
        mBudget = au::flow::testing::budget();
    }
    void TearDown() override
    {
        au::flow::testing::runtimeHook(nullptr);
        EXPECT_EQ(au::flow::XFlow::get().waitAll(), 0);
        EXPECT_EQ(au::flow::testing::setBudget(mBudget), 0);
    }
    void fresh(std::size_t budget)
    {
        ASSERT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
        ASSERT_EQ(au::flow::testing::setBudget(budget), 0);
    }
    std::size_t mBudget = 1;
};
}

TEST_F(XFlowRuntime, ControlEpochDoesNotConsumeWorkOrLosePreSleepNotifications)
{
    au::sys::flow::WorkSemaphore gate;
    for (unsigned int i = 0; i < 100; ++i) {
        const auto epoch = gate.epoch();
        gate.post();
        gate.control(); // Both happened before the waiter entered the CV.
        EXPECT_EQ(gate.acquireUntil(epoch, std::chrono::steady_clock::now()),
                  au::sys::flow::WorkSemaphore::Wake::Control);
        EXPECT_TRUE(gate.tryAcquire());
        EXPECT_FALSE(gate.tryAcquire());
    }
    std::atomic<bool> parked{false};
    au::sys::flow::WorkSemaphore::Wake wake = au::sys::flow::WorkSemaphore::Wake::Timeout;
    const auto epoch = gate.epoch();
    std::thread surplus([&] {
        parked.store(true, std::memory_order_release);
        wake = gate.acquireUntil(epoch, std::chrono::steady_clock::now() + std::chrono::seconds(5), false);
    });
    EXPECT_TRUE(await([&] { return parked.load(std::memory_order_acquire); }));
    gate.post();
    EXPECT_EQ(gate.acquireUntil(epoch, std::chrono::steady_clock::now() + std::chrono::seconds(1)),
              au::sys::flow::WorkSemaphore::Wake::Work);
    gate.control();
    surplus.join();
    EXPECT_EQ(wake, au::sys::flow::WorkSemaphore::Wake::Control);
    EXPECT_FALSE(gate.tryAcquire());
}

TEST_F(XFlowRuntime, QueuePublicationBeforeSleepAndIdleHaveNoPeriodicWake)
{
    fresh(1);
    auto& flow = au::flow::XFlow::get();
    {
        RuntimeBlock paused(au::flow::testing::RuntimePoint::BeforeSleep);
        auto first = flow.enqueue([]() -> int { return 0; });
        EXPECT_TRUE(await([&] { return paused.mEntered.load(std::memory_order_acquire); }));
        auto next = flow.enqueue([]() -> int { return 0; });
        paused.mRelease.store(true, std::memory_order_release);
        EXPECT_EQ(first.waitFor(5000), 0);
        EXPECT_EQ(next.waitFor(5000), 0);
        EXPECT_FALSE(paused.mTimedOut.load(std::memory_order_acquire));
    }
    const auto before = au::flow::testing::runtimeStats();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto after = au::flow::testing::runtimeStats();
    EXPECT_EQ(after.mWaitTimeouts, before.mWaitTimeouts);
    EXPECT_EQ(after.mPollPasses, before.mPollPasses);
    EXPECT_EQ(after.mPollOwner, -1);
}

TEST_F(XFlowRuntime, CompletionBeforeCooperativeSleepIsNotLost)
{
    fresh(2);
    auto& flow = au::flow::XFlow::get();
    std::atomic<bool> go{false}, targetEntered{false}, targetRelease{false};
    au::flow::Task target;
    auto parent = flow.makeTask([&]() -> int {
        if (!await([&] { return go.load(std::memory_order_acquire); })) return 99;
        return target.wait();
    });
    EXPECT_EQ(flow.enqueue(parent).submissionCode(), 0);
    target = flow.enqueue([&]() -> int {
        targetEntered.store(true, std::memory_order_release);
        return await([&] { return targetRelease.load(std::memory_order_acquire); }) ? 0 : 99;
    });
    EXPECT_TRUE(await([&] { return targetEntered.load(std::memory_order_acquire); }));
    RuntimeBlock paused(au::flow::testing::RuntimePoint::BeforeSleep, parent.id());
    go.store(true, std::memory_order_release);
    EXPECT_TRUE(await([&] { return paused.mEntered.load(std::memory_order_acquire); }));
    targetRelease.store(true, std::memory_order_release);
    EXPECT_EQ(target.waitFor(5000), 0); // Completion/control publication precedes actual gate sleep.
    paused.mRelease.store(true, std::memory_order_release);
    EXPECT_EQ(parent.waitFor(5000), 0);
    EXPECT_FALSE(paused.mTimedOut.load(std::memory_order_acquire));
}

TEST_F(XFlowRuntime, CooperativeCompletionIsObservedAcrossAQueuePublicationHole)
{
    fresh(2);
    auto& flow = au::flow::XFlow::get();
    struct Pause {
        std::atomic<bool> mEnabled{false}, mReserved{false}, mRelease{false}, mProbed{false};
    } paused;
    au::flow::detail::QueueHooks queue{};
    queue.mPayload = &paused;
    queue.mRingReserved = [](void* payload) noexcept {
        auto& p = *static_cast<Pause*>(payload);
        if (!p.mEnabled.load(std::memory_order_acquire)) return;
        bool first = false;
        if (!p.mReserved.compare_exchange_strong(first, true, std::memory_order_acq_rel)) return;
        (void)await([&] { return p.mRelease.load(std::memory_order_acquire); }, 10000);
    };
    ASSERT_EQ(au::flow::testing::queueHooks(queue), 0);
    std::atomic<bool> targetEntered{false}, releaseTarget{false}, parentEntered{false}, releaseParent{false};
    auto target = flow.enqueue([&]() -> int {
        targetEntered.store(true, std::memory_order_release);
        return await([&] { return releaseTarget.load(std::memory_order_acquire); }, 10000) ? 0 : 99;
    });
    auto parent = flow.enqueue([&]() -> int {
        parentEntered.store(true, std::memory_order_release);
        if (!await([&] { return releaseParent.load(std::memory_order_acquire); }, 10000)) return 99;
        return target.wait();
    });
    EXPECT_TRUE(await([&] { return targetEntered.load(std::memory_order_acquire) && parentEntered.load(std::memory_order_acquire); }));
    au::flow::testing::RuntimeHook observe{[](void* payload, au::flow::testing::RuntimePoint point, std::size_t) noexcept {
        if (point == au::flow::testing::RuntimePoint::QueueProbeMiss)
            static_cast<Pause*>(payload)->mProbed.store(true, std::memory_order_release);
    }, &paused};
    au::flow::testing::runtimeHook(&observe);
    paused.mEnabled.store(true, std::memory_order_release);
    int first = 99;
    std::thread producer([&] { first = flow.enqueue([]() -> int { return 0; }).waitFor(5000); });
    EXPECT_TRUE(await([&] { return paused.mReserved.load(std::memory_order_acquire); }));
    auto second = flow.enqueue([]() -> int { return 0; }); // A real permit exists behind the unpublished ring slot.
    releaseParent.store(true, std::memory_order_release);
    EXPECT_TRUE(await([&] { return paused.mProbed.load(std::memory_order_acquire); }));
    releaseTarget.store(true, std::memory_order_release);
    EXPECT_EQ(parent.waitFor(1000), 0); // Must return while the queue publisher remains paused.
    paused.mRelease.store(true, std::memory_order_release);
    producer.join();
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second.waitFor(5000), 0);
    EXPECT_EQ(target.waitFor(5000), 0);
    au::flow::testing::runtimeHook(nullptr);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
    EXPECT_EQ(au::flow::testing::queueHooks({}), 0);
}

TEST_F(XFlowRuntime, PureExternalJobDoesNotKeepUnusedWorkersAlive)
{
    fresh(1);
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.enqueue([]() -> int { return 0; }).wait(), 0);
    std::atomic<bool> entered{false}, release{false};
    int code = 99;
    std::thread caller([&] {
        code = flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) -> int {
            entered.store(true, std::memory_order_release);
            return await([&] { return release.load(std::memory_order_acquire); }) ? 0 : 99;
        });
    });
    EXPECT_TRUE(await([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
    EXPECT_EQ(flow.waitAll(), 0);
    release.store(true, std::memory_order_release);
    caller.join();
    EXPECT_EQ(code, 0);
}

TEST_F(XFlowRuntime, QueryPinDelaysRetirementAndNewGenerationRestarts)
{
    fresh(1);
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.enqueue([]() -> int { return 0; }).wait(), 0);
    const auto before = au::flow::testing::runtimeStats();
    au::flow::WorkerInfo info[8]{};
    std::size_t count = 0;
    ASSERT_EQ(flow.getWorkers(info, 8, count), 0);
    ASSERT_EQ(count, 1u);
    int queried = 99;
    {
        RuntimeBlock pinned(au::flow::testing::RuntimePoint::QueryPinned);
        std::thread query([&] { std::size_t cpus = 0; queried = flow.getWorkerAffinity(info[0].mWorkerId, nullptr, 0, cpus); });
        EXPECT_TRUE(await([&] { return pinned.mEntered.load(std::memory_order_acquire); }));
        EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mRetireBlocked != 0; }));
        const auto during = au::flow::testing::runtimeStats();
        EXPECT_EQ(during.mLive, 1u);
        EXPECT_EQ(during.mGenerations, before.mGenerations);
        pinned.mRelease.store(true, std::memory_order_release);
        query.join();
        EXPECT_FALSE(pinned.mTimedOut.load(std::memory_order_acquire));
    }
    EXPECT_TRUE(queried == au::err::kErrorBufferUnderflow || queried == au::err::kErrorNotSupported);
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
    EXPECT_EQ(flow.enqueue([]() -> int { return 0; }).waitFor(5000), 0);
    const auto after = au::flow::testing::runtimeStats();
    EXPECT_GT(after.mGenerations[0], before.mGenerations[0]);
}

TEST_F(XFlowRuntime, AdmissionKeepsAConsumerUntilAcceptedWorkIsPublished)
{
    fresh(1);
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.enqueue([]() -> int { return 0; }).wait(), 0);
    const auto before = au::flow::testing::runtimeStats();
    RuntimeBlock admission(au::flow::testing::RuntimePoint::AfterAdmission);
    int code = 99;
    std::thread submit([&] { code = flow.enqueue([]() -> int { return 0; }).waitFor(5000); });
    EXPECT_TRUE(await([&] { return admission.mEntered.load(std::memory_order_acquire); }));
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mRetireBlocked != 0; }));
    EXPECT_EQ(au::flow::testing::runtimeStats().mLive, 1u);
    EXPECT_EQ(flow.waitAll(), 0); // Admission is pinned, but this root has not committed yet.
    admission.mRelease.store(true, std::memory_order_release);
    submit.join();
    EXPECT_EQ(code, 0);
    EXPECT_EQ(au::flow::testing::runtimeStats().mGenerations, before.mGenerations);
    EXPECT_FALSE(admission.mTimedOut.load(std::memory_order_acquire));
}

TEST_F(XFlowRuntime, SubmissionWhileLastSlotRetiresDoesNotHoldGraphLockOrLoseWork)
{
    fresh(1);
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.enqueue([]() -> int { return 0; }).wait(), 0);
    const auto before = au::flow::testing::runtimeStats();
    RuntimeBlock retiring(au::flow::testing::RuntimePoint::AfterRetire);
    EXPECT_TRUE(await([&] { return retiring.mEntered.load(std::memory_order_acquire); }));
    std::atomic<bool> submitting{false};
    int code = 99;
    std::thread submit([&] {
        submitting.store(true, std::memory_order_release);
        code = flow.enqueue([]() -> int { return 0; }).waitFor(5000);
    });
    EXPECT_TRUE(await([&] { return submitting.load(std::memory_order_acquire); }));
    EXPECT_EQ(flow.waitAll(), 0); // The stalled startup/join cannot hold the graph mutex.
    retiring.mRelease.store(true, std::memory_order_release);
    submit.join();
    EXPECT_EQ(code, 0);
    EXPECT_GT(au::flow::testing::runtimeStats().mGenerations[0], before.mGenerations[0]);
    EXPECT_FALSE(retiring.mTimedOut.load(std::memory_order_acquire));
}

TEST_F(XFlowRuntime, SinglePollerSurvivesBudgetParkingCpuDispatchAndRetirement)
{
    fresh(4);
    auto& flow = au::flow::XFlow::get();
    std::atomic<unsigned int> started{0};
    std::atomic<bool> releaseWorkers{false};
    std::array<au::flow::Task, 4> warm;
    for (auto& task : warm) task = flow.enqueue([&]() -> int {
        started.fetch_add(1, std::memory_order_relaxed);
        return await([&] { return releaseWorkers.load(std::memory_order_acquire); }) ? 0 : 99;
    });
    EXPECT_TRUE(await([&] { return started.load(std::memory_order_relaxed) == 4; }));
    releaseWorkers.store(true, std::memory_order_release);
    for (const auto& task : warm) EXPECT_EQ(task.waitFor(5000), 0);
    struct Event { std::atomic<bool> mDone{false}; std::atomic<unsigned int> mQueries{0}; };
    std::array<Event, 24> events;
    std::atomic<unsigned int> released{0};
    struct Payload { Event* mEvent; std::atomic<unsigned int>* mReleased; };
    std::array<Payload, 24> payloads;
    au::flow::detail::ExternalOps ops{};
    ops.mPoll = [](void* value, bool* done, au::flow::detail::ExternalStatus*) noexcept -> int {
        auto& payload = *static_cast<Payload*>(value);
        payload.mEvent->mQueries.fetch_add(1, std::memory_order_relaxed);
        *done = payload.mEvent->mDone.load(std::memory_order_acquire);
        return 0;
    };
    ops.mRelease = [](void* value) noexcept { static_cast<Payload*>(value)->mReleased->fetch_add(1, std::memory_order_relaxed); };
    auto root = flow.enqueue([&]() -> int {
        for (std::size_t i = 0; i < events.size(); ++i) {
            payloads[i] = {&events[i], &released};
            au::flow::detail::ExternalToken* token = nullptr;
            const int code = au::flow::detail::prepareExternal(ops, &payloads[i], &token);
            if (code) return code;
            au::flow::detail::pollExternal(token);
            au::flow::detail::leaveExternalRegistrar(token);
        }
        return 0;
    });
    EXPECT_TRUE(await([&] {
        for (const auto& event : events) if (!event.mQueries.load(std::memory_order_relaxed)) return false;
        return au::flow::testing::runtimeStats().mPollOwner >= 0;
    }));
    const auto handoff = [&](bool cooperative) {
        if (au::sys::flow::affinitySupported()) return;
        std::vector<std::uint32_t> cpus;
        EXPECT_EQ(au::sys::flow::allowedCpus(cpus), 0);
        if (cpus.size() < 2) return;
        const au::flow::AffinityPolicy one{au::flow::AffinityMode::ExplicitCpuSet, cpus.data(), 1};
        EXPECT_EQ(flow.setAffinity(one), 0);
        EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mPollOwner == 0; }, 1000));
        std::atomic<bool> cpuEntered{false}, cpuRelease{false};
        auto cpu = flow.enqueue([&]() -> int {
            if (cooperative && root.waitFor(10) != au::err::kErrorTimeout) return 99;
            cpuEntered.store(true, std::memory_order_release);
            return await([&] { return cpuRelease.load(std::memory_order_acquire); }) ? 0 : 99;
        });
        EXPECT_TRUE(await([&] { return cpuEntered.load(std::memory_order_acquire); }));
        const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
        EXPECT_EQ(flow.setAffinity(restore), 0);
        EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mPollOwner > 0; }, 1000));
        cpuRelease.store(true, std::memory_order_release);
        EXPECT_EQ(cpu.waitFor(5000), 0);
        (void)cpu.wait(); // Keep borrowed flags alive even on a failed bounded assertion.
    };
    handoff(true); // Existing sleepers must wake promptly; no two-second deadline can mask a lost control wake.
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 1; }));
    const auto start = events[0].mQueries.load(std::memory_order_relaxed);
    std::atomic<unsigned int> querySlots{0};
    au::flow::testing::RuntimeHook observe{[](void* value, au::flow::testing::RuntimePoint point, std::size_t slot) noexcept {
        if (point == au::flow::testing::RuntimePoint::BeforePendingQuery)
            static_cast<std::atomic<unsigned int>*>(value)->fetch_or(1u << slot, std::memory_order_relaxed);
    }, &querySlots};
    au::flow::testing::runtimeHook(&observe);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    au::flow::testing::runtimeHook(nullptr);
    const auto mask = querySlots.load(std::memory_order_relaxed);
    EXPECT_NE(mask, 0u);
    EXPECT_EQ(mask & (mask - 1), 0u); // Only one worker performs periodic queries during this idle window.
    EXPECT_GT(events[0].mQueries.load(std::memory_order_relaxed), start);
    handoff(false); // Also cover handoff after followers retired and a new generation must start.
    for (auto& event : events) event.mDone.store(true, std::memory_order_release);
    EXPECT_EQ(root.waitFor(5000), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), events.size());
    EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
}

TEST_F(XFlowRuntime, ConcurrentSubmissionsPolicyQueriesAndFiveRetirementGenerations)
{
    fresh(4);
    auto& flow = au::flow::XFlow::get();
    std::vector<std::uint32_t> cpus;
    ASSERT_EQ(au::sys::flow::allowedCpus(cpus), 0);
    ASSERT_FALSE(cpus.empty());
    std::atomic<unsigned int> failures{0};
    std::atomic<std::size_t> total{0};
    const auto firstGeneration = au::flow::testing::runtimeStats().mGenerations[0];
    for (unsigned int epoch = 0; epoch < 5; ++epoch) {
        std::atomic<bool> stop{false};
        std::thread policy([&] {
            if (au::sys::flow::affinitySupported()) return;
            while (!stop.load(std::memory_order_acquire)) {
                const au::flow::AffinityPolicy one{au::flow::AffinityMode::ExplicitCpuSet, cpus.data(), 1};
                const au::flow::AffinityPolicy all{au::flow::AffinityMode::SystemDefault, nullptr, 0};
                if (flow.setAffinity(one) || flow.setAffinity(all)) failures.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        std::thread query([&] {
            while (!stop.load(std::memory_order_acquire)) {
                std::array<au::flow::WorkerInfo, 8> info{};
                std::size_t count = 0;
                if (flow.getWorkers(info.data(), info.size(), count)) failures.fetch_add(1, std::memory_order_relaxed);
                for (std::size_t i = 0; i < count; ++i) {
                    std::size_t size = 0;
                    const int code = flow.getWorkerAffinity(info[i].mWorkerId, nullptr, 0, size);
                    if (code != au::err::kErrorBufferUnderflow && code != au::err::kErrorNotReady &&
                        code != au::err::kErrorNotSupported) failures.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::array<std::thread, 4> producers;
        for (auto& producer : producers) producer = std::thread([&] {
            for (unsigned int i = 0; i < 100; ++i) {
                auto root = flow.enqueue([&]() -> int {
                    auto child = flow.enqueue([&]() -> int { total.fetch_add(1, std::memory_order_relaxed); return 0; });
                    const int loop = flow.parallelizeTiledTasks(64, 1, [&](std::size_t, std::size_t count) {
                        total.fetch_add(count, std::memory_order_relaxed);
                    });
                    const int joined = child.wait();
                    return loop ? loop : joined;
                });
                if (root.waitFor(5000)) failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (auto& producer : producers) producer.join();
        stop.store(true, std::memory_order_release);
        policy.join();
        query.join();
        const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
        EXPECT_EQ(flow.setAffinity(restore), 0);
        EXPECT_EQ(flow.waitAll(), 0);
        EXPECT_TRUE(await([] { return au::flow::testing::runtimeStats().mLive == 0; }));
    }
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 4u * 100u * 5u * 65u);
    EXPECT_GE(au::flow::testing::runtimeStats().mGenerations[0], firstGeneration + 5);
}
