#include <algorithm>
#include <cstdio>
#include <exception>
#include <new>
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

bool insideTaskOrTile() noexcept { return gCurrentTask || gTileDepth; }

// Called only from an active catch, after scheduler/node locks have unwound.
int reportException(const char* context, au::log::Level level = au::log::Level::Error) noexcept
{
    try {
        throw;
    } catch (const std::bad_alloc& error) {
        XLOG_IMPL_LOC(level, "flow [%s]: %s\n", context, error.what());
        return au::err::kErrorNoMemory;
    } catch (const std::exception& error) {
        XLOG_IMPL_LOC(level, "flow [%s]: %s\n", context, error.what());
        return au::err::kErrorUnknown;
    } catch (...) {
        XLOG_IMPL_LOC(level, "flow [%s]: non-standard exception\n", context);
        return au::err::kErrorUnknown;
    }
}

// Execute synchronously; exceptions become logged error codes, not exceptions across the C ABI.
template <class F>
int boundary(const char* context, F fn) noexcept
{
    try {
        return fn();
    } catch (...) {
        return reportException(context);
    }
}

// Names are immutable after creation; callers keep the nodes alive while logging outside locks.
const char* taskName(const Node* task) noexcept
{
    return task ? (task->mName.empty() ? "unnamed task" : task->mName.c_str()) : "<null>";
}

// Reserve before changing links/counts; the eventual append must not allocate.
void reserveOne(std::vector<NodePtr>& values)
{
    const auto size = values.size();
    if (size < values.capacity())
        return;
    const auto limit = values.max_size();
    if (size == limit)
        throw std::bad_alloc();
    const auto growth = std::min(size, limit - size);
    values.reserve(size == 0 ? 1 : size + growth);
}

