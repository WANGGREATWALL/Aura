#include "flow/core.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
#ifdef AURA_FLOW_TESTING
std::atomic<std::uint64_t> gSearches{0}, gSearchNodes{0}, gSearchEdges{0}, gSearchActiveScans{0}, gSearchNanoseconds{0},
    gSearchMaximum{0};
std::atomic<std::uint64_t> gWaitChecks{0}, gWaitLockNanoseconds{0}, gMaximumWaitLock{0};
void                       recordGraphWaitLock(std::uint64_t nanoseconds) noexcept
{
    // Diagnostic only; these relaxed counters publish no scheduler state.
    gWaitChecks.fetch_add(1, std::memory_order_relaxed);
    gWaitLockNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto maximum = gMaximumWaitLock.load(std::memory_order_relaxed);
    while (maximum < nanoseconds &&
           !gMaximumWaitLock.compare_exchange_weak(maximum, nanoseconds, std::memory_order_relaxed)) {
    }
}
void recordGraphSearch(std::uint64_t nodes, std::uint64_t edges, std::uint64_t activeScans,
                       std::uint64_t nanoseconds) noexcept
{
    // Measurement-only counters; they publish no scheduler state.
    gSearches.fetch_add(1, std::memory_order_relaxed);
    gSearchNodes.fetch_add(nodes, std::memory_order_relaxed);
    gSearchEdges.fetch_add(edges, std::memory_order_relaxed);
    gSearchActiveScans.fetch_add(activeScans, std::memory_order_relaxed);
    gSearchNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto maximum = gSearchMaximum.load(std::memory_order_relaxed);
    while (maximum < nanoseconds &&
           !gSearchMaximum.compare_exchange_weak(maximum, nanoseconds, std::memory_order_relaxed)) {
    }
}

std::array<std::atomic<int>, 5>               gFailure{{-1, -1, -1, -1, -1}};
std::atomic<std::size_t>                      gOutstandingLimit{SIZE_MAX};
std::atomic<au::flow::testing::SnapshotHook*> gSnapshotHook{nullptr};
std::mutex                                    gRuntimeHookMutex;
std::condition_variable                       gRuntimeHookChanged;
au::flow::testing::RuntimeHook*               gRuntimeHook    = nullptr;
std::size_t                                   gRuntimeReaders = 0;
void runtimePoint(au::flow::testing::RuntimePoint point, std::size_t id) noexcept
{
    au::flow::testing::RuntimeHook* hook;
    {
        std::lock_guard<std::mutex> lock(gRuntimeHookMutex);
        hook = gRuntimeHook;
        if (!hook)
            return;
        ++gRuntimeReaders;
    }
    hook->mInvoke(hook->mPayload, point, id);  // No scheduler or seam lock held.
    {
        std::lock_guard<std::mutex> lock(gRuntimeHookMutex);
        --gRuntimeReaders;
    }
    gRuntimeHookChanged.notify_all();
}

bool fail(au::flow::testing::Failure failure) noexcept
{
    std::atomic<int>& counter = gFailure[static_cast<unsigned int>(failure)];
    int               value   = counter.load(std::memory_order_relaxed);  // Fault schedule only.
    while (value >= 0) {
        if (counter.compare_exchange_weak(value, value - 1, std::memory_order_relaxed))
            return value == 0;
    }
    return false;
}
#endif

#ifdef AURA_FLOW_TESTING
int Core::queueHooks(const QueueHooks& hooks) noexcept
{
    std::lock_guard<std::mutex> workers(mWorkerMutex);
    std::lock_guard<std::mutex> graph(mMutex);
    if (mWorkerCount.load(std::memory_order_acquire) || mAdmissions || mAccepted || mJobs)
        return au::err::kErrorBusy;
    mReady.testHooks(hooks);  // Private test installation at a quiescent queue only.
    return 0;
}

