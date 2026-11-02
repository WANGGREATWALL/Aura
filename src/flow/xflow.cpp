#include <algorithm>
#include <exception>
#include <set>
#include <utility>

#include "flow/xflow_core.h"
#include "log/xlogger.h"
#include "perf/xtimer.h"
#include "perf/xtracer.h"
#include "sys/xsystem.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
thread_local Node*         gCurrentTask              = nullptr;
thread_local std::size_t   gWorkerIndex              = 0;
thread_local unsigned      gTileDepth                = 0;
thread_local unsigned      gTaskDepth                = 0;
thread_local std::uint64_t gAttemptedAffinityVersion = 0;

template <class F>
int boundary(F fn, const char* name = nullptr) noexcept
{
    try {
        return fn();
    } catch (const std::bad_alloc& error) {
        XLOG_E("flow [%s]: %s\n", name ? name : "operation", error.what());
        return au::err::kErrorNoMemory;
    } catch (const std::exception& error) {
        XLOG_E("flow [%s]: %s\n", name ? name : "operation", error.what());
        return au::err::kErrorUnknown;
    } catch (...) {
        XLOG_E("flow [%s]: non-standard exception\n", name ? name : "operation");
        return au::err::kErrorUnknown;
    }
}

// Reserve before changing links/counts; the eventual append must not allocate.
void reserveOne(std::vector<NodePtr>& values)
{
    if (values.size() == values.capacity()) {
        const auto limit = values.max_size();
        if (values.size() == limit)
            throw std::bad_alloc();
        values.reserve(values.empty() ? 1 : values.size() + std::min(values.size(), limit - values.size()));
    }
}

void PayloadDeleter::operator()(void* data) const noexcept
{
    if (mDestroy)
        mDestroy(data);
    if (mAlignment > alignof(std::max_align_t))
        ::operator delete(data, std::align_val_t(mAlignment));
    else if (mAlignment)
        ::operator delete(data);
}

Core& core()
{
    static Core instance;
    return instance;
}

Core::Core()
{
    (void)au::perf::Config::get();  // Perf and log state outlive worker teardown.
    (void)au::log::Config::get();
    std::size_t count = 0;
    mAffinityInitCode = au::sys::getThreadAffinity(nullptr, 0, count);
    if (!mAffinityInitCode) {
        mInitialCpus.resize(count);
        mAffinityInitCode = au::sys::getThreadAffinity(mInitialCpus.data(), mInitialCpus.size(), count);
        if (mAffinityInitCode)
            mInitialCpus.clear();
        else {
            mInitialCpus.resize(count);
            for (auto cpu : mInitialCpus)
                if (cpu < 64)
                    mInitialCpuMask |= UINT64_C(1) << cpu;
        }
    }
}

Core::~Core()
{
    std::unique_lock<std::mutex> lock(mMutex);
    mChanged.wait(lock, [&] { return !mReleasing; });
    mClosing = true;
    mChanged.wait(lock, [&] { return mAccepted.empty() && !mTileCallers; });
    mStop = true;
    mChanged.notify_all();
    lock.unlock();
    for (auto& thread : mThreads)
        if (thread.joinable())
            thread.join();
}

bool Core::reaches(Node* from, Node* target, bool includeRuntimeEdges)
{
    std::vector<Node*> stack{from};
    std::set<Node*>    seen;
    while (!stack.empty()) {
        auto* n = stack.back();
        stack.pop_back();
        if (!n || !seen.insert(n).second)
            continue;
        if (n == target)
            return true;
        for (auto& pred : n->mPredecessors)
            stack.push_back(pred.get());
        if (includeRuntimeEdges) {
            stack.push_back(n->mWaiting);
            stack.push_back(n->mExecuting);
        }
    }

    return false;
}