std::unique_lock<std::mutex> submissionLock(Core& runtime)
{
    std::unique_lock<std::mutex> lock(runtime.mMutex);
    runtime.mChanged.wait(lock, [&] { return !runtime.mReleasing || insideTaskOrTile(); });
    return lock;
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
                if (cpu < kAffinityMaskBits)
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
    auto recordBuildError = [&](int code) {
        std::lock_guard<std::mutex> lock(successor->mMutex);
        if (successor->mState == TaskState::Created && !successor->mBuildCode)
            successor->mBuildCode = code;
        return code;
    };

    if (predecessor == successor)
        return recordBuildError(au::err::kErrorFlowCycleDetected);
    if (!predecessor)
        return recordBuildError(au::err::kErrorInvalidParam);

    if (successor->mState != TaskState::Created)
        return au::err::kErrorStateInvalid;
    try {
        if (std::find(successor->mPredecessors.begin(), successor->mPredecessors.end(), predecessor) !=
            successor->mPredecessors.end())
            return au::err::kSuccess;

        // For a new edge, handle the predecessor by state:
        //
        // Predecessor state      Cycle check       Completion handling
        // Created                Search if needed  Register notification; increment pending count
        // Pending/Ready/Running  Skip              Register notification; increment pending count
        // Done                   Skip              Inherit result; no notification or pending count
        //
        // Accepted tasks have their whole upstream accepted and cannot gain new predecessors,
        // so they cannot depend on this Created successor.
        // A Created successor with no outgoing edges cannot lead back either.
        // Expired weak entries may cause an extra search, but cannot hide a cycle.
        // Check declared dependencies only; runtime mWaiting/mExecuting edges are checked
        // separately for wait deadlocks.
        if (predecessor->mState == TaskState::Created && !successor->mSuccessors.empty() &&
            reaches(predecessor.get(), successor.get()))
            return recordBuildError(au::err::kErrorFlowCycleDetected);
        reserveOne(successor->mPredecessors);

        // A Done predecessor will not complete again, so no successor notification or pending count is needed.
        // Inherit its existing result unless another dependency has already failed.
        if (predecessor->mState != TaskState::Done) {
            predecessor->mSuccessors.emplace_back(successor);
            ++successor->mPendingDependencyCount;
        } else if (!successor->mDependencyCode)
            successor->mDependencyCode = predecessor->mResultCode;

        // Every new dependency retains its predecessor, including completed ones.
        successor->mPredecessors.push_back(predecessor);
    } catch (...) {
        return recordBuildError(au::err::kErrorNoMemory);
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

int Core::submit(const NodePtr& root, NodePtr* failedTask)
{
    if (root->mState != TaskState::Created)
        return au::err::kSuccess;
    if (root->mBuildCode)
        return root->mBuildCode;
    // Acquire payload construction published by commit before accepting execution.
    if (!root->mPayloadReady.load(std::memory_order_acquire)) {
        if (failedTask)
            *failedTask = root;
        return au::err::kErrorNotReady;
    }
    std::list<NodePtr> batch{root};
    // Ordinary independent tasks need only an accepted-list entry, not graph containers.
    if (!root->mPredecessors.empty()) {
        std::vector<NodePtr> stack(root->mPredecessors);
        std::set<Node*>      collected{root.get()};
        while (!stack.empty()) {
            auto task = std::move(stack.back());
            stack.pop_back();
            if (task->mState != TaskState::Created || !collected.insert(task.get()).second)
                continue;
            if (task->mBuildCode)
                return task->mBuildCode;
            if (!task->mPayloadReady.load(std::memory_order_acquire)) {
                if (failedTask)
                    *failedTask = task;
                return au::err::kErrorNotReady;
            }
            batch.push_back(task);
            stack.insert(stack.end(), task->mPredecessors.begin(), task->mPredecessors.end());
        }
    }
    // During final shutdown, reject new external submissions but let running tasks
    // and tile callbacks enqueue the work they need to finish.
    if (mClosing && !insideTaskOrTile()) {
        if (failedTask)
            *failedTask = root;
        return au::err::kErrorStateInvalid;
    }
    growWorkers();  // No accepted state if the required thread could not start.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        auto&                       task = *it;
        std::lock_guard<std::mutex> lock(task->mMutex);
        task->mAcceptedPos = it;
        task->mState       = TaskState::Pending;
        if (!task->mPendingDependencyCount)
            enqueueReady(task.get());
    }

    // splice transfers the existing list nodes without invalidating their iterators,
    // so each task's mAcceptedPos remains valid in mAccepted and needs no update.
    mAccepted.splice(mAccepted.end(), batch);
    mChanged.notify_all();
    return au::err::kSuccess;
}

TileJob* Core::findTileJobToHelp()
{
    for (auto* j = mActiveTileJobs; j; j = j->mNextJob)
        if (j->mActiveHelpers < kMaxTileHelpers && j->mNextTileIndex.load(std::memory_order_relaxed) < j->mTileCount)
            return j;
    return nullptr;
}

void Core::applyAffinity(std::unique_lock<std::mutex>& lock)
{
    for (;;) {
        const auto version = mAffinityVersion;
        // Skip non-worker threads, configurations already attempted by this worker,
        // or an unavailable initial CPU set. Retry failed bindings only after reconfiguration.
        if (!gWorkerIndex || gAttemptedAffinityVersion == version || mInitialCpus.empty())
            break;
        const auto mask           = mRequestedCpuMask;
        const bool useInitialCpus = !mask;
        lock.unlock();
        std::array<std::uint32_t, kAffinityMaskBits> selectedCpus;
        std::size_t                                  cpuCount = 0;
        if (!useInitialCpus)
            for (unsigned i = 0; i < kAffinityMaskBits; ++i)
                if (mask & (UINT64_C(1) << i))
                    selectedCpus[cpuCount++] = i;
        const auto* cpus  = useInitialCpus ? mInitialCpus.data() : selectedCpus.data();
        const auto  count = useInitialCpus ? mInitialCpus.size() : cpuCount;
        const int   error = au::sys::setThreadAffinity(cpus, count);
        if (error)
            XLOG_W("flow worker %zu: affinity failed, requested_mask=0x%llx, version=%llu, code=%d\n", gWorkerIndex,
                   static_cast<unsigned long long>(mask), static_cast<unsigned long long>(version), error);
        // One attempt per configuration, outside scheduler locks. Reconfigure to retry.
        gAttemptedAffinityVersion = version;
        lock.lock();
        if (mAffinityVersion == version)
            break;
    }
}

// Run outside scheduler locks, keeping the current-task context through payload cleanup.
int runTask(Node& task)
{
    int                 code = task.mDependencyCode;
    au::perf::PerfScope trace{};
    const bool          traced = !code && !task.mName.empty();
    if (traced)
        au::perf::traceBegin(&trace, task.mName);
    if (!code)
        code = boundary(taskName(&task), [&]() -> int { return task.mFunction(task.mPayload.get()); });
    if (traced)
        au::perf::traceEnd(&trace);
    task.mPayload.reset();
    return code;
}

bool Core::helpOne(std::unique_lock<std::mutex>& lock)
{
    if (mStop)
        return false;
    applyAffinity(lock);

    if (auto* job = gWorkerIndex ? findTileJobToHelp() : nullptr) {
        // Register under mMutex so the caller cannot destroy the job before this helper exits.
        ++job->mActiveHelpers;
        lock.unlock();
        job->run(gWorkerIndex);
        lock.lock();
        --job->mActiveHelpers;
        mChanged.notify_all();
        return true;
    }

    if (!mReadyHead || gTaskDepth >= kMaxTaskDepth)
        return false;
    // Take one ready task and record nested execution while holding the scheduler lock.
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

    // Execute and clean up the payload outside the lock, before restoring task context.
    lock.unlock();
    gCurrentTask = task.get();
    ++gTaskDepth;
    const int code = runTask(*task);
    --gTaskDepth;
    gCurrentTask = previousTask;
    lock.lock();
    if (previousTask)
        previousTask->mExecuting = nullptr;
    // Publish completion and detach links without allocating; release owners outside the lock.
    std::vector<std::weak_ptr<Node>> successors;
    std::vector<NodePtr>             predecessors;
    {
        std::lock_guard<std::mutex> nodeLock(task->mMutex);
        task->mResultCode = code;
        task->mState      = TaskState::Done;
        successors.swap(task->mSuccessors);
        // Completed predecessors no longer participate in scheduling or wait cycles.
        predecessors.swap(task->mPredecessors);
    }

    for (auto& weakSuccessor : successors) {
        auto successor = weakSuccessor.lock();
        if (!successor)
            continue;
        {
            std::lock_guard<std::mutex> successorLock(successor->mMutex);
            if (code && !successor->mDependencyCode)
                successor->mDependencyCode = code;
            --successor->mPendingDependencyCount;
            if (successor->mPendingDependencyCount == 0 && successor->mState == TaskState::Pending)
                enqueueReady(successor.get());
        }
        lock.unlock();
        // This temporary owner may be the last one; payload cleanup must run outside mMutex.
        successor.reset();
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
        const auto observedAffinityVersion = mAffinityVersion;
        if (helpOne(lock))
            continue;
        ++mIdleWorkerCount;
        mChanged.wait(lock, [&] {
            return mStop || mReadyHead || findTileJobToHelp() || mAffinityVersion != observedAffinityVersion;
        });
        --mIdleWorkerCount;
    }
}

int Core::wait(const NodePtr& task)
{
    if (!task) {
        XLOG_E("flow wait: null task\n");
        return au::err::kErrorInvalidParam;
    }
    std::unique_lock<std::mutex> lock(mMutex);
    if (task->mState == TaskState::Done)
        return task->mResultCode;
    if (task->mState == TaskState::Created) {
        if (task->mBuildCode)
            return task->mBuildCode;  // The originating graph operation already reported it.
        lock.unlock();
        XLOG_E("flow wait: task not submitted, target=\"%s\" (%p)\n", taskName(task.get()),
               static_cast<void*>(task.get()));
        return au::err::kErrorStateInvalid;
    }
    const char* reason = nullptr;
    if (gTileDepth)
        reason = "wait from tile callback";
    else if (gTaskDepth >= kMaxTaskDepth)
        reason = "task nesting limit";
    else if (gCurrentTask && reaches(task.get(), gCurrentTask, true))
        reason = "dependency/runtime wait cycle";
    if (reason) {
        lock.unlock();
        XLOG_E("flow wait: %s, current=\"%s\" (%p), target=\"%s\" (%p)\n", reason, taskName(gCurrentTask),
               static_cast<void*>(gCurrentTask), taskName(task.get()), static_cast<void*>(task.get()));
        return au::err::kErrorFlowWouldDeadlock;
    }
    Node*  externalWaitTarget = nullptr;
    Node*& waitTarget         = gCurrentTask ? gCurrentTask->mWaiting : externalWaitTarget;

    struct RestoreWaitTarget
    {
        Node*& mSlot;
        Node*  mOld;

        ~RestoreWaitTarget() { mSlot = mOld; }
    } restore{waitTarget, waitTarget};

    waitTarget = task.get();
    while (task->mState != TaskState::Done)
        if (!helpOne(lock))
            mChanged.wait(lock);
    return task->mResultCode;
}

void Core::runTiles(TileJob& job)
{
    auto lock = submissionLock(*this);
    if (mClosing) {
        lock.unlock();
        job.run(0);
        return;
    }

    // Includes single-tile calls: a release barrier must not miss their body.
    ++mTileCallers;
    if (job.mTileCount > 1) {
        job.mNextJob    = mActiveTileJobs;
        mActiveTileJobs = &job;
        for (std::size_t i = 0; i < kMaxTileHelpers; ++i) {
            try {
                growWorkers();
            } catch (...) {
                // Helpers are optional; the caller still processes the remaining tiles.
                break;
            }
            mChanged.notify_one();
        }
    }

    lock.unlock();
    job.run(0);
    lock.lock();
    if (job.mTileCount > 1) {
        auto** link = &mActiveTileJobs;
        while (*link != &job)
            link = &(*link)->mNextJob;
        // Stop new helpers from joining, then wait before destroying this stack-allocated job.
        *link = job.mNextJob;
        mChanged.wait(lock, [&] { return job.mActiveHelpers == 0; });
    }

    --mTileCallers;
    mChanged.notify_all();
}

int Core::waitAll()
{
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mAccepted.empty() || mTileCallers)
        if (!helpOne(lock))
            mChanged.wait(lock);
    return au::err::kSuccess;
}

