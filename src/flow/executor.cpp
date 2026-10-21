#include <system_error>

#include "flow/core.h"
#include "flow/graph_search.h"
#include "perf/xperf.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
Core::CooperativeWait::CooperativeWait(Core* core, Node* node) noexcept
    : mCore(core), mNode(gFrame && !gCleanupDepth ? node : nullptr)
{
    if (mNode) {
        std::lock_guard<std::mutex> lock(mNode->mWaitMutex);
        ++mNode->mCooperativeWaiters;
    }
}

Core::CooperativeWait::~CooperativeWait() noexcept
{
    if (!mNode)
        return;
    {
        std::lock_guard<std::mutex> lock(mNode->mWaitMutex);
        --mNode->mCooperativeWaiters;
    }
    mCore->leaveCooperativeWait();
}

int Core::WorkerAdmission::accept(int* native) noexcept
{
    std::unique_lock<std::mutex> lock(mCore->mWorkerMutex);
    if (mCore->mClosingQueries)
        return au::err::kErrorStateInvalid;
    if (mCore->mAdmissions == SIZE_MAX)
        return au::err::kErrorOverflow;
    ++mCore->mAdmissions;
    mAccepted      = true;
    const int code = mCore->eligibleWorkers() ? 0 : mCore->startWorker(lock);
    if (native)
        *native = code ? mCore->mLastWorkerNative : 0;
    lock.unlock();
#ifdef AURA_FLOW_TESTING
    if (!code)
        runtimePoint(au::flow::testing::RuntimePoint::AfterAdmission,
                     gWorker < 0 ? kExternalTraceLane : static_cast<std::size_t>(gWorker));
#endif
    return code;
}

Core::WorkerAdmission::~WorkerAdmission() noexcept
{
    if (!mAccepted)
        return;
    bool wake;
    {
        std::lock_guard<std::mutex> lock(mCore->mWorkerMutex);
        --mCore->mAdmissions;
        wake = mCore->mRetirePending.load(std::memory_order_acquire);
    }
    mCore->mPolicyChanged.notify_all();
    if (wake)
        mCore->mWork.control();
}

namespace {
// Internal linkage also localizes libstdc++'s generated thread-state vtable.
// A member lambda can leave that vtable preemptible across RTLD_GLOBAL plugins
// even when the worker function itself has hidden visibility.
struct WorkerEntry
{
    Core*       mCore;
    std::size_t mId;
    void        operator()() const noexcept;
};
}  // namespace

void Core::publishReady(Node* node) noexcept
{
    mReadyCount.fetch_add(1, std::memory_order_relaxed);  // Scaling hint only.
    mReady.push(&node->mReady);
    mWork.post();
    grow();
}

