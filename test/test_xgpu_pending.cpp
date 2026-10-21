#include "gpu/pending.h"
#include "gpu/xopencl_api.h"
#include "flow/xflow.h"
#include "flow/testing.h"
#include "sys/flow_runtime.h"
#include <vector>
#include "mm/access.h"
#include "mm/xaccess.h"
#include "mm/xmemory.h"
#include "gtest/gtest.h"
#include <functional>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {
struct PendingModel {
    std::atomic<bool> mReady{false};
    std::atomic<int> mQueries{0}, mFinalized{0}, mDestroyed{0};
    std::atomic<bool> mQueryFailure{false};
    int mCleanup = 0;
    bool mHostOnly = false;
    std::thread::id mWaiter = std::this_thread::get_id();
    void (*mHook)(void*) noexcept = nullptr;
    void* mHookData = nullptr;
};
struct PendingPayload {
    PendingModel* mModel;
    au::mm::detail::DeviceAccess mAccess;
    explicit PendingPayload(PendingModel& model) : mModel(&model) {}
    static int query(void* payload, bool* done, au::flow::detail::ExternalStatus* status) noexcept
    {
        auto& model = *static_cast<PendingPayload*>(payload)->mModel;
        model.mQueries.fetch_add(1, std::memory_order_relaxed); // Test diagnostics only.
        if (model.mHostOnly && std::this_thread::get_id() != model.mWaiter) return 0;
        if (model.mQueryFailure.exchange(false, std::memory_order_acq_rel)) { // One simulated native query failure.
            *status = {au::err::kErrorPlatformAPI, -77, au::flow::ErrorDomain::OpenCL};
            return status->mCode;
        }
        *done = model.mReady.load(std::memory_order_acquire); // Simulated device completion publication.
        return 0;
    }
    static au::flow::detail::ExternalStatus finalize(void* payload, const au::flow::detail::ExternalStatus& status) noexcept
    {
        auto& self = *static_cast<PendingPayload*>(payload);
        self.mModel->mFinalized.fetch_add(1, std::memory_order_relaxed);
        if (self.mModel->mHook) self.mModel->mHook(self.mModel->mHookData);
        const int code = self.mAccess.releaseCompleted({status.mCode, status.mNativeCode, 0, true});
        return {code ? code : self.mModel->mCleanup, 0, au::flow::ErrorDomain::Memory};
    }
    static void destroy(void* payload) noexcept
    {
        auto* self = static_cast<PendingPayload*>(payload);
        self->mModel->mDestroyed.fetch_add(1, std::memory_order_relaxed);
        delete self;
    }
};
const au::gpu::detail::PendingOps gPendingOps{&PendingPayload::query, &PendingPayload::finalize, &PendingPayload::destroy};
}

TEST(XGpu, QueryCompletionPinsTaskAndAllocation)
{
    PendingModel model;
    au::gpu::detail::PendingRegistry registry;
    au::mm::MemBlock memory;
    ASSERT_EQ(au::mm::alloc(256, au::mm::MemType::Pss, memory), 0);
    const auto access = au::mm::readWriteAccess(memory.ptr, memory.size);
    std::shared_ptr<au::mm::detail::DeviceFence> fence;
    std::atomic<bool> published{false};
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        int code = submission.prepare(registry, gPendingOps, payload.get(), 17);
        if (code) return code;
        auto* raw = payload.release();
        code = raw->mAccess.begin(&access, 1, 17);
        if (!code) code = raw->mAccess.commit(submission.fence());
        if (code) { submission.cancel({code, 0, au::flow::ErrorDomain::Memory}); return code; }
        fence = submission.fence();
        submission.publish();
        published.store(true, std::memory_order_release); // Publishes the fence to test waiters.
        return 0;
    });
    while (!published.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_EQ(au::flow::XFlow::get().waitFor(task, 2), au::err::kErrorTimeout);
    EXPECT_EQ(au::mm::free(memory.ptr), 0); // Retires lookup; provider still owns the allocation.
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 0);
    std::atomic<int> waitErrors{0};
    std::thread waiter([&] { if (fence->wait().mCode) waitErrors.fetch_add(1, std::memory_order_relaxed); });
    std::thread wrapperEntry([&] {
        while (!model.mReady.load(std::memory_order_acquire)) registry.pollSome();
        registry.pollSome();
    });
    model.mReady.store(true, std::memory_order_release);
    waiter.join(); wrapperEntry.join();
    EXPECT_EQ(au::flow::XFlow::get().wait(task), 0);
    EXPECT_EQ(waitErrors.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(registry.drain().mCode, 0);
    EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(fence->wait().mCode, 0); // Completed fence does not retain provider or backing allocation.
}