int Core::addDependency(const NodePtr& predecessor, const NodePtr& successor)
{
    if (!successor)
        return au::err::kErrorInvalidParam;
    // Core::mMutex protects links and state; public build-error queries also need the node lock.
    auto error = [&](int code) {
        std::lock_guard<std::mutex> lock(successor->mMutex);
        if (successor->mState == TaskState::Created && !successor->mBuildCode)
            successor->mBuildCode = code;
        return code;
    };

    if (predecessor == successor)
        return error(au::err::kErrorFlowCycleDetected);
    if (!predecessor)
        return error(au::err::kErrorInvalidParam);

    if (successor->mState != TaskState::Created)
        return au::err::kErrorStateInvalid;
    try {
        if (std::find(successor->mPredecessors.begin(), successor->mPredecessors.end(), predecessor) !=
            successor->mPredecessors.end())
            return au::err::kSuccess;
        // Closure submission accepts every upstream node, and accepted targets reject new edges.
        // Thus an accepted predecessor cannot depend on a Created successor. If that rule changes, revisit this guard.
        // A Created successor has never finished, so all of its outgoing edges are in mSuccessors;
        // an empty list rules out a return path. Expired weak entries merely keep DFS conservative.
        if (predecessor->mState == TaskState::Created && !successor->mSuccessors.empty() &&
            reaches(predecessor.get(), successor.get()))
            return error(au::err::kErrorFlowCycleDetected);
        reserveOne(successor->mPredecessors);
        if (predecessor->mState != TaskState::Done) {
            predecessor->mSuccessors.emplace_back(successor);
            ++successor->mPendingDependencyCount;
        } else if (!successor->mDependencyCode)
            successor->mDependencyCode = predecessor->mResultCode;
        successor->mPredecessors.push_back(predecessor);
    } catch (...) {
        return error(au::err::kErrorNoMemory);
    }

    return au::err::kSuccess;
}

void Core::enqueueReady(Node* node)
{
    node->mState = TaskState::Ready;
    if (mReadyTail)
        mReadyTail->mNextReady = node;
    else
        mReadyHead = node;
    mReadyTail = node;
}

void Core::growWorkers()
{
    if (!mIdleWorkerCount && mWorkerCount < mThreads.size()) {
        const auto workerIndex = mWorkerCount + 1;
        mThreads[mWorkerCount] = std::thread([this, workerIndex] { workerLoop(workerIndex); });
        ++mWorkerCount;
    }
}

int Core::submit(const NodePtr& root)
{
    if (root->mState != TaskState::Created)
        return au::err::kSuccess;
    if (root->mBuildCode)
        return root->mBuildCode;
    // Acquire payload construction published by commit before accepting execution.
    if (!root->mPayloadReady.load(std::memory_order_acquire))
        return au::err::kErrorNotReady;
    std::list<NodePtr> batch{root};
    // Ordinary independent tasks need only an accepted-list entry, not graph containers.
    if (!root->mPredecessors.empty()) {
        std::vector<NodePtr> stack(root->mPredecessors);
        std::set<Node*>      collected{root.get()};
        while (!stack.empty()) {
            auto n = std::move(stack.back());
            stack.pop_back();
            if (n->mState != TaskState::Created || !collected.insert(n.get()).second)
                continue;
            if (n->mBuildCode)
                return n->mBuildCode;
            if (!n->mPayloadReady.load(std::memory_order_acquire))
                return au::err::kErrorNotReady;
            batch.push_back(n);
            stack.insert(stack.end(), n->mPredecessors.begin(), n->mPredecessors.end());
        }
    }
    if (mClosing && !gCurrentTask && !gTileDepth)
        return au::err::kErrorStateInvalid;
    growWorkers();  // No accepted state if the required thread could not start.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        auto&                       n = *it;
        std::lock_guard<std::mutex> lock(n->mMutex);
        n->mAcceptedPos = it;
        n->mState       = TaskState::Pending;
        if (!n->mPendingDependencyCount)
            enqueueReady(n.get());
    }

    mAccepted.splice(mAccepted.end(), batch);
    mChanged.notify_all();
    return au::err::kSuccess;
}

TileJob* Core::findTileJob()
{
    for (auto* j = mActiveTileJobs; j; j = j->mNextJob)
        if (j->mActiveHelpers.load(std::memory_order_relaxed) < kMaxTileHelpers &&
            j->mNextTileIndex.load(std::memory_order_relaxed) < j->mTileCount)
            return j;
    return nullptr;
}

