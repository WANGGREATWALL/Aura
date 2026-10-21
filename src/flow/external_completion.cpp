#include "flow/core.h"
#include "log/xlogger.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
int Core::prepareExternal(const ExternalOps& ops, void* payload, ExternalToken** out) noexcept
{
    if (!out || !ops.mRelease)
        return au::err::kErrorInvalidParam;
    *out = nullptr;
    if (gTileContext) {
        scopedError(au::err::kErrorStateInvalid);
        return au::err::kErrorStateInvalid;
    }
    if (gCleanupDepth)
        return au::err::kErrorStateInvalid;
    if (!gFrame)
        return 0;
    ExternalToken* token = nullptr;
    try {
        token               = mExternal.create();
        token->mCore        = this;
        token->mParent      = NodeRef(gFrame->mNode);
        token->mOps         = ops;
        token->mPayload     = payload;
        token->mLink.mValue = token;
        bool accepted;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            accepted = mOrder != UINT64_MAX && reserveOutstanding(token->mParent.get());
            if (accepted)
                token->mOrder = ++mOrder;
        }
        if (!accepted) {
            mExternal.destroy(token);  // Drop the parent reference outside the graph lock.
            return au::err::kErrorOverflow;
        }
        *out = token;
        return 0;
    } catch (...) {
        if (token)
            mExternal.destroy(token);
        scopedError(au::err::kErrorNoMemory, au::flow::ErrorDomain::Memory);
        return au::err::kErrorNoMemory;
    }
}

void Core::queueExternal(ExternalToken* token) noexcept
{
    if (token->mQueued.exchange(true, std::memory_order_acq_rel))
        return;  // Queue/pending list owns this record once.
    mCompletions.push(&token->mLink);
    mWork.post();
}

void Core::notifyExternal(ExternalToken* token, const ExternalStatus& status) noexcept
{
    unsigned int pending = 0;
    if (!token->mNotification.compare_exchange_strong(pending, 1, std::memory_order_acq_rel))
        return;  // First notifier owns status.
    token->mStatus = status;
    token->mNotification.store(2, std::memory_order_release);  // Worker can now read immutable terminal status.
    queueExternal(token);
}

bool Core::consumeExternal(ExternalToken* token) noexcept
{
    if (token->mRegistrar.load(std::memory_order_acquire))
        return false;  // Registration may still touch payload/event.
    Core::CleanupExtent extent(this, token->mParent.get());
    if (token->mNotification.load(std::memory_order_acquire) != 2 && token->mOps.mPoll) {
        bool           done = false;
        ExternalStatus status;
        const int      query = token->mOps.mPoll(token->mPayload, &done, &status);
        if (query) {
            const bool first = token->mParent->mErrors.scoped(
                token->mParent->mId, query,
                status.mDomain == au::flow::ErrorDomain::None ? au::flow::ErrorDomain::Flow : status.mDomain,
                status.mNativeCode, status.mCauseTaskId);
            if (first)
                XLOG_E("au::flow external query failed task=%llu code=%d native=%d\n",
                       static_cast<unsigned long long>(token->mParent->mId), query, status.mNativeCode);
        }
        if (done)
            notifyExternal(token, status);
    }
    if (token->mNotification.load(std::memory_order_acquire) != 2)
        return false;
    ExternalStatus       status  = token->mStatus;
    const ExternalStatus cleanup = token->mOps.mFinalize ? token->mOps.mFinalize(token->mPayload) : ExternalStatus{};
    if (status.mCode)
        XLOG_E("au::flow external completion failed task=%llu code=%d native=%d\n",
               static_cast<unsigned long long>(token->mParent->mId), status.mCode, status.mNativeCode);
    if (cleanup.mCode)
        XLOG_E("au::flow external cleanup failed task=%llu code=%d native=%d\n",
               static_cast<unsigned long long>(token->mParent->mId), cleanup.mCode, cleanup.mNativeCode);
    if (!status.mCode && cleanup.mCode)
        status = cleanup;
    token->mOps.mRelease(token->mPayload);  // Never run release or driver calls under the pending-list lock.
    NodeRef parent = token->mParent;
    if (status.mCode) {
        au_flow_result failure = taskResult(parent->mId, status.mCode, status.mDomain);
        failure.mNativeCode    = status.mNativeCode;
        if (status.mCauseTaskId)
            failure.mCauseTaskId = status.mCauseTaskId;
        inherit(parent.get(), failure, token->mOrder);
    }
    mExternal.destroy(token);
    if (parent->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
        finish(parent.get());  // Release event reservation last.
    return true;
}

void Core::deferExternal(ExternalToken* token, bool announce) noexcept
{
    std::lock_guard<std::mutex> lock(mExternalMutex);
    token->mPendingNext = nullptr;
    if (mPendingExternalTail)
        mPendingExternalTail->mPendingNext = token;
    else
        mPendingExternal = token;
    mPendingExternalTail = token;
    const auto previous  = mPendingExternalCount.fetch_add(1, std::memory_order_release);
    if (announce && !previous)
        mWork.control();  // First query claimant must wake an indefinitely sleeping worker.
}

bool Core::pollPendingExternal() noexcept
{
    ExternalToken* token;
    {
        std::lock_guard<std::mutex> lock(mExternalMutex);
        token = mPendingExternal;
        if (!token)
            return false;
        mPendingExternal = token->mPendingNext;
        if (!mPendingExternal)
            mPendingExternalTail = nullptr;
        mPendingExternalCount.fetch_sub(1, std::memory_order_release);
    }
    if (!consumeExternal(token)) {
        deferExternal(token, false);
        return false;
    }
    return true;
}

void Core::releasePoller() noexcept
{
    int owner = gWorker;
    // Most CPU dispatches have no lease. Avoid compare-exchange traffic
    // on a shared cache line, and take the worker lock only for a handoff.
    if (owner < 0 || mPollOwner.load(std::memory_order_acquire) != owner)
        return;
    bool released;
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        released = mPollOwner.compare_exchange_strong(owner, -1, std::memory_order_acq_rel);
    }
    if (released)
        mWork.control();  // Retirement and relinquishing the last query consumer are serialized.
}

