#include "gpu/pending.h"
#include "log/xerror.h"
#include "log/xlogger.h"
#include "mm/access_context.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace {
constexpr auto kRetryMinimum = std::chrono::microseconds(200);
constexpr auto kRetryMaximum = std::chrono::microseconds(2000);
struct FinalizingFrame { au::gpu::detail::PendingRegistry* mRegistry; FinalizingFrame* mPrevious; };
thread_local FinalizingFrame* gFinalizingFrame = nullptr;
}

namespace au {
namespace gpu AU_LOCAL {
namespace detail {
class Pending;
struct CompletionState {
    // The fence must not retain the provider's leases back to MemoryControl.
    // The registry owns the live record; the fence only has a weak reference.
    std::weak_ptr<au::gpu::detail::Pending> mPending;
    std::atomic<bool> mDone{false};
    std::mutex mMutex;
    std::condition_variable mChanged;
    au::gpu::detail::PendingStatus mStatus;
};
class Pending final {
public:
    au::gpu::detail::PendingRegistry* mRegistry = nullptr;
    au::gpu::detail::PendingOps mOps;
    void* mPayload = nullptr;
    std::shared_ptr<au::gpu::detail::CompletionState> mState;
    // Self ownership is installed only while linked in the intrusive registry.
    // retire moves it out under the registry lock, then releases it outside.
    std::shared_ptr<au::gpu::detail::Pending> mKeepAlive;
    au::gpu::detail::Pending* mPrevious = nullptr;
    au::gpu::detail::Pending* mNext = nullptr;
    std::atomic<bool> mPublished{false};
    std::atomic_flag mQuerying = ATOMIC_FLAG_INIT;
    au::gpu::detail::PendingStatus mStatus; // One querying thread until mDone publication.
    ~Pending() noexcept { if (mPayload) mOps.mDestroy(mPayload); }
    Pending() = default;
    Pending(const au::gpu::detail::Pending&) = delete;
    au::gpu::detail::Pending& operator=(const au::gpu::detail::Pending&) = delete;
    Pending(au::gpu::detail::Pending&&) = delete;
    au::gpu::detail::Pending& operator=(au::gpu::detail::Pending&&) = delete;

    void complete(au::gpu::detail::PendingStatus status) noexcept
    {
        struct Extent {
            FinalizingFrame mFrame;
            au::mm::detail::AccessErrorSink mPreviousSink;
            au::gpu::detail::PendingStatus mError;
            explicit Extent(au::gpu::detail::PendingRegistry* registry) noexcept
                : mFrame{registry, gFinalizingFrame}, mPreviousSink(au::mm::detail::setAccessErrorSink({&Extent::error, this, false}))
            { gFinalizingFrame = &mFrame; }
            ~Extent() noexcept { au::mm::detail::setAccessErrorSink(mPreviousSink); gFinalizingFrame = mFrame.mPrevious; }
            static void error(void* payload, int code, int native, std::uint64_t cause, bool device) noexcept
            {
                auto& self = *static_cast<Extent*>(payload);
                if (!self.mError.mCode) self.mError = {code, native, device ? au::gpu::detail::ErrorDomain::OpenCL : au::gpu::detail::ErrorDomain::Memory, cause};
            }
        } extent(mRegistry);
        const auto cleanup = mOps.mFinalize ? mOps.mFinalize(mPayload, status) : au::gpu::detail::PendingStatus{};
        if (!status.mCode) status = cleanup;
        else if (cleanup.mCode && (cleanup.mCode != status.mCode || cleanup.mNativeCode != status.mNativeCode))
            XLOG_E("au::gpu cleanup failed code=%d native=%d\n", cleanup.mCode, cleanup.mNativeCode);
        void* payload = mPayload; mPayload = nullptr;
        mOps.mDestroy(payload); // Drop native/resource leases before publishing safe completion.
        if (!status.mCode) status = extent.mError;
        mState->mStatus = status;
        {
            std::lock_guard<std::mutex> lock(mState->mMutex);
            mState->mDone.store(true, std::memory_order_release); // Publish status/cleanup before either wake channel.
        }
        mState->mChanged.notify_all();
    }
    int poll(bool* done, au::gpu::detail::PendingStatus* status) noexcept
    {
        *done = mState->mDone.load(std::memory_order_acquire);
        if (*done) { *status = mState->mStatus; return 0; }
        if (!mPublished.load(std::memory_order_acquire) || mQuerying.test_and_set(std::memory_order_acquire))
            return 0; // Another normal thread owns the query/finalization extent.
        if (mState->mDone.load(std::memory_order_acquire)) {
            *done = true; *status = mState->mStatus;
            mQuerying.clear(std::memory_order_release); return 0;
        }
        bool terminal = false;
        au::gpu::detail::PendingStatus observed;
        const int query = mOps.mQuery(mPayload, &terminal, &observed);
        if (query && !observed.mCode) {
            observed.mCode = query; observed.mDomain = au::gpu::detail::ErrorDomain::OpenCL;
        }
        if (!mStatus.mCode && observed.mCode) mStatus = observed;
        *status = query ? observed : mStatus;
        if (terminal) {
            complete(mStatus);
            *status = mState->mStatus; *done = true;
            mRegistry->retire(this);
        }
        mQuerying.clear(std::memory_order_release); // Never hold this gate while waiting for another record.
        return query;
    }
};
} // namespace detail
} // namespace gpu
} // namespace au