bool Core::helpOne(bool blocking, std::chrono::steady_clock::time_point deadline, Node* awaited,
                   const std::uint64_t* observed, bool (*externalReady)(void*) noexcept, void* externalPayload) noexcept
{
    if (gDepth >= kMaxDispatchDepth)
        return false;
    const auto epoch = observed ? *observed : mWork.epoch();
    if (mStop.load(std::memory_order_acquire))
        return false;
    if (gWorker >= 0)
        applyPolicy(static_cast<std::size_t>(gWorker));
    const bool eligible = gWorker < 0 || static_cast<std::size_t>(gWorker) < mBudget.load(std::memory_order_acquire);
    if (eligible && (gCompletionBurst < kCompletionBurst || mReadyCount.load(std::memory_order_relaxed) == 0) &&
        pumpExternal(deadline)) {
        ++gCompletionBurst;
        return true;
    }
    if (awaited && finalState(awaited->mState.load(std::memory_order_acquire)))
        return false;
    if (externalReady && externalReady(externalPayload))
        return false;
    if (!eligible)
        releasePoller();
    if (blocking) {
#ifdef AURA_FLOW_TESTING
        runtimePoint(au::flow::testing::RuntimePoint::BeforeSleep,
                     gWorker < 0 ? kExternalTraceLane : static_cast<std::size_t>(gWorker));
#endif
        const auto wake = mWork.acquireUntil(epoch, deadline, eligible);
#ifdef AURA_FLOW_TESTING
        if (wake == au::sys::flow::WorkSemaphore::Wake::Timeout)
            mWaitTimeouts.fetch_add(1, std::memory_order_relaxed);
        else if (wake == au::sys::flow::WorkSemaphore::Wake::Control)
            mControlWakes.fetch_add(1, std::memory_order_relaxed);
        else
            mWorkWakes.fetch_add(1, std::memory_order_relaxed);
#endif
        if (wake != au::sys::flow::WorkSemaphore::Wake::Work)
            return false;
    } else if (!eligible || !mWork.tryAcquire())
        return false;
    if (gWorker >= 0 && static_cast<std::size_t>(gWorker) >= mBudget.load(std::memory_order_acquire)) {
        mWork.post();  // Return an acquired real permit if the budget changed.
        return false;
    }
    releasePoller();  // Dispatch may run a long callable; transfer timer ownership first.
    ReadyLink*   link          = nullptr;
    bool         overflowFirst = false;
    bool         completion    = false;
    unsigned int attempts      = 0;
    for (;;) {
        if (gCompletionBurst < kCompletionBurst && mCompletions.pop(link, overflowFirst)) {
            completion = true;
            break;
        }
        if (mReady.pop(link, overflowFirst))
            break;
        if (mCompletions.pop(link, overflowFirst)) {
            completion = true;
            break;
        }
        if (++attempts == kQueueProbeLimit) {
#ifdef AURA_FLOW_TESTING
            runtimePoint(au::flow::testing::RuntimePoint::QueueProbeMiss,
                         gWorker < 0 ? kExternalTraceLane : static_cast<std::size_t>(gWorker));
#endif
            mWork.post();  // Preserve the real permit; recheck target/control/timeout outside this bounded probe.
            return false;
        }
        overflowFirst = !overflowFirst;
        std::this_thread::yield();  // Keep the acquired permit across transient publication gaps.
    }
    if (completion) {
        auto* token = static_cast<ExternalToken*>(link->mValue);
        if (!consumeExternal(token))
            deferExternal(token);
        ++gCompletionBurst;
        return true;
    }
    gCompletionBurst = 0;
    mReadyCount.fetch_sub(1, std::memory_order_relaxed);  // Scaling hint only.
    Node* node = static_cast<Node*>(link->mValue);
    for (unsigned int burst = 0; node; ++burst) {
        Node* next = nullptr;
        run(node, gWorker >= 0 && burst + 1 < kContinuationBurst ? &next : nullptr);
        node = next;
        if (node) {
            applyPolicy(static_cast<std::size_t>(gWorker));
            if (static_cast<std::size_t>(gWorker) >= mBudget.load(std::memory_order_acquire)) {
                publishReady(node);  // Budget changed: return this unposted continuation to the queue.
                break;
            }
        }
    }
    return true;
}

void Core::run(Node* node, Node** continuation) noexcept
{
    CpuSlicePause  outerSlice;
    TileSuspension tileSuspension;
    NodeRef        pin(node);
    node->mState.store(au::flow::TaskState::Running, std::memory_order_release);
    Frame frame{node, gFrame};
    if (frame.mPrevious) {
        std::lock_guard<std::mutex> lock(mMutex);
        frame.mPrevious->mNode->mExecutingChild = node;
    }
    gFrame = &frame;
    ExecutionErrors memoryErrors(this);
    ++gDepth;
    // External tiled callers provide extra capacity, not busy pool workers.
    if (gWorker >= 0)
        mBusy.fetch_add(1, std::memory_order_relaxed);  // Scaling hint; cooperative stacks may overlap.
    if (mPendingExternalCount.load(std::memory_order_acquire))
        grow();  // Preserve a query consumer when dispatch takes its leader.
    int code = 0;
    trace(node, "callable wall", 'B');  // Includes all waits inside the function.
    frame.mCpuActive = true;
    trace(node, "CPU slice", 'B');  // Excludes known waits; not OS scheduled CPU time.
    if (node->mClosure.mInvoke) {
        try {
            if (mDiagnostics.enabled()) {
                au::perf::XPerfScoped scope(node->mName);
                code = node->mClosure.mInvoke(node->mClosure.mPayload);
            } else
                code = node->mClosure.mInvoke(node->mClosure.mPayload);
        } catch (...) {
            code = au::flow::kErrorUnhandledException;
            node->mErrors.exception();
        }
    }
    if (frame.mCpuActive) {
        trace(node, "CPU slice", 'E', code);
        frame.mCpuActive = false;
    }
    trace(node, "callable wall", 'E', code);
    memoryErrors.restore();
    if (gWorker >= 0)
        mBusy.fetch_sub(1, std::memory_order_relaxed);
    --gDepth;
    gFrame = frame.mPrevious;
    if (node->mContinue && node->mContinue(node->mClosure.mPayload)) {
        node->mState.store(au::flow::TaskState::Ready, std::memory_order_release);
        mReadyCount.fetch_add(1, std::memory_order_relaxed);  // Helper yields after a bounded tile batch.
        mReady.push(&node->mReady);
        mWork.post();
        grow();
        if (frame.mPrevious) {
            std::lock_guard<std::mutex> lock(mMutex);
            frame.mPrevious->mNode->mExecutingChild = nullptr;
        }
        return;
    }
    bodyEnd(node, code, continuation);
    if (frame.mPrevious) {
        std::lock_guard<std::mutex> lock(mMutex);
        frame.mPrevious->mNode->mExecutingChild = nullptr;
    }
}

