#include "flow/core.h"
#include "flow/graph_search.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
int Core::JobScope::accept() noexcept
{
    std::lock_guard<std::mutex> lock(mCore->mMutex);
    if (mCore->mPhase == Phase::Closed || (mCore->mPhase == Phase::Stopping && !gFrame))
        return au::err::kErrorStateInvalid;
    if (mCore->mJobs == SIZE_MAX)
        return au::err::kErrorOverflow;
    ++mCore->mJobs;
    mAccepted = true;
    if (mCore->mPhase == Phase::Dormant)
        mCore->mPhase = Phase::Running;
    return 0;
}

Core::JobScope::~JobScope() noexcept
{
    if (!mAccepted)
        return;
    {
        std::lock_guard<std::mutex> lock(mCore->mMutex);
        --mCore->mJobs;
    }
    mCore->mDrained.notify_all();
}

std::size_t Core::acceptHelpers(NodeRef* helpers, std::size_t count) noexcept
{
    WorkerAdmission admission(this);
    if (!count || admission.accept())
        return 0;  // Optional: caller can finish every tile.
    std::size_t accepted = 0;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        Node*                       parent = gFrame ? gFrame->mNode : nullptr;
        for (; accepted < count; ++accepted) {
            if (mPhase == Phase::Closed || mOrder == UINT64_MAX || mAccepted == SIZE_MAX)
                break;
            if (parent && !reserveOutstanding(parent))
                break;
            Node* node    = helpers[accepted].get();
            node->mOrder  = ++mOrder;
            node->mParent = NodeRef(parent);
            linkChild(node);
            node->mCommitted.store(true, std::memory_order_relaxed);  // Before ready-queue publication.
            node->mSubmission.store(0, std::memory_order_relaxed);
            retain(node);  // Accepted reference, released by finish.
            node->mActiveNext = mActive;
            if (mActive)
                mActive->mActivePrev = node;
            mActive = node;
            ++mAccepted;
            node->mState.store(au::flow::TaskState::Ready, std::memory_order_release);
        }
    }
    for (std::size_t i = 0; i < accepted; ++i) {
        trace(helpers[i].get(), "Task", 'b');
        publishReady(helpers[i].get());
    }
    grow();
    return accepted;
}

void Core::joinHelper(Node* node) noexcept
{
    CpuSlicePause waiting;
    // This edge already exists: managed helpers reserve their parent's
    // completion, and all callbacks still use the normal wait-cycle checks.
    // Re-validating it through general wait() would allocate and could fail
    // before join, letting a helper retain a pointer to an expired Loop.
    CooperativeWait registration(this, node);
    while (!finalState(node->mState.load(std::memory_order_acquire))) {
        if (gFrame) {
            (void)helpOne(true, std::chrono::steady_clock::time_point::max(), node);
        } else {
            std::unique_lock<std::mutex> lock(node->mWaitMutex);
            node->mChanged.wait(lock, [node] { return finalState(node->mState.load(std::memory_order_acquire)); });
        }
    }
}
struct Loop
{
    Loop(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept
        : mRange(range), mTile(tile), mInvoke(invoke), mPayload(payload)
    {
    }
    const std::size_t        mRange;
    const std::size_t        mTile;
    au_flow_tile             mInvoke;
    void*                    mPayload;
    std::atomic<std::size_t> mNext{0};
    std::atomic<bool>        mFailed{false};
    std::mutex               mMutex;
    std::size_t              mErrorOffset = SIZE_MAX;
    ExternalStatus           mError;
    bool                     mErrorException = false;
    // Bound queue traffic while retaining a scheduling opportunity for other
    // tasks. This is a measured tuning candidate, not a time/fairness guarantee.
    static bool more(void* value) noexcept
    {
        auto& loop = *static_cast<Loop*>(value);
        return !loop.mFailed.load(std::memory_order_acquire) &&
               loop.mNext.load(std::memory_order_relaxed) < loop.mRange;
    }
    static int step(void* value) noexcept
    {
        auto& loop = *static_cast<Loop*>(value);
        if (!more(value))
            return 0;
        std::size_t begin = loop.mNext.load(std::memory_order_relaxed);  // Disjoint tickets, not data publication.
        std::size_t count;
        do {
            if (begin == loop.mRange)
                return 0;
            count = std::min(loop.mTile, loop.mRange - begin);
        } while (!loop.mNext.compare_exchange_weak(begin, begin + count, std::memory_order_relaxed));
        TileContext context{{}, &loop.mFailed};
        int         code;
        {
            TileExtent extent(&context);
            code = loop.mInvoke(loop.mPayload, begin, count);
        }
        // A callback's explicit error wins. Forwarding an equal scoped error
        // retains native provenance; void/zero still reports ignored violations.
        auto       error     = context.mError;
        const bool exception = code == au::flow::kErrorUnhandledException && context.mException;
        if (code && (code != error.mCode || exception))
            error = {code, 0, exception ? au::flow::ErrorDomain::Flow : au::flow::ErrorDomain::Callable, 0};
        if (error.mCode) {
            {
                std::lock_guard<std::mutex> lock(loop.mMutex);
                if (begin < loop.mErrorOffset) {
                    loop.mErrorOffset    = begin;
                    loop.mError          = error;
                    loop.mErrorException = exception;
                }
            }
            loop.mFailed.store(true, std::memory_order_release);  // Stop new tiles; all in-flight tiles still join.
        }
        return 0;
    }
    static int batch(void* value) noexcept
    {
        for (unsigned int i = 0; i < kHelperTileBatch && more(value); ++i)
            (void)step(value);
        return 0;
    }
    static int run(void* value) noexcept
    {
        while (more(value))
            (void)step(value);
        return 0;
    }
};
int Core::tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept
{
    if (!tile || !invoke)
        return au::err::kErrorInvalidParam;
    if (!range)
        return 0;
    if (gCleanupDepth) {
        scopedError(au::flow::kErrorWouldDeadlock);
        return au::flow::kErrorWouldDeadlock;
    }
    // Nested tiles borrow their enclosing Job. Job acceptance and Stopping
    // share mMutex, so shutdown cannot pass a zero counter before a late entry.
    JobScope job(this);
    if (!gTileContext) {
        const int code = job.accept();
        if (code) {
            scopedError(code);
            return code;
        }
    }
    Loop                                       loop{range, tile, invoke, payload};
    std::array<NodeRef, kTileParticipants - 1> helpers;
    std::size_t                                prepared = 0;
    if (!gTileContext && gDepth + 1 < kMaxDispatchDepth) {
        const std::size_t tiles = range / tile + (range % tile != 0);
        const std::size_t participants =
            std::min<std::size_t>(kTileParticipants, std::min(tiles, mBudget.load(std::memory_order_acquire)));
        try {
            for (std::size_t i = 1; i < participants; ++i) {
                auto helper         = make(Kind::Work);
                helper->mClosure    = Closure(&Loop::batch, nullptr, &loop);
                helper->mContinue   = &Loop::more;
                helpers[prepared++] = std::move(helper);
            }
        } catch (...) { /* Optional helpers: caller fallback does not poison the Job. */
        }
    }
    const std::size_t accepted = acceptHelpers(helpers.data(), prepared);
    (void)Loop::run(&loop);  // Same TileContext for caller, helpers and serial fallbacks.
    for (std::size_t i = 0; i < accepted; ++i)
        joinHelper(helpers[i].get());
    if (loop.mErrorException)
        au_flow_record_exception();
    scopedError(loop.mError.mCode, loop.mError.mDomain, loop.mError.mNativeCode, loop.mError.mCauseTaskId);
    return loop.mError.mCode;
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