namespace {
au::mm::detail::FenceStatus waitFence(void* payload) noexcept
{
    auto& state = *static_cast<std::shared_ptr<au::gpu::detail::CompletionState>*>(payload);
    auto delay = kRetryMinimum;
    while (!state->mDone.load(std::memory_order_acquire)) {
        // Explicit outside callers may query managed fences too. In particular,
        // completion and cleanup must remain possible on the thread calling wait.
        if (auto record = state->mPending.lock()) {
            bool done = false; au::gpu::detail::PendingStatus status;
            (void)record->poll(&done, &status);
            if (done) break;
            if (!au::gpu::detail::pendingCleanupActive()) record->mRegistry->pollSome();
        }
        std::unique_lock<std::mutex> lock(state->mMutex);
        state->mChanged.wait_for(lock, delay, [&] { return state->mDone.load(std::memory_order_acquire); });
        delay = std::min(kRetryMaximum, delay * 2);
    }
    const auto status = state->mStatus;
    return {status.mCode, status.mNativeCode, status.mCauseTaskId,
        status.mDomain == au::gpu::detail::ErrorDomain::OpenCL};
}
void destroyFence(void* payload) noexcept
{ delete static_cast<std::shared_ptr<au::gpu::detail::CompletionState>*>(payload); }
bool readyFence(void* payload) noexcept
{ return (*static_cast<std::shared_ptr<au::gpu::detail::CompletionState>*>(payload))->mDone.load(std::memory_order_acquire); }
}

bool au::gpu::detail::pendingCleanupActive() noexcept { return gFinalizingFrame != nullptr; }