TEST(XGpu, StandaloneQueryFailureAndImmediateCompletion)
{
    for (bool queryFailure : {false, true}) {
        PendingModel model;
        model.mQueryFailure.store(queryFailure, std::memory_order_relaxed);
        au::gpu::detail::PendingRegistry registry;
        std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
        (void)payload.release();
        const auto fence = submission.fence();
        submission.publish();
        registry.pollSome(1);
        EXPECT_EQ(registry.size(), 1u);
        EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 0); // Unknown native state is still in flight.
        model.mReady.store(true, std::memory_order_release);
        const auto status = fence->wait();
        EXPECT_EQ(status.mCode, queryFailure ? au::err::kErrorPlatformAPI : 0);
        EXPECT_EQ(status.mNativeCode, queryFailure ? -77 : 0);
        EXPECT_EQ(registry.drain().mCode, status.mCode);
        EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
    }
    PendingModel immediate;
    immediate.mReady.store(true, std::memory_order_relaxed);
    au::gpu::detail::PendingRegistry registry;
    au::gpu::detail::PreparedSubmission submission;
    std::unique_ptr<PendingPayload> payload(new PendingPayload(immediate));
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
    (void)payload.release(); submission.publish(); registry.pollSome();
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_EQ(immediate.mDestroyed.load(std::memory_order_relaxed), 1);
}