bool Core::pumpExternal(std::chrono::steady_clock::time_point& deadline) noexcept
{
    const int id = gWorker;
    if (!mPendingExternalCount.load(std::memory_order_acquire)) {
        releasePoller();
        return false;
    }
    if (id < 0 || static_cast<std::size_t>(id) >= mBudget.load(std::memory_order_acquire))
        return false;
    int   unclaimed = -1;
    auto& worker    = mWorkers[static_cast<std::size_t>(id)];
    if (mPollOwner.load(std::memory_order_acquire) == -1 &&
        mPollOwner.compare_exchange_strong(unclaimed, id, std::memory_order_acq_rel)) {
        worker.mPollDue       = std::chrono::steady_clock::now();
        worker.mPollDelay     = kPollMinimum;
        worker.mPollRemaining = 0;
    }
    if (mPollOwner.load(std::memory_order_acquire) != id)
        return false;
    bool progress = false;
    if (std::chrono::steady_clock::now() >= worker.mPollDue && !mPolling.test_and_set(std::memory_order_acquire)) {
        if (!worker.mPollRemaining) {
            worker.mPollRemaining = mPendingExternalCount.load(std::memory_order_acquire);
            worker.mPollProgress  = false;
        }
#ifdef AURA_FLOW_TESTING
        mPollPasses.fetch_add(1, std::memory_order_relaxed);
        runtimePoint(au::flow::testing::RuntimePoint::BeforePendingQuery, static_cast<std::size_t>(id));
#endif
        const auto attempts = std::min<std::size_t>(kPollBatch, worker.mPollRemaining);
        for (std::size_t i = 0; i < attempts; ++i)
            progress = pollPendingExternal() || progress;
        worker.mPollRemaining -= attempts;
        worker.mPollProgress = worker.mPollProgress || progress;
        if (!worker.mPollRemaining) {
            worker.mPollDelay = worker.mPollProgress ? kPollMinimum : std::min(kPollMaximum, worker.mPollDelay * 2);
            worker.mPollDue   = std::chrono::steady_clock::now() + worker.mPollDelay;
        } else
            worker.mPollDue = std::chrono::steady_clock::now();  // Finish a round fairly, eight queries per dispatch.
        mPolling.clear(std::memory_order_release);
    }
    if (!mPendingExternalCount.load(std::memory_order_acquire))
        releasePoller();
    else
        deadline = std::min(deadline, worker.mPollDue);
    return progress;
}
int prepareExternal(const ExternalOps& ops, void* payload, ExternalToken** out) noexcept
{
    return core().prepareExternal(ops, payload, out);
}
std::uint64_t currentTaskId() noexcept { return gFrame ? gFrame->mNode->mId : 0; }
void          initializeExecutor() noexcept { (void)core(); }
bool          cleanupActive() noexcept { return gCleanupDepth != 0; }
bool          assistExternalWait(bool (*ready)(void*) noexcept, void* payload, std::uint64_t retryMicroseconds) noexcept
{
    if (!gFrame || gWorker < 0 || gCleanupDepth || gDepth >= kMaxDispatchDepth)
        return false;
    const auto deadline = retryMicroseconds == UINT64_MAX
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + std::chrono::microseconds(retryMicroseconds);
    (void)core().helpOne(true, deadline, nullptr, nullptr, ready, payload);
    return true;
}
void notifyExternalWaiters() noexcept { core().notifyExternalWaiters(); }
void leaveExternalWait() noexcept { core().leaveCooperativeWait(); }

void cleanupExternal(ExternalToken* token, void (*fn)(void*) noexcept, void* payload) noexcept
{
    auto*               owner = token ? token->mCore : &core();
    NodeRef             pin(token ? token->mParent.get() : nullptr);
    Core::CleanupExtent extent(owner, pin.get());
    if (fn)
        fn(payload);
}
void recordExternalError(const ExternalStatus& status) noexcept
{
    core().scopedError(status.mCode, status.mDomain, status.mNativeCode, status.mCauseTaskId);
}
void notifyExternal(ExternalToken* token, const ExternalStatus& status) noexcept
{
    if (token)
        token->mCore->notifyExternal(token, status);
}
void pollExternal(ExternalToken* token) noexcept
{
    if (token)
        token->mCore->queueExternal(token);
}
void leaveExternalRegistrar(ExternalToken* token) noexcept
{
    if (token)
        token->mRegistrar.store(false, std::memory_order_release); /* Last registrar access. */
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