au::flow::testing::RuntimeStats Core::runtimeStats() noexcept
{
    au::flow::testing::RuntimeStats stats;
    std::lock_guard<std::mutex>     lock(mWorkerMutex);
    stats.mLive         = mWorkerCount.load(std::memory_order_acquire);
    stats.mPollOwner    = mPollOwner.load(std::memory_order_acquire);
    stats.mWaitTimeouts = mWaitTimeouts.load(std::memory_order_relaxed);
    stats.mControlWakes = mControlWakes.load(std::memory_order_relaxed);
    stats.mWorkWakes    = mWorkWakes.load(std::memory_order_relaxed);
    stats.mPollPasses   = mPollPasses.load(std::memory_order_relaxed);
    stats.mRetirements  = mRetirements.load(std::memory_order_relaxed);
    for (std::size_t id = 0; id < mWorkers.size(); ++id) {
        stats.mGenerations[id] = mWorkers[id].mGeneration;
        if (mWorkers[id].mRetirePending)
            ++stats.mRetireBlocked;
    }
    return stats;
}

std::size_t Core::testBudget() const noexcept { return mBudget.load(std::memory_order_acquire); }

int Core::testBudget(std::size_t budget) noexcept
{
    if (!budget || budget > 8)
        return au::err::kErrorInvalidParam;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mJobs)
            return au::err::kErrorBusy;
        for (Node* node = mActive; node; node = node->mActiveNext)
            if (!finalState(node->mState.load(std::memory_order_acquire)))
                return au::err::kErrorBusy;
    }
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        mBudget.store(budget, std::memory_order_release);
        mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
    }
    mPolicyChanged.notify_all();
    mWork.control();
    grow(true);
    return 0;
}

#endif

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#ifdef AURA_FLOW_TESTING
au::flow::testing::GraphStats au::flow::testing::graphStats(bool reset) noexcept
{
    au::flow::testing::GraphStats result;
    const auto                    read = [reset](std::atomic<std::uint64_t>& value) noexcept {
        return reset ? value.exchange(0, std::memory_order_relaxed) : value.load(std::memory_order_relaxed);
    };
    result.mSearches                   = read(au::flow::detail::gSearches);
    result.mNodes                      = read(au::flow::detail::gSearchNodes);
    result.mEdges                      = read(au::flow::detail::gSearchEdges);
    result.mActiveScans                = read(au::flow::detail::gSearchActiveScans);
    result.mSearchNanoseconds          = read(au::flow::detail::gSearchNanoseconds);
    result.mMaximumNanoseconds         = read(au::flow::detail::gSearchMaximum);
    result.mWaitChecks                 = read(au::flow::detail::gWaitChecks);
    result.mWaitLockNanoseconds        = read(au::flow::detail::gWaitLockNanoseconds);
    result.mMaximumWaitLockNanoseconds = read(au::flow::detail::gMaximumWaitLock);
    result.mNodeBytes                  = sizeof(au::flow::detail::Node);
    return result;
}
int au::flow::testing::queueHooks(const au::flow::detail::QueueHooks& hooks) noexcept
{
    return au::flow::detail::core().queueHooks(hooks);
}
void au::flow::testing::runtimeHook(au::flow::testing::RuntimeHook* hook) noexcept
{
    std::unique_lock<std::mutex> lock(au::flow::detail::gRuntimeHookMutex);
    au::flow::detail::gRuntimeHook = nullptr;
    au::flow::detail::gRuntimeHookChanged.wait(lock, [] { return au::flow::detail::gRuntimeReaders == 0; });
    au::flow::detail::gRuntimeHook = hook;
}
au::flow::testing::RuntimeStats au::flow::testing::runtimeStats() noexcept
{
    return au::flow::detail::core().runtimeStats();
}
void au::flow::testing::failAfter(au::flow::testing::Failure failure, int count) noexcept
{
    au::flow::detail::gFailure[static_cast<unsigned int>(failure)].store(count, std::memory_order_relaxed);
}
void au::flow::testing::setOutstandingLimit(std::size_t limit) noexcept
{
    au::flow::detail::gOutstandingLimit.store(limit, std::memory_order_relaxed);
}
void au::flow::testing::snapshotHook(au::flow::testing::SnapshotHook* hook) noexcept
{
    au::flow::detail::gSnapshotHook.store(hook, std::memory_order_release);
}
std::size_t au::flow::testing::budget() noexcept { return au::flow::detail::core().testBudget(); }
int au::flow::testing::setBudget(std::size_t budget) noexcept { return au::flow::detail::core().testBudget(budget); }
#endif