TEST(XGpu, CancelCleanupAndPartialSubmissionError)
{
    au::gpu::detail::PendingRegistry registry;
    PendingModel cancelled;
    {
        au::gpu::detail::PreparedSubmission submission;
        std::unique_ptr<PendingPayload> payload(new PendingPayload(cancelled));
        ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 3), 0);
        (void)payload.release();
        submission.cancel({au::err::kErrorInvalidParam, 0, au::flow::ErrorDomain::OpenCL});
        EXPECT_EQ(submission.fence()->wait().mCode, au::err::kErrorInvalidParam);
    }
    EXPECT_EQ(cancelled.mQueries.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(cancelled.mDestroyed.load(std::memory_order_relaxed), 1);
    PendingModel partial;
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        au::gpu::detail::PreparedSubmission submission;
        std::unique_ptr<PendingPayload> payload(new PendingPayload(partial));
        const int code = submission.prepare(registry, gPendingOps, payload.get(), 3);
        if (code) return code;
        (void)payload.release();
        submission.publish({au::err::kErrorPlatformAPI, -5, au::flow::ErrorDomain::OpenCL});
        return 0; // Forgotten int error cannot hide accepted native work's error.
    });
    EXPECT_EQ(au::flow::XFlow::get().waitFor(task, 2), au::err::kErrorTimeout);
    partial.mReady.store(true, std::memory_order_release);
    au::flow::TaskResult result{};
    EXPECT_EQ(au::flow::XFlow::get().wait(task, &result), au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(result.mNativeCode, -5);
    EXPECT_EQ(partial.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(registry.drain().mNativeCode, -5);
}

TEST(XGpu, HostFenceCleanupRejectsReentryInOriginalTaskScope)
{
    for (bool managed : {true, false}) {
        PendingModel model; model.mHostOnly = true;
        au::gpu::detail::PendingRegistry registry;
        au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
        au::flow::Task task;
        struct Hook {
            au::flow::Task* mTask;
            void* mPointer;
            std::uint64_t mTaskId = 0;
            int mWait = 0, mSubmit = 0, mCpu = 0, mFinish = 0, mAll = 0, mShutdown = 0, mTiled = 0;
            bool mTileInvoked = false;
            static void invoke(void* payload) noexcept
            {
                auto& self = *static_cast<Hook*>(payload);
                self.mTaskId = au::flow::detail::currentTaskId();
                self.mWait = au::flow::XFlow::get().wait(*self.mTask);
                auto rejected = au::flow::XFlow::get().enqueue([]() -> int { return 0; });
                self.mSubmit = rejected.submissionCode();
                au::mm::CpuAccessScope cpu;
                self.mCpu = cpu.begin({au::mm::readAccess(self.mPointer, 64)});
                self.mFinish = ::au_gpu_finish(AU_GPU_GENERAL);
                self.mAll = au::flow::XFlow::get().waitAll();
                self.mShutdown = au::flow::XFlow::get().shutdown();
                self.mTiled = au::flow::XFlow::get().parallelizeTiledTasks(4, 1,
                    [&](std::size_t, std::size_t) { self.mTileInvoked = true; });
            }
        } hook{&task, memory.ptr};
        model.mHook = &Hook::invoke; model.mHookData = &hook;
        const auto access = au::mm::writeAccess(memory.ptr, memory.size);
        std::shared_ptr<au::mm::detail::DeviceFence> fence;
        std::atomic<bool> published{false};
        auto launch = [&]() -> int {
            std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
            au::gpu::detail::PreparedSubmission submission;
            int code = submission.prepare(registry, gPendingOps, payload.get(), 17);
            if (code) return code;
            auto* raw = payload.release();
            code = raw->mAccess.begin(&access, 1, 17);
            if (!code) code = raw->mAccess.commit(submission.fence());
            if (code) { submission.cancel({code, 0, au::flow::ErrorDomain::Memory}); return code; }
            fence = submission.fence(); submission.publish();
            published.store(true, std::memory_order_release); return 0;
        };
        if (managed) task = au::flow::XFlow::get().enqueue(launch);
        else {
            ASSERT_EQ(launch(), 0);
            task = au::flow::XFlow::get().enqueue([&]() -> int {
                au::mm::CpuAccessScope cpu;
                return cpu.begin({au::mm::readAccess(memory.ptr, memory.size)});
            });
        }
        while (!published.load(std::memory_order_acquire)) std::this_thread::yield();
        model.mReady.store(true, std::memory_order_release);
        EXPECT_EQ(fence->wait().mCode, au::err::kErrorBusy); // Main performs completion while the resource is still leased.
        EXPECT_EQ(hook.mTaskId, managed ? task.id() : 0);
        EXPECT_EQ(hook.mWait, au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(hook.mSubmit, au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(hook.mCpu, au::err::kErrorBusy);
        EXPECT_EQ(hook.mFinish, au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(hook.mAll, au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(hook.mShutdown, au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(hook.mTiled, au::flow::kErrorWouldDeadlock);
        EXPECT_FALSE(hook.mTileInvoked);
        au::flow::TaskResult result{};
        ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), managed ? au::flow::kErrorWouldDeadlock : au::err::kErrorBusy);
        EXPECT_EQ(result.mCode, managed ? au::flow::kErrorWouldDeadlock : au::err::kErrorBusy);
        EXPECT_EQ(result.mDomain, managed ? au::flow::ErrorDomain::Flow : au::flow::ErrorDomain::Memory);
        EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(au::mm::free(memory.ptr), 0);
    }
}

TEST(XGpu, WorkerResourceWaitCooperatesAtSingleWorkerBudget)
{
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    struct Restore {
        std::size_t mBudget;
        ~Restore() { (void)au::flow::testing::setBudget(mBudget); }
    } restore{budget};
    for (bool managed : {false, true}) {
    PendingModel model;
    au::gpu::detail::PendingRegistry registry;
    au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
    const auto access = au::mm::writeAccess(memory.ptr, memory.size);
    std::atomic<bool> published{false};
    const auto launch = [&]() -> int {
        std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        int code = submission.prepare(registry, gPendingOps, payload.get(), 17);
        if (code) return code;
        auto* raw = payload.release();
        code = raw->mAccess.begin(&access, 1, 17);
        if (!code) code = raw->mAccess.commit(submission.fence());
        if (code) { submission.cancel({code, 0, au::flow::ErrorDomain::Memory}); return code; }
        submission.publish(); published.store(true, std::memory_order_release);
        return 0;
    };
    if (!managed) ASSERT_EQ(launch(), 0);
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        if (managed) { const int code = launch(); if (code) return code; }
        au::mm::CpuAccessScope cpu;
        return cpu.begin({au::mm::readAccess(memory.ptr, memory.size)});
    });
    while (!published.load(std::memory_order_acquire)) std::this_thread::yield();
    auto ready = au::flow::XFlow::get().enqueue([&]() -> int {
        model.mReady.store(true, std::memory_order_release); return 0;
    });
    const int completed = au::flow::XFlow::get().waitFor(task, 1000);
    EXPECT_EQ(completed, 0); // The only worker must execute ready while its CPU guard waits on the modeled event.
    model.mReady.store(true, std::memory_order_release); // Recovery also drains resources if the assertion detects a regression.
    au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), 0); EXPECT_EQ(result.mCode, 0);
    ASSERT_EQ(au::flow::XFlow::get().wait(ready, &result), 0); EXPECT_EQ(result.mCode, 0);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
    }
}