void Core::leaveCooperativeWait() noexcept
{
    if (!gFrame || gWorker < 0 || gCleanupDepth)
        return;
    releasePoller();  // The callable resumes here; it must not retain an idle query lease.
    if (mPendingExternalCount.load(std::memory_order_acquire))
        grow();
}

int Core::wait(Node* node, std::uint64_t timeout, au_flow_result* out) noexcept
{
    CpuSlicePause waiting;
    if (!owns(node))
        return au::err::kErrorInvalidHandle;
    NodeRef pin(node);
    if (node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft)
        return au::err::kErrorNotReady;
    if (out && out->mStructSize != sizeof(au_flow_result))
        return au::err::kErrorVersionMismatch;
    if (gCleanupDepth && !finalState(node->mState.load(std::memory_order_acquire)))
        return au::flow::kErrorWouldDeadlock;
    const auto now = std::chrono::steady_clock::now();
    const auto maxMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - now)
            .count();
    const auto deadline = timeout >= static_cast<std::uint64_t>(maxMs) ? std::chrono::steady_clock::time_point::max()
                                                                       : now + std::chrono::milliseconds(timeout);
    Node*      current  = gFrame ? gFrame->mNode : nullptr;
    if (current && !finalState(node->mState.load(std::memory_order_acquire))) {
        try {
            std::lock_guard<std::mutex> lock(mMutex);
#ifdef AURA_FLOW_TESTING
            GraphWaitLock graphWaitLock;
#endif
            GraphTargets targets(nullptr, false, gFrame);
            GraphSearch  search(true);
            search.add(node);
            bool deadlock = gDepth >= kMaxDispatchDepth || search.reachesTarget();
            for (Node* p = current; p; p = p->mParent.get())
                if (p == node)
                    deadlock = true;
            if (deadlock)
                return au::flow::kErrorWouldDeadlock;
            current->mWaitTarget = node;
        } catch (...) {
            return au::err::kErrorNoMemory;
        }
    }
    CooperativeWait registration(this, node);
    int             ret = 0;
    while (!finalState(node->mState.load(std::memory_order_acquire))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ret = au::err::kErrorTimeout;
            break;
        }
        if (current) {
            (void)helpOne(true, deadline, node);
            continue;
        }
        std::unique_lock<std::mutex> lock(node->mWaitMutex);
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            node->mChanged.wait(lock, [node] { return finalState(node->mState.load(std::memory_order_acquire)); });
        } else {
            node->mChanged.wait_until(lock, deadline,
                                      [node] { return finalState(node->mState.load(std::memory_order_acquire)); });
        }
    }
    if (current) {
        std::lock_guard<std::mutex> lock(mMutex);
        current->mWaitTarget = nullptr;
    }
    if (!ret && out)
        *out = node->mResult;
    return ret;
}

int Core::shutdown() noexcept
{
    if (gFrame || gTileContext || gCleanupDepth)
        return au::flow::kErrorWouldDeadlock;
    std::lock_guard<std::mutex> closing(mCloseMutex);
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mPhase == Phase::Closed)
            return 0;
        mPhase = Phase::Stopping;
        mDrained.wait(lock, [this] { return mAccepted == 0 && mJobs == 0; });
    }
    {
        std::unique_lock<std::mutex> lock(mWorkerMutex);
        mClosingQueries = true;
        mPolicyChanged.wait(lock, [this] { return mAffinityQueries == 0 && mAdmissions == 0 && mJoiners == 0; });
        mStop.store(true, std::memory_order_release);
    }
    mWork.control();  // Control wakes never manufacture queue permits.
    for (auto& worker : mWorkers)
        if (worker.mThread.joinable())
            worker.mThread.join();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPhase = Phase::Closed;
    }
    return 0;
}