int Core::releaseWorkers()
{
    std::array<std::thread, kMaxWorkers> retired;
    std::unique_lock<std::mutex>         lock(mMutex);
    mChanged.wait(lock, [&] { return !mReleasing; });
    if (mClosing)
        return au::err::kErrorStateInvalid;
    mReleasing = true;
    lock.unlock();
    const int error = boundary(__func__, [&]() -> int { return waitAll(); });
    lock.lock();
    if (error) {
        mReleasing = false;
        mChanged.notify_all();
        return error;
    }
    mStop = true;
    mChanged.notify_all();
    retired.swap(mThreads);
    lock.unlock();
    for (auto& thread : retired)
        if (thread.joinable())
            thread.join();
    lock.lock();
    mWorkerCount = 0;
    mStop        = false;
    mReleasing   = false;
    mChanged.notify_all();
    return au::err::kSuccess;
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
            char context[192];
            std::snprintf(context, sizeof(context), "parallel_for callback: range=%zu, tile=%zu, index=%zu, worker=%zu",
                          mRange, mTile, tileIndex, workerIndex);
            (void)reportException(context);
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

}  // namespace

extern "C" {
XFlowTaskHandle au_flow_create_task(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy) noexcept
{
    try {
        auto payload = std::unique_ptr<void, au::flow::detail::PayloadDeleter>(data, {destroy, 0});
        if (!fn) {
            XLOG_E("flow create_task: null function\n");
            return nullptr;
        }
        auto owner               = std::make_shared<XFlowTaskNode>(fn);
        owner->mStorage.mPayload = std::move(payload);
        owner->mStorage.mName    = name ? name : "";
        // Establish the self-owning C pin only after all throwing initialization has succeeded.
        owner->mNode = NodePtr(owner, &owner->mStorage);
        return owner.get();
    } catch (...) {
        (void)au::flow::detail::reportException(__func__);
        return nullptr;
    }
}

XFlowTaskHandle au_flow_create_task_ex(const char* name, size_t payloadSize, size_t payloadAlignment, XFlowTaskFn fn,
                                       XFlowDeleterFn destroyPayload, void** outPayload) noexcept
{
    if (outPayload)
        *outPayload = nullptr;
    const bool validAlignment = payloadAlignment != 0 && (payloadAlignment & (payloadAlignment - 1)) == 0;
    if (!outPayload || !fn || !payloadSize || !validAlignment) {
        XLOG_E("flow create_task_ex: invalid arguments, out=%p, function=%s, size=%zu, alignment=%zu\n",
               static_cast<void*>(outPayload), fn ? "set" : "null", payloadSize, payloadAlignment);
        return nullptr;
    }
    try {
        auto  owner        = std::make_shared<XFlowTaskNode>(fn);
        auto& task         = owner->mStorage;
        task.mName         = name ? name : "";
        void*      storage = owner->mInlinePayload;
        const bool needsFallback =
            payloadSize > XFlowTaskNode::kInlinePayloadCapacity || payloadAlignment > alignof(std::max_align_t);
        if (needsFallback) {
            if (payloadAlignment > alignof(std::max_align_t))
                storage = ::operator new(payloadSize, std::align_val_t(payloadAlignment));
            else
                storage = ::operator new(payloadSize);
            task.mPayload.get_deleter().mAlignment = payloadAlignment;
        }
        task.mPayload.reset(storage);
        task.mPendingDestroy = destroyPayload;                       // Destructor only; the runtime owns the storage.
        task.mPayloadReady.store(false, std::memory_order_relaxed);  // Not yet published to another owner.
        // Establish the self-owning C pin only after all throwing initialization has succeeded.
        owner->mNode = NodePtr(owner, &task);
        *outPayload  = storage;
        return owner.get();
    } catch (...) {
        (void)au::flow::detail::reportException(__func__);
        return nullptr;
    }
}

int au_flow_commit_task_payload(XFlowTaskHandle handle) noexcept
{
    return au::flow::detail::boundary(__func__, [&]() -> int {
        auto task = node(handle);
        if (!task) {
            XLOG_E("flow commit_task_payload: null task\n");
            return au::err::kErrorInvalidParam;
        }
        {
            std::lock_guard<std::mutex> lock(task->mMutex);
            if (task->mState == au::flow::detail::TaskState::Created &&
                !task->mPayloadReady.load(std::memory_order_relaxed)) {
                task->mPayload.get_deleter().mDestroy = task->mPendingDestroy;
                task->mPayloadReady.store(true, std::memory_order_release);
                return au::err::kSuccess;
            }
        }

        XLOG_E("flow commit_task_payload: task not Created or already committed, task=\"%s\" (%p)\n",
               au::flow::detail::taskName(task.get()), static_cast<void*>(task.get()));
        return au::err::kErrorInvalidParam;
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
    return au::flow::detail::boundary(__func__, [&]() -> int {
        auto& runtime = au::flow::detail::core();
        int   code;
        bool  report;
        {
            std::lock_guard<std::mutex> lock(runtime.mMutex);
            const bool                  alreadyFailed = succ && succ->mBuildCode;
            code                                      = runtime.addDependency(pred, succ);
            report                                    = code && !alreadyFailed;
        }

        if (report)
            XLOG_E("flow add_dependency: %s (%d), predecessor=\"%s\" (%p), successor=\"%s\" (%p)\n",
                   au::err::getErrorStr(code), code, au::flow::detail::taskName(pred.get()),
                   static_cast<void*>(pred.get()), au::flow::detail::taskName(succ.get()),
                   static_cast<void*>(succ.get()));
        return code;
    });
}

int au_flow_enqueue_task(XFlowTaskHandle handle) noexcept
{
    if (!handle) {
        XLOG_E("flow enqueue_task: null task\n");
        return au::err::kErrorInvalidParam;
    }
    return au::flow::detail::boundary(__func__, [&]() -> int {
        auto&   runtime = au::flow::detail::core();
        NodePtr failedTask;
        int     code;
        {
            auto lock = au::flow::detail::submissionLock(runtime);
            code      = runtime.submit(node(handle), &failedTask);
        }

        if (failedTask)
            XLOG_E("flow enqueue_task: %s, task=\"%s\" (%p), code=%d\n",
                   code == au::err::kErrorNotReady ? "payload not committed" : "runtime closing",
                   au::flow::detail::taskName(failedTask.get()), static_cast<void*>(failedTask.get()), code);
        return code;
    });
}

int au_flow_wait_task(XFlowTaskHandle handle) noexcept
{
    return au::flow::detail::boundary(__func__, [&]() -> int { return au::flow::detail::core().wait(node(handle)); });
}

int au_flow_get_task_result(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary(__func__, [&]() -> int {
        auto&                       task = handle->mStorage;
        std::lock_guard<std::mutex> lock(task.mMutex);
        if (task.mState == au::flow::detail::TaskState::Done)
            return task.mResultCode;
        return task.mBuildCode ? task.mBuildCode : au::err::kErrorNotReady;
    });
}

int au_flow_task_done(XFlowTaskHandle handle) noexcept
{
    if (!handle)
        return 0;
    try {
        auto&                       task = handle->mStorage;
        std::lock_guard<std::mutex> lock(task.mMutex);
        return task.mState == au::flow::detail::TaskState::Done;
    } catch (...) {
        (void)au::flow::detail::reportException(__func__);
        return 0;
    }
}

void au_flow_parallel_for(size_t range, size_t tile, XFlowTileFn fn, void* data) noexcept
{
    if (!range)
        return;
    if (!fn) {
        XLOG_E("flow parallel_for: null callback, range=%zu, tile=%zu\n", range, tile);
        return;
    }
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
        (void)au::flow::detail::reportException("parallel_for initialization; running serially", au::log::Level::Warn);
        job.run(0);
        return;
    }

    runtime->runTiles(job);
}

int au_flow_wait_all() noexcept
{
    return au::flow::detail::boundary(__func__, []() -> int {
        if (au::flow::detail::insideTaskOrTile()) {
            XLOG_E("flow wait_all: cannot drain from a task or tile callback\n");
            return au::err::kErrorFlowWouldDeadlock;
        }
        return au::flow::detail::core().waitAll();
    });
}

int au_flow_release_workers() noexcept
{
    // Tasks/tiles cannot drain themselves. A worker may also run a payload deleter outside
    // task context; it must not join its own thread then either.
    if (au::flow::detail::insideTaskOrTile() || au::flow::detail::gWorkerIndex) {
        XLOG_E("flow release_workers: cannot drain from a task/tile or join the calling worker, worker=%zu\n",
               au::flow::detail::gWorkerIndex);
        return au::err::kErrorFlowWouldDeadlock;
    }
    return au::flow::detail::boundary(__func__, []() -> int { return au::flow::detail::core().releaseWorkers(); });
}

int au_flow_set_affinity_mask(uint64_t mask) noexcept
{
    return au::flow::detail::boundary(__func__, [&]() -> int {
        auto& runtime = au::flow::detail::core();
        if (runtime.mAffinityInitCode)
            return runtime.mAffinityInitCode;
        if (mask & ~runtime.mInitialCpuMask)
            return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(runtime.mMutex);
        runtime.mRequestedCpuMask = mask;
        ++runtime.mAffinityVersion;
        runtime.mChanged.notify_all();
        return au::err::kSuccess;
    });
}

uint64_t au_flow_get_affinity_mask() noexcept
{
    try {
        auto&                       runtime = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(runtime.mMutex);
        return runtime.mRequestedCpuMask ? runtime.mRequestedCpuMask : runtime.mInitialCpuMask;
    } catch (...) {
        (void)au::flow::detail::reportException(__func__);
        return 0;
    }
}

size_t au_flow_get_worker_count() noexcept
{
    try {
        auto&                       runtime = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(runtime.mMutex);
        return runtime.mWorkerCount;
    } catch (...) {
        (void)au::flow::detail::reportException(__func__);
        return 0;
    }
}
}