void Core::applyAffinity(std::unique_lock<std::mutex>& lock)
{
    for (;;) {
        const auto version = mAffinityVersion.load(std::memory_order_acquire);
        if (!gWorkerIndex || gAttemptedAffinityVersion == version || mInitialCpus.empty())
            break;
        const auto mask           = mRequestedCpuMask;
        const bool useInitialCpus = !mask;
        lock.unlock();
        std::array<std::uint32_t, 64> selectedCpus;
        std::size_t                   cpuCount = 0;
        if (!useInitialCpus)
            for (unsigned i = 0; i < 64; ++i)
                if (mask & (UINT64_C(1) << i))
                    selectedCpus[cpuCount++] = i;
        const int error = au::sys::setThreadAffinity(useInitialCpus ? mInitialCpus.data() : selectedCpus.data(),
                                                     useInitialCpus ? mInitialCpus.size() : cpuCount);
        if (error)
            XLOG_W("flow worker %zu: affinity application failed (%d)", gWorkerIndex, error);
        // One attempt per configuration, outside scheduler locks. Reconfigure to retry.
        gAttemptedAffinityVersion = version;
        lock.lock();
        if (mAffinityVersion.load(std::memory_order_acquire) == version)
            break;
    }
}

bool Core::helpOne(std::unique_lock<std::mutex>& lock)
{
    if (mStop)
        return false;
    applyAffinity(lock);

    if (auto* job = gWorkerIndex ? findTileJob() : nullptr) {
        // Register under mMutex so the caller cannot destroy the job before this helper exits.
        job->mActiveHelpers.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        job->run(gWorkerIndex);
        lock.lock();
        job->mActiveHelpers.fetch_sub(1, std::memory_order_relaxed);
        mChanged.notify_all();
        return true;
    }

    if (!mReadyHead || gTaskDepth >= kMaxTaskDepth)
        return false;
    NodePtr task = *mReadyHead->mAcceptedPos;
    mReadyHead   = task->mNextReady;
    if (!mReadyHead)
        mReadyTail = nullptr;
    task->mNextReady = nullptr;
    {
        std::lock_guard<std::mutex> nodeLock(task->mMutex);
        task->mState = TaskState::Running;
    }

    Node* previousTask = gCurrentTask;
    if (previousTask)
        previousTask->mExecuting = task.get();
    if (mReadyHead) {
        try {
            growWorkers();
        } catch (...) {
            // Expansion is optional; the current executor can continue draining work.
        }
    }

    lock.unlock();
    gCurrentTask = task.get();
    ++gTaskDepth;
    int                 code  = task->mDependencyCode;
    const auto          start = std::chrono::steady_clock::now();
    au::perf::PerfScope trace{};
    const bool          traced = !code && !task->mName.empty();
    if (traced)
        au::perf::traceBegin(&trace, task->mName);
    if (!code)
        code = boundary([&]() -> int { return task->mFunction(task->mPayload.get()); },
                        task->mName.empty() ? "unnamed task" : task->mName.c_str());
    if (traced)
        au::perf::traceEnd(&trace);
    const auto costUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    task->mPayload.reset();
    --gTaskDepth;
    gCurrentTask = previousTask;
    lock.lock();
    if (previousTask)
        previousTask->mExecuting = nullptr;
    std::vector<std::weak_ptr<Node>> successors;
    std::vector<NodePtr>             predecessors;
    {
        std::lock_guard<std::mutex> nodeLock(task->mMutex);
        task->mResultCode = code;
        task->mCostUs     = costUs;
        task->mState      = TaskState::Done;
        successors.swap(task->mSuccessors);
        predecessors.swap(
            task->mPredecessors);  // Completed dependencies no longer participate in scheduling or wait cycles.
    }

    for (auto& weak : successors)
        if (auto succ = weak.lock()) {
            {
                std::lock_guard<std::mutex> successorLock(succ->mMutex);
                if (code && !succ->mDependencyCode)
                    succ->mDependencyCode = code;
                if (!--succ->mPendingDependencyCount && succ->mState == TaskState::Pending)
                    enqueueReady(succ.get());
            }
            lock.unlock();
            // This temporary owner may be the last one; payload cleanup must run outside mMutex.
            succ.reset();
            lock.lock();
        }
    mAccepted.erase(task->mAcceptedPos);
    mChanged.notify_all();
    lock.unlock();
    predecessors.clear();
    task.reset();
    lock.lock();  // Last owners/deleters never run under scheduler locks.
    return true;
}