std::size_t Core::eligibleWorkers() const noexcept
{
    std::size_t       count  = 0;
    const std::size_t budget = mBudget.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < budget; ++i)
        if (mWorkers[i].mState == WorkerState::Running)
            ++count;
    return count;  // mWorkerMutex held.
}

std::size_t Core::workDemand() const noexcept
{
    return std::min(mBudget.load(std::memory_order_acquire),
                    mReadyCount.load(std::memory_order_relaxed) + mBusy.load(std::memory_order_relaxed) +
                        (mPendingExternalCount.load(std::memory_order_acquire) ? 1 : 0));
}

void Core::grow(bool force) noexcept
{
    if (!force && mEligibleCount.load(std::memory_order_acquire) >= workDemand())
        return;
    std::unique_lock<std::mutex> lock(mWorkerMutex);
    // startWorker can unlock to join; policy and demand may change during
    // that interval. Never spin toward a capacity above the current budget.
    while (!mClosingQueries && eligibleWorkers() < workDemand())
        if (startWorker(lock))
            break;
}

int Core::startWorker(std::unique_lock<std::mutex>& lock) noexcept
{
    mLastWorkerNative = 0;
    for (;;) {
        if (mClosingQueries)
            return au::err::kErrorStateInvalid;
        const std::size_t budget   = mBudget.load(std::memory_order_acquire);
        bool              retiring = false;
        for (std::size_t id = 0; id < budget; ++id) {
            auto& worker = mWorkers[id];
            if (worker.mState == WorkerState::Exited) {
                std::thread previous = std::move(worker.mThread);
                worker.mState        = WorkerState::Joining;
                ++mJoiners;
                lock.unlock();
                previous.join();  // No graph, worker, gate or provider lock held.
                lock.lock();
                --mJoiners;
                worker.mState = WorkerState::Free;
                mPolicyChanged.notify_all();
                break;  // Recheck shutdown/budget and all slots after the unlocked join.
            }
            if (worker.mState == WorkerState::Retiring || worker.mState == WorkerState::Joining) {
                retiring = true;
                continue;
            }
            if (worker.mState != WorkerState::Free)
                continue;
#ifdef AURA_FLOW_TESTING
            if (fail(au::flow::testing::Failure::Worker))
                return au::err::kErrorPlatformAPI;
#endif
            if (worker.mGeneration == UINT64_MAX)
                return au::err::kErrorOverflow;
            try {
                worker.mThreadId = 0;
                worker.mAppliedVersion.store(UINT64_MAX,
                                             std::memory_order_relaxed);  // New generation, not running yet.
                worker.mAffinityCode = au::err::kErrorNotReady;
                worker.mThread       = std::thread(WorkerEntry{this, id});
                ++worker.mGeneration;
                worker.mState = WorkerState::Running;
                mWorkerCount.fetch_add(1, std::memory_order_release);
                mEligibleCount.store(eligibleWorkers(), std::memory_order_release);  // Enqueue-side capacity hint.
                return 0;
            } catch (const std::system_error& error) {
                mLastWorkerNative = error.code().value();
                return au::err::kErrorPlatformAPI;
            } catch (const std::bad_alloc&) {
                return au::err::kErrorNoMemory;
            } catch (...) {
                return au::err::kErrorPlatformAPI;
            }
        }
        bool reusable = false;
        for (std::size_t id = 0; id < budget; ++id)
            if (mWorkers[id].mState == WorkerState::Free || mWorkers[id].mState == WorkerState::Exited)
                reusable = true;
        if (reusable)
            continue;
        if (!retiring)
            return 0;               // Budget already filled by another grow/admission.
        mPolicyChanged.wait(lock);  // A retiring/joining slot must finish before reuse.
    }
}

void Core::updateRetirementPending() noexcept
{
    bool pending = false;
    for (const auto& worker : mWorkers)
        pending = pending || worker.mRetirePending;
    mRetirePending.store(pending, std::memory_order_release);
}