au::gpu::detail::PendingRegistry::~PendingRegistry() noexcept { (void)drain(); }
void au::gpu::detail::PendingRegistry::add(const std::shared_ptr<au::gpu::detail::Pending>& pending) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    pending->mKeepAlive = pending;
    pending->mPrevious = mTail;
    if (mTail) mTail->mNext = pending.get(); else mHead = pending.get();
    mTail = pending.get(); ++mCount;
    ++mRevision; mChanged.notify_all();
}
void au::gpu::detail::PendingRegistry::retire(au::gpu::detail::Pending* pending) noexcept
{
    std::shared_ptr<au::gpu::detail::Pending> retired;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (pending->mPrevious) pending->mPrevious->mNext = pending->mNext; else mHead = pending->mNext;
        if (pending->mNext) pending->mNext->mPrevious = pending->mPrevious; else mTail = pending->mPrevious;
        pending->mPrevious = nullptr; pending->mNext = nullptr;
        --mCount;
        ++mRevision; mChanged.notify_all();
        if (!mFailure.mCode && pending->mState->mStatus.mCode) mFailure = pending->mState->mStatus;
        retired = std::move(pending->mKeepAlive);
    }
}
std::size_t au::gpu::detail::PendingRegistry::size() const noexcept
{ std::lock_guard<std::mutex> lock(mMutex); return mCount; }
bool au::gpu::detail::PendingRegistry::finalizingHere() const noexcept
{
    for (auto* frame = gFinalizingFrame; frame; frame = frame->mPrevious) if (frame->mRegistry == this) return true;
    return false;
}
void au::gpu::detail::PendingRegistry::pollSome(std::size_t limit) noexcept
{
    for (std::size_t index = 0; index < limit; ++index) {
        std::shared_ptr<au::gpu::detail::Pending> pending;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mHead) break;
            pending = mHead->mKeepAlive;
            if (mHead != mTail) {
                auto* first = mHead; mHead = first->mNext; mHead->mPrevious = nullptr;
                first->mPrevious = mTail; first->mNext = nullptr; mTail->mNext = first; mTail = first;
            }
        }
        bool done = false; au::gpu::detail::PendingStatus status;
        (void)pending->poll(&done, &status);
    }
}
au::gpu::detail::PendingStatus au::gpu::detail::PendingRegistry::drain() noexcept
{
    if (finalizingHere()) return {au::err::kErrorBusy, 0, au::gpu::detail::ErrorDomain::System};
    std::unique_lock<std::mutex> lock(mMutex);
    mChanged.wait(lock, [this] { return !mDraining; });
    mDraining = true; // One explicit drain caller owns the retry timer.
    auto delay = kRetryMinimum;
    while (mCount) {
        const auto count = mCount;
        const auto revision = mRevision;
        lock.unlock();
        pollSome(count); // No registry/lifecycle mutex held during driver calls or cleanup.
        lock.lock();
        if (mRevision != revision) delay = kRetryMinimum;
        if (mCount) mChanged.wait_for(lock, delay, [this, revision] { return mRevision != revision; });
        delay = std::min(kRetryMaximum, delay * 2);
    }
    const auto status = mFailure;
    mFailure = {};
    mDraining = false;
    lock.unlock();
    mChanged.notify_all();
    return status;
}
au::gpu::detail::PreparedSubmission::~PreparedSubmission() noexcept
{ if (mPending && !mPublished) cancel({au::err::kErrorAborted, 0, au::gpu::detail::ErrorDomain::OpenCL}); }
int au::gpu::detail::PreparedSubmission::prepare(au::gpu::detail::PendingRegistry& registry,
    const au::gpu::detail::PendingOps& ops, void* payload, std::uintptr_t context, bool hostCompletion) noexcept
{
    if (mPending || !ops.mQuery || !ops.mDestroy || !payload) return au::err::kErrorInvalidParam;
    try {
        auto pending = std::make_shared<au::gpu::detail::Pending>();
        auto state = std::make_shared<au::gpu::detail::CompletionState>();
        auto fence = std::make_shared<au::mm::detail::DeviceFence>();
        std::unique_ptr<std::shared_ptr<au::gpu::detail::CompletionState>> fenceOwner(
            new std::shared_ptr<au::gpu::detail::CompletionState>(state));
        pending->mRegistry = &registry; pending->mOps = ops; pending->mState = state;
        state->mPending = pending;
        fence->mPayload = fenceOwner.release(); fence->mWait = &waitFence; fence->mDestroy = &destroyFence;
        fence->mReady = &readyFence;
        fence->mContext = context; fence->mHostCompletion = hostCompletion;
        pending->mPayload = payload;
        mFence = std::move(fence); mPending = std::move(pending);
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
void au::gpu::detail::PreparedSubmission::publish(const au::gpu::detail::PendingStatus& status) noexcept
{
    if (!mPending || mPublished) return;
    mPublished = true;
    mPending->mStatus = status;
    mPending->mRegistry->add(mPending);
    mPending->mPublished.store(true, std::memory_order_release); // Publish fully committed native/fence state to all pollers.
}
void au::gpu::detail::PreparedSubmission::cancel(const au::gpu::detail::PendingStatus& status) noexcept
{
    if (!mPending || mPublished) return;
    mPublished = true;
    mPending->complete(status); // No native command has been accepted; cleanup is safe synchronously.
}