void Core::workerLoop(std::size_t workerIndex)
{
    gWorkerIndex = workerIndex;
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStop) {
        const auto observedAffinityVersion = mAffinityVersion.load(std::memory_order_acquire);
        if (helpOne(lock))
            continue;
        ++mIdleWorkerCount;
        mChanged.wait(lock, [&] {
            return mStop || mReadyHead || findTileJob() ||
                   mAffinityVersion.load(std::memory_order_acquire) != observedAffinityVersion;
        });
        --mIdleWorkerCount;
    }
}

int Core::wait(const NodePtr& task)
{
    if (!task)
        return au::err::kErrorInvalidParam;
    std::unique_lock<std::mutex> lock(mMutex);
    if (task->mState == TaskState::Done)
        return task->mResultCode;
    if (task->mState == TaskState::Created)
        return task->mBuildCode ? task->mBuildCode : au::err::kErrorStateInvalid;
    if (gTileDepth || gTaskDepth >= kMaxTaskDepth || (gCurrentTask && reaches(task.get(), gCurrentTask, true)))
        return au::err::kErrorFlowWouldDeadlock;
    Node*  unused  = nullptr;
    Node*& waiting = gCurrentTask ? gCurrentTask->mWaiting : unused;

    struct Restore
    {
        Node*& mSlot;
        Node*  mOld;

        ~Restore() { mSlot = mOld; }
    } restore{waiting, waiting};

    waiting = task.get();
    while (task->mState != TaskState::Done)
        if (!helpOne(lock))
            mChanged.wait(lock);
    return task->mResultCode;
}

void TileJob::run(std::size_t workerIndex) noexcept
{
    ++gTileDepth;
    std::size_t tileIndex = mNextTileIndex.load(std::memory_order_relaxed);
    while (tileIndex < mTileCount) {
        if (!mNextTileIndex.compare_exchange_weak(tileIndex, tileIndex + 1, std::memory_order_relaxed))
            continue;
        const auto begin = tileIndex * mTile;
        try {
            mFunction(begin, begin + std::min(mTile, mRange - begin), workerIndex, mData);
        } catch (...) {
            std::terminate();
        }
        tileIndex = mNextTileIndex.load(std::memory_order_relaxed);
    }

    --gTileDepth;
}
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

namespace {
typedef au::flow::detail::NodePtr NodePtr;

NodePtr node(XFlowTaskHandle handle) { return handle ? handle->mNode : NodePtr(); }

std::unique_lock<std::mutex> submissionLock(au::flow::detail::Core& c)
{
    std::unique_lock<std::mutex> lock(c.mMutex);
    c.mChanged.wait(lock,
                    [&] { return !c.mReleasing || au::flow::detail::gCurrentTask || au::flow::detail::gTileDepth; });
    return lock;
}

}  // namespace

