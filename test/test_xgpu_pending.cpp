#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "gpu/pending.h"
#include "gtest/gtest.h"
#include "mm/access.h"
#include "mm/xaccess.h"
#include "mm/xmemory.h"
namespace {
struct PendingModel
{
    std::atomic<bool> mReady{false};
    std::atomic<int>  mQueries{0}, mFinalized{0}, mDestroyed{0};
    std::atomic<bool> mQueryFailure{false};
    int               mCleanup    = 0;
    bool              mHostOnly   = false;
    std::thread::id   mWaiter     = std::this_thread::get_id();
    void (*mHook)(void*) noexcept = nullptr;
    void* mHookData               = nullptr;
};
struct PendingPayload
{
    PendingModel*                mModel;
    au::mm::detail::DeviceAccess mAccess;
    explicit PendingPayload(PendingModel& model) : mModel(&model) {}
    static int query(void* payload, bool* done, au::gpu::detail::PendingStatus* status) noexcept
    {
        auto& model = *static_cast<PendingPayload*>(payload)->mModel;
        model.mQueries.fetch_add(1, std::memory_order_relaxed);  // Test diagnostics only.
        if (model.mHostOnly && std::this_thread::get_id() != model.mWaiter)
            return 0;
        if (model.mQueryFailure.exchange(false, std::memory_order_acq_rel)) {  // One simulated native query failure.
            *status = {au::err::kErrorPlatformAPI, -77, au::gpu::detail::ErrorDomain::OpenCL};
            return status->mCode;
        }
        *done = model.mReady.load(std::memory_order_acquire);  // Simulated device completion publication.
        return 0;
    }
    static au::gpu::detail::PendingStatus finalize(void* payload, const au::gpu::detail::PendingStatus& status) noexcept
    {
        auto& self = *static_cast<PendingPayload*>(payload);
        self.mModel->mFinalized.fetch_add(1, std::memory_order_relaxed);
        if (self.mModel->mHook)
            self.mModel->mHook(self.mModel->mHookData);
        const int code = self.mAccess.releaseCompleted({status.mCode, status.mNativeCode, 0, true});
        return {code ? code : self.mModel->mCleanup, 0, au::gpu::detail::ErrorDomain::Memory};
    }
    static void destroy(void* payload) noexcept
    {
        auto* self = static_cast<PendingPayload*>(payload);
        self->mModel->mDestroyed.fetch_add(1, std::memory_order_relaxed);
        delete self;
    }
};
const au::gpu::detail::PendingOps gPendingOps{&PendingPayload::query, &PendingPayload::finalize,
                                              &PendingPayload::destroy};
}  // namespace

TEST(XGpu, QueryCompletionPinsAllocationWithoutScheduler)
{
    PendingModel                     model;
    au::gpu::detail::PendingRegistry registry;
    au::mm::MemBlock                 memory;
    ASSERT_EQ(au::mm::alloc(256, au::mm::MemType::Pss, memory), 0);
    const auto                          access = au::mm::readWriteAccess(memory.ptr, memory.size);
    std::unique_ptr<PendingPayload>     payload(new PendingPayload(model));
    au::gpu::detail::PreparedSubmission submission;
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 17), 0);
    auto* raw = payload.release();
    ASSERT_EQ(raw->mAccess.begin(&access, 1, 17), 0);
    ASSERT_EQ(raw->mAccess.commit(submission.fence()), 0);
    const auto fence = submission.fence();
    submission.publish();
    registry.pollSome();
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 0);
    std::atomic<int> error{0};
    std::thread      waiter([&] { error.store(fence->wait().mCode, std::memory_order_relaxed); });
    model.mReady.store(true, std::memory_order_release);
    waiter.join();
    EXPECT_EQ(error.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(registry.drain().mCode, 0);
    EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(fence->wait().mCode, 0);
}