bool Core::retire(std::size_t id) noexcept
{
    std::lock_guard<std::mutex> workers(mWorkerMutex);
    auto&                       worker = mWorkers[id];
    if (id < mBudget.load(std::memory_order_acquire) && mPendingExternalCount.load(std::memory_order_acquire)) {
        int unclaimed = -1;
        if (mPollOwner.compare_exchange_strong(unclaimed, static_cast<int>(id), std::memory_order_acq_rel)) {
            worker.mPollDue       = std::chrono::steady_clock::now();
            worker.mPollDelay     = kPollMinimum;
            worker.mPollRemaining = 0;
            return false;  // Become the idle query consumer instead of retiring behind a busy worker.
        }
        if (unclaimed == static_cast<int>(id))
            return false;
    }
    // Worker -> graph is the only nested order. Admissions never hold the
    // graph mutex while obtaining the worker mutex or joining a thread.
    std::lock_guard<std::mutex> graph(mMutex);
    worker.mRetirePending = true;
    updateRetirementPending();  // Before checking counters, under their writers' mutexes.
    const bool lastConsumer = mWorkerCount.load(std::memory_order_relaxed) == 1 ||
                              (id < mBudget.load(std::memory_order_acquire) && eligibleWorkers() == 1);
    if (worker.mPins || (lastConsumer && (mAdmissions || mAccepted)))
        return false;
    worker.mRetirePending = false;
    worker.mState         = WorkerState::Retiring;
    mWorkerCount.fetch_sub(1, std::memory_order_release);
    mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
    updateRetirementPending();
    return true;
}

void Core::workerLoop(std::size_t id) noexcept
{
    gWorker = static_cast<int>(id);
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkers[id].mThreadId = au::sys::flow::threadId();
    }
    auto lastActive = std::chrono::steady_clock::now();
    while (!mStop.load(std::memory_order_acquire)) {
        const auto epoch = mWork.epoch();  // Before checking policy/retirement predicates.
        applyPolicy(id);
        const auto idleDeadline = lastActive + kIdleTimeout;
        auto       deadline     = idleDeadline;
        if (std::chrono::steady_clock::now() >= idleDeadline) {
            if (retire(id)) {
#ifdef AURA_FLOW_TESTING
                mRetirements.fetch_add(1, std::memory_order_relaxed);
                runtimePoint(au::flow::testing::RuntimePoint::AfterRetire, id);
#endif
                break;
            }
            deadline =
                std::chrono::steady_clock::time_point::max();  // Counters/pins signal changes, no periodic retry.
        }
        if (id >= mBudget.load(std::memory_order_acquire)) {
            releasePoller();
            (void)mWork.acquireUntil(epoch, deadline, false);
            continue;
        }
        if (helpOne(true, deadline, nullptr, &epoch)) {
            lastActive = std::chrono::steady_clock::now();
            if (!mRetirePending.load(std::memory_order_acquire))
                continue;
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            if (mWorkers[id].mRetirePending) {
                mWorkers[id].mRetirePending = false;
                updateRetirementPending();
            }
        }
    }
    releasePoller();
    gWorker = -1;
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        auto&                       worker = mWorkers[id];
        if (worker.mState == WorkerState::Running)
            mWorkerCount.fetch_sub(1, std::memory_order_release);
        worker.mRetirePending = false;
        worker.mState         = WorkerState::Exited;  // No Core access after notifying slot waiters.
        mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
        updateRetirementPending();
    }
    mPolicyChanged.notify_all();
}

void Core::applyPolicy(std::size_t id) noexcept
{
    if (mWorkers[id].mAppliedVersion.load(std::memory_order_acquire) == mPolicyVersion.load(std::memory_order_acquire))
        return;
    std::vector<std::uint32_t> cpus;
    std::uint64_t              version;
    try {
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            version = mPolicyVersion.load(std::memory_order_relaxed);
            if (mWorkers[id].mAppliedVersion.load(std::memory_order_relaxed) == version)
                return;
            cpus = mPolicy.empty() ? mAllowed : mPolicy;
        }
        const int                   code = version == 0 || cpus.empty() ? 0 : au::sys::flow::applyAffinity(cpus);
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkers[id].mAppliedVersion.store(version, std::memory_order_release);
        mWorkers[id].mAffinityCode = code;  // Effective affinity is queried on demand, not cached.
    } catch (...) {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mWorkers[id].mAffinityCode = au::err::kErrorNoMemory;
    }
}