extern "C" {
XFlowTaskHandle au_flow_create_task(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy) noexcept
{
    try {
        auto payload = std::unique_ptr<void, au::flow::detail::PayloadDeleter>(data, {destroy, 0});
        if (!fn)
            return nullptr;
        auto owner               = std::make_shared<XFlowTaskNode>(fn);
        owner->mStorage.mPayload = std::move(payload);
        owner->mStorage.mName    = name ? name : "";
        // Establish the self-owning C pin only after all throwing initialization has succeeded.
        owner->mNode = NodePtr(owner, &owner->mStorage);
        return owner.get();
    } catch (...) {
        return nullptr;
    }
}

XFlowTaskHandle au_flow_create_task_ex(const char* name, size_t payloadSize, size_t payloadAlignment, XFlowTaskFn fn,
                                       XFlowDeleterFn destroyPayload, void** outPayload) noexcept
{
    if (outPayload)
        *outPayload = nullptr;
    if (!outPayload || !fn || !payloadSize || !payloadAlignment || (payloadAlignment & (payloadAlignment - 1)))
        return nullptr;
    try {
        auto  owner   = std::make_shared<XFlowTaskNode>(fn);
        auto& n       = owner->mStorage;
        n.mName       = name ? name : "";
        void* storage = owner->mInlinePayload;
        if (payloadSize > XFlowTaskNode::kInlinePayloadCapacity || payloadAlignment > alignof(std::max_align_t)) {
            if (payloadAlignment > alignof(std::max_align_t))
                storage = ::operator new(payloadSize, std::align_val_t(payloadAlignment));
            else
                storage = ::operator new(payloadSize);
            n.mPayload.get_deleter().mAlignment = payloadAlignment;
        }
        n.mPayload.reset(storage);
        n.mPendingDestroy = destroyPayload;                       // Destructor only; the runtime owns the storage.
        n.mPayloadReady.store(false, std::memory_order_relaxed);  // Not yet published to another owner.
        // Establish the self-owning C pin only after all throwing initialization has succeeded.
        owner->mNode = NodePtr(owner, &n);
        *outPayload  = storage;
        return owner.get();
    } catch (...) {
        return nullptr;
    }
}

int au_flow_commit_task_payload(XFlowTaskHandle handle) noexcept
{
    return au::flow::detail::boundary([&]() -> int {
        auto n = node(handle);
        if (!n)
            return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(n->mMutex);
        if (n->mState != au::flow::detail::TaskState::Created || n->mPayloadReady.load(std::memory_order_relaxed))
            return au::err::kErrorInvalidParam;
        n->mPayload.get_deleter().mDestroy = n->mPendingDestroy;
        n->mPayloadReady.store(true, std::memory_order_release);
        return 0;
    });
}

void au_flow_retain_task(XFlowTaskHandle handle) noexcept
{
    if (handle)
        handle->mRefs.fetch_add(1, std::memory_order_relaxed);
}

void au_flow_release_task(XFlowTaskHandle handle) noexcept
{
    // The alias owns the handle, Node and inline bytes together. Accepted work
    // may outlive the last C handle; only drop its C owning pin here.
    if (handle && handle->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto owner = std::move(handle->mNode);
    }
}

int au_flow_add_dependency(XFlowTaskHandle predecessorHandle, XFlowTaskHandle successorHandle) noexcept
{
    const auto pred = node(predecessorHandle);
    const auto succ = node(successorHandle);
    return au::flow::detail::boundary([&]() -> int {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.addDependency(pred, succ);
    });
}

int au_flow_enqueue_task(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto& c    = au::flow::detail::core();
        auto  lock = submissionLock(c);
        return c.submit(node(handle));
    });
}

int au_flow_wait_task(XFlowTaskHandle handle) noexcept
{
    return au::flow::detail::boundary([&]() -> int { return au::flow::detail::core().wait(node(handle)); });
}

int au_flow_get_task_result(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        std::lock_guard<std::mutex> lock(handle->mNode->mMutex);
        if (handle->mNode->mState == au::flow::detail::TaskState::Done)
            return handle->mNode->mResultCode;
        return handle->mNode->mBuildCode ? handle->mNode->mBuildCode : au::err::kErrorNotReady;
    });
}

int au_flow_task_done(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return 0;
    try {
        std::lock_guard<std::mutex> lock(handle->mNode->mMutex);
        return handle->mNode->mState == au::flow::detail::TaskState::Done;
    } catch (...) {
        return 0;
    }
}

int64_t au_flow_get_task_cost_us(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return 0;
    try {
        std::lock_guard<std::mutex> lock(handle->mNode->mMutex);
        return handle->mNode->mCostUs;
    } catch (...) {
        return 0;
    }
}