TEST(XGpu, ConcurrentStandaloneFenceWaitersAndDrainersCompleteOnce)
{
    PendingModel model;
    au::gpu::detail::PendingRegistry registry;
    au::gpu::detail::PreparedSubmission submission;
    std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 17), 0);
    (void)payload.release();
    const auto fence = submission.fence();
    submission.publish();
    std::array<std::thread, 5> callers;
    std::array<int, 5> codes{{99, 99, 99, 99, 99}};
    for (std::size_t i = 0; i < callers.size(); ++i) callers[i] = std::thread([&, i] {
        codes[i] = i < 3 ? fence->wait().mCode : registry.drain().mCode;
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!model.mQueries.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    model.mReady.store(true, std::memory_order_release);
    for (auto& caller : callers) caller.join();
    for (int code : codes) EXPECT_EQ(code, 0);
    EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(XGpu, DeepManagedFenceWaitStillMakesProgressAtDispatchDepthLimit)
{
    const auto budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    PendingModel model;
    model.mReady.store(true, std::memory_order_release);
    au::gpu::detail::PendingRegistry registry;
    au::mm::MemBlock memory;
    ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
    const auto access = au::mm::writeAccess(memory.ptr, memory.size);
    std::function<int(unsigned int)> descend;
    descend = [&](unsigned int depth) -> int {
        if (depth < 31) return au::flow::XFlow::get().enqueue([&, depth]() -> int { return descend(depth + 1); }).wait();
        std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        int code = submission.prepare(registry, gPendingOps, payload.get(), 17);
        if (code) return code;
        auto* raw = payload.release();
        code = raw->mAccess.begin(&access, 1, 17);
        if (!code) code = raw->mAccess.commit(submission.fence());
        if (code) { submission.cancel({code, 0, au::flow::ErrorDomain::Memory}); return code; }
        submission.publish();
        au::mm::CpuAccessScope cpu;
        return cpu.begin({au::mm::readAccess(memory.ptr, memory.size)});
    };
    auto root = au::flow::XFlow::get().enqueue([&]() -> int { return descend(0); });
    EXPECT_EQ(root.waitFor(5000), 0);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XGpu, ReturningFromManagedFenceWaitHandsOffRemainingEvents)
{
    auto& flow = au::flow::XFlow::get();
    std::vector<std::uint32_t> cpus;
    ASSERT_EQ(au::sys::flow::allowedCpus(cpus), 0);
    if (cpus.size() < 2) GTEST_SKIP() << "Needs capacity for a resumed callable and a query worker";
    const auto budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    PendingModel first, second;
    au::gpu::detail::PendingRegistry registry;
    au::mm::MemBlock a, b;
    ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, a), 0);
    ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, b), 0);
    std::atomic<bool> published{false}, resumed{false}, release{false};
    const auto launch = [&](PendingModel& model, const au::mm::MemBlock& memory) -> int {
        std::unique_ptr<PendingPayload> payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        int code = submission.prepare(registry, gPendingOps, payload.get(), 17);
        if (code) return code;
        auto* raw = payload.release();
        const auto access = au::mm::writeAccess(memory.ptr, memory.size);
        code = raw->mAccess.begin(&access, 1, 17);
        if (!code) code = raw->mAccess.commit(submission.fence());
        if (code) { submission.cancel({code, 0, au::flow::ErrorDomain::Memory}); return code; }
        submission.publish();
        return 0;
    };
    auto root = flow.enqueue([&]() -> int {
        int code = launch(first, a);
        if (!code) code = launch(second, b);
        if (code) return code;
        published.store(true, std::memory_order_release);
        au::mm::CpuAccessScope cpu;
        code = cpu.begin({au::mm::readAccess(a.ptr, a.size)});
        if (!code) code = cpu.finish();
        if (code) return code;
        resumed.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return release.load(std::memory_order_acquire) ? 0 : 99;
    });
    const auto querying = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!published.load(std::memory_order_acquire) || first.mQueries.load(std::memory_order_relaxed) < 3 ||
            second.mQueries.load(std::memory_order_relaxed) < 2 || au::flow::testing::runtimeStats().mPollOwner != 0) &&
           std::chrono::steady_clock::now() < querying) std::this_thread::yield();
    EXPECT_TRUE(published.load(std::memory_order_acquire));
    EXPECT_EQ(au::flow::testing::runtimeStats().mPollOwner, 0);
    first.mReady.store(true, std::memory_order_release);
    const auto returned = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!resumed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < returned)
        std::this_thread::yield();
    EXPECT_TRUE(resumed.load(std::memory_order_acquire));
    const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
    EXPECT_EQ(flow.setAffinity(restore), 0);
    second.mReady.store(true, std::memory_order_release);
    const auto completed = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!second.mFinalized.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < completed)
        std::this_thread::yield();
    EXPECT_EQ(second.mFinalized.load(std::memory_order_relaxed), 1);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(root.waitFor(5000), 0);
    (void)root.wait();
    EXPECT_EQ(au::mm::free(a.ptr), 0);
    EXPECT_EQ(au::mm::free(b.ptr), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}