int Core::affinity(std::uint32_t mode, const std::uint32_t* cpus, std::size_t count) noexcept
{
    if (mode > 1 || (mode && (!cpus || !count)))
        return au::err::kErrorInvalidParam;
    if (mode && au::sys::flow::affinitySupported() != 0)
        return au::err::kErrorNotSupported;
    try {
        std::vector<std::uint32_t> selected;
        if (mode)
            for (std::size_t i = 0; i < count; ++i)
                if (std::find(mAllowed.begin(), mAllowed.end(), cpus[i]) != mAllowed.end())
                    selected.push_back(cpus[i]);
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        if (mode && selected.empty())
            return au::err::kErrorInvalidParam;
        {
            std::unique_lock<std::mutex> lock(mWorkerMutex);
            if (mClosingQueries)
                return au::err::kErrorStateInvalid;
            const auto budget =
                std::max<std::size_t>(1, std::min<std::size_t>(kMaxWorkers, mode ? selected.size() : mAllowed.size()));
            bool consumer = false;
            for (std::size_t id = 0; id < budget; ++id)
                consumer = consumer || mWorkers[id].mState == WorkerState::Running;
            if (!consumer && mWorkerCount.load(std::memory_order_relaxed)) {
                const int code = startWorker(lock);
                if (code)
                    return code;  // Do not strand accepted work by narrowing to empty live slots.
            }
            if (mClosingQueries)
                return au::err::kErrorStateInvalid;
            if (mPolicyVersion.load(std::memory_order_relaxed) == UINT64_MAX)
                return au::err::kErrorOverflow;
            mPolicy = std::move(selected);
            mPolicyVersion.fetch_add(1,
                                     std::memory_order_release);  // Publish policy edits before dispatch fast checks.
            mBudget.store(budget, std::memory_order_release);
            mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
        }
        mPolicyChanged.notify_all();
        mWork.control();
        grow(true);
        return 0;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}

int Core::workers(au_flow_worker* out, std::size_t capacity, std::size_t* count) noexcept
{
    if (!count || (capacity && !out))
        return au::err::kErrorInvalidParam;
    std::lock_guard<std::mutex> lock(mWorkerMutex);
    *count = 0;
    for (std::size_t id = 0; id < mWorkers.size(); ++id) {
        const auto& worker = mWorkers[id];
        if (worker.mState != WorkerState::Running)
            continue;
        const auto index = (*count)++;
        if (index >= capacity)
            continue;
        if (out[index].mStructSize != sizeof(au_flow_worker))
            return au::err::kErrorVersionMismatch;
        out[index] = {sizeof(au_flow_worker),
                      static_cast<std::uint32_t>(id),
                      worker.mThreadId,
                      mPolicyVersion.load(std::memory_order_relaxed),
                      worker.mAppliedVersion.load(std::memory_order_acquire),
                      worker.mAffinityCode};
    }
    return capacity < *count ? au::err::kErrorBufferUnderflow : 0;
}

int Core::workerAffinity(std::uint32_t id, std::uint32_t* cpus, std::size_t capacity, std::size_t* count) noexcept
{
    if (!count || (capacity && !cpus))
        return au::err::kErrorInvalidParam;
    std::uint64_t thread;
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        if (id >= mWorkers.size())
            return au::err::kErrorOutOfRange;
        if (mClosingQueries)
            return au::err::kErrorStateInvalid;
        if (mWorkers[id].mState != WorkerState::Running)
            return au::err::kErrorNotReady;
        thread = mWorkers[id].mThreadId;
        if (!thread)
            return au::err::kErrorNotReady;
        if (mAffinityQueries == SIZE_MAX || mWorkers[id].mPins == SIZE_MAX)
            return au::err::kErrorOverflow;
        ++mAffinityQueries;
        ++mWorkers[id].mPins;  // Pin this generation until the native query returns.
    }
#ifdef AURA_FLOW_TESTING
    runtimePoint(au::flow::testing::RuntimePoint::QueryPinned, id);
#endif
    std::vector<std::uint32_t> effective;
    int                        code = au::sys::flow::affinitySupported();
    if (!code)
        code = au::sys::flow::threadAffinity(thread, effective);
    bool wake;
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        --mAffinityQueries;
        --mWorkers[id].mPins;
        wake = mWorkers[id].mRetirePending;
    }
    mPolicyChanged.notify_all();
    if (wake)
        mWork.control();
    if (code)
        return code;
    *count = effective.size();
    if (cpus)
        std::copy_n(effective.begin(), std::min(capacity, *count), cpus);
    return capacity < *count ? au::err::kErrorBufferUnderflow : 0;
}
void WorkerEntry::operator()() const noexcept { mCore->workerLoop(mId); }

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