void au_flow_parallelize_tile_ex(size_t range, size_t tile, XFlowTileExFn fn, void* data) noexcept
{
    if (!range || !fn)
        return;
    tile = tile ? tile : 1;
    au::flow::detail::TileJob job;
    job.mRange     = range;
    job.mTile      = tile;
    job.mTileCount = range / tile + (range % tile != 0);
    job.mFunction  = fn;
    job.mData      = data;
    if (au::flow::detail::gTileDepth) {
        job.run(0);
        return;
    }

    au::flow::detail::Core* runtime;
    try {
        runtime = &au::flow::detail::core();
    } catch (...) {
        job.run(0);
        return;
    }

    auto& c    = *runtime;
    auto  lock = submissionLock(c);
    if (c.mClosing) {
        lock.unlock();
        job.run(0);
        return;
    }

    // Includes single-tile calls: a release barrier must not miss their body.
    ++c.mTileCallers;
    if (job.mTileCount > 1) {
        job.mNextJob      = c.mActiveTileJobs;
        c.mActiveTileJobs = &job;
        for (std::size_t i = 0; i < au::flow::detail::kMaxTileHelpers; ++i) {
            try {
                c.growWorkers();
            } catch (...) {
                // Helpers are optional; the caller still processes the remaining tiles.
                break;
            }
            c.mChanged.notify_one();
        }
    }

    lock.unlock();
    job.run(0);
    lock.lock();
    if (job.mTileCount > 1) {
        auto** link = &c.mActiveTileJobs;
        while (*link != &job)
            link = &(*link)->mNextJob;
        // Stop new helpers from joining, then wait before destroying this stack-allocated job.
        *link = job.mNextJob;
        c.mChanged.wait(lock, [&] { return job.mActiveHelpers.load(std::memory_order_relaxed) == 0; });
    }

    --c.mTileCallers;
    c.mChanged.notify_all();
}

int au_flow_wait_all() noexcept
{
    return au::flow::detail::boundary([]() -> int {
        if (au::flow::detail::gCurrentTask || au::flow::detail::gTileDepth)
            return au::err::kErrorFlowWouldDeadlock;
        auto&                        c = au::flow::detail::core();
        std::unique_lock<std::mutex> lock(c.mMutex);
        while (!c.mAccepted.empty() || c.mTileCallers)
            if (!c.helpOne(lock))
                c.mChanged.wait(lock);
        return 0;
    });
}

int au_flow_release_workers() noexcept
{
    // A synchronous task/tile cannot drain itself or join its own pool thread.
    if (au::flow::detail::gCurrentTask || au::flow::detail::gTileDepth || au::flow::detail::gWorkerIndex)
        return au::err::kErrorFlowWouldDeadlock;
    return au::flow::detail::boundary([]() -> int {
        auto&                                                  c = au::flow::detail::core();
        std::array<std::thread, au::flow::detail::kMaxWorkers> retired;
        std::unique_lock<std::mutex>                           lock(c.mMutex);
        c.mChanged.wait(lock, [&] { return !c.mReleasing; });
        if (c.mClosing)
            return au::err::kErrorStateInvalid;
        c.mReleasing = true;
        lock.unlock();
        const int error = au_flow_wait_all();
        lock.lock();
        if (error) {
            c.mReleasing = false;
            c.mChanged.notify_all();
            return error;
        }
        c.mStop = true;
        c.mChanged.notify_all();
        for (size_t i = 0; i < c.mThreads.size(); ++i)
            retired[i] = std::move(c.mThreads[i]);
        lock.unlock();
        for (auto& thread : retired)
            if (thread.joinable())
                thread.join();
        lock.lock();
        c.mWorkerCount     = 0;
        c.mIdleWorkerCount = 0;
        c.mStop            = false;
        c.mReleasing       = false;
        c.mChanged.notify_all();
        return 0;
    });
}

int au_flow_set_affinity_mask(uint64_t mask) noexcept
{
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        if (c.mAffinityInitCode)
            return c.mAffinityInitCode;
        if (mask & ~c.mInitialCpuMask)
            return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(c.mMutex);
        c.mRequestedCpuMask = mask;
        c.mAffinityVersion.fetch_add(1, std::memory_order_release);  // Workers acquire the accepted configuration.
        c.mChanged.notify_all();
        return 0;
    });
}

uint64_t au_flow_get_affinity_mask() noexcept
{
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.mRequestedCpuMask ? c.mRequestedCpuMask : c.mInitialCpuMask;
    } catch (...) {
        return 0;
    }
}

size_t au_flow_get_worker_count() noexcept
{
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.mWorkerCount;
    } catch (...) {
        return 0;
    }
}
}