TEST(XGpu, StandaloneQueryFailureAndImmediateCompletion)
{
    for (bool queryFailure : {false, true}) {
        PendingModel model;
        model.mQueryFailure.store(queryFailure, std::memory_order_relaxed);
        au::gpu::detail::PendingRegistry    registry;
        std::unique_ptr<PendingPayload>     payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
        (void)payload.release();
        const auto fence = submission.fence();
        submission.publish();
        registry.pollSome(1);
        EXPECT_EQ(registry.size(), 1u);
        EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 0);  // Unknown native state is still in flight.
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
    au::gpu::detail::PendingRegistry    registry;
    au::gpu::detail::PreparedSubmission submission;
    std::unique_ptr<PendingPayload>     payload(new PendingPayload(immediate));
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
    (void)payload.release();
    submission.publish();
    registry.pollSome();
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_EQ(immediate.mDestroyed.load(std::memory_order_relaxed), 1);
}

TEST(XGpu, CancelAndPartialSubmissionPreserveErrors)
{
    for (bool partial : {false, true}) {
        PendingModel                        model;
        au::gpu::detail::PendingRegistry    registry;
        std::unique_ptr<PendingPayload>     payload(new PendingPayload(model));
        au::gpu::detail::PreparedSubmission submission;
        ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
        (void)payload.release();
        const auto fence = submission.fence();
        if (partial) {
            submission.publish({au::err::kErrorPlatformAPI, -5, au::gpu::detail::ErrorDomain::OpenCL});
            registry.pollSome();
            EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 0);
            model.mReady.store(true, std::memory_order_release);
        } else
            submission.cancel({au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::OpenCL});
        const auto status = fence->wait();
        EXPECT_EQ(status.mCode, partial ? au::err::kErrorPlatformAPI : au::err::kErrorInvalidParam);
        EXPECT_EQ(status.mNativeCode, partial ? -5 : 0);
        EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
        (void)registry.drain();
    }
}

TEST(XGpu, CleanupRejectsRegistryReentry)
{
    PendingModel                     model;
    au::gpu::detail::PendingRegistry registry;
    struct Hook
    {
        au::gpu::detail::PendingRegistry* mRegistry;
        int                               mCode = 0;
        static void                       run(void* p) noexcept
        {
            auto& self = *static_cast<Hook*>(p);
            EXPECT_TRUE(au::gpu::detail::pendingCleanupActive());
            self.mCode = self.mRegistry->drain().mCode;
        }
    } hook{&registry};
    model.mHook     = &Hook::run;
    model.mHookData = &hook;
    std::unique_ptr<PendingPayload>     payload(new PendingPayload(model));
    au::gpu::detail::PreparedSubmission submission;
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 9), 0);
    (void)payload.release();
    submission.publish();
    model.mReady.store(true, std::memory_order_release);
    EXPECT_EQ(registry.drain().mCode, 0);
    EXPECT_EQ(hook.mCode, au::err::kErrorBusy);
    EXPECT_FALSE(au::gpu::detail::pendingCleanupActive());
}

TEST(XGpu, ConcurrentStandaloneFenceWaitersAndDrainersCompleteOnce)
{
    PendingModel                        model;
    au::gpu::detail::PendingRegistry    registry;
    au::gpu::detail::PreparedSubmission submission;
    std::unique_ptr<PendingPayload>     payload(new PendingPayload(model));
    ASSERT_EQ(submission.prepare(registry, gPendingOps, payload.get(), 17), 0);
    (void)payload.release();
    const auto fence = submission.fence();
    submission.publish();
    std::array<std::thread, 5> callers;
    std::array<int, 5>         codes{{99, 99, 99, 99, 99}};
    for (std::size_t i = 0; i < callers.size(); ++i)
        callers[i] = std::thread([&, i] { codes[i] = i < 3 ? fence->wait().mCode : registry.drain().mCode; });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!model.mQueries.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    model.mReady.store(true, std::memory_order_release);
    for (auto& caller : callers)
        caller.join();
    for (int code : codes)
        EXPECT_EQ(code, 0);
    EXPECT_EQ(model.mFinalized.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mDestroyed.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(registry.size(), 0u);
}
