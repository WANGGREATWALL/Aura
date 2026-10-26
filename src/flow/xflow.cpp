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
thread_local Node*         gCurrent     = nullptr;
thread_local std::size_t   gWorkerIndex = 0;
thread_local unsigned      gTileDepth = 0, gDepth = 0;
thread_local std::uint64_t gAffinity = 0;

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
    mAffinityStatus   = au::sys::getThreadAffinity(nullptr, 0, count);
    if (!mAffinityStatus) {
        mDefaultCpus.resize(count);
        mAffinityStatus = au::sys::getThreadAffinity(mDefaultCpus.data(), mDefaultCpus.size(), count);
        if (mAffinityStatus)
            mDefaultCpus.clear();
        else {
            mDefaultCpus.resize(count);
            for (auto cpu : mDefaultCpus)
                if (cpu < 64)
                    mDefaultMask |= UINT64_C(1) << cpu;
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

bool Core::reaches(Node* from, Node* target, bool waits)
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
        for (auto& pred : n->mPreds)
            stack.push_back(pred.get());
        if (waits) {
            stack.push_back(n->mWaiting);
            stack.push_back(n->mExecuting);
        }
    }

    return false;
}

int Core::edge(const NodePtr& pred, const NodePtr& succ)
{
    if (!succ)
        return au::err::kErrorInvalidParam;
    // Core::mMutex already protects links and predecessor state; only succ's build error needs its query lock.
    std::lock_guard<std::mutex> lock(succ->mMutex);
    auto                        error = [&](int code) {
        if (succ->mState == State::Created && !succ->mBuild)
            succ->mBuild = code;
        return code;
    };

    if (pred == succ)
        return error(au::err::kErrorFlowCycleDetected);
    if (!pred)
        return error(au::err::kErrorInvalidParam);

    if (succ->mState != State::Created)
        return au::err::kErrorStateInvalid;
    try {
        if (std::find(succ->mPreds.begin(), succ->mPreds.end(), pred) != succ->mPreds.end())
            return 0;
        // Closure submission accepts every upstream node, and accepted targets reject new edges.
        // Thus an accepted pred cannot depend on a Created succ. If that rule changes, revisit this guard.
        // A Created succ has never finished, so all of its outgoing edges are in mSuccessors;
        // an empty list rules out a return path. Expired weak entries merely keep DFS conservative.
        if (pred->mState == State::Created && !succ->mSuccessors.empty() && reaches(pred.get(), succ.get()))
            return error(au::err::kErrorFlowCycleDetected);
        reserveOne(succ->mPreds);
        if (pred->mState != State::Done) {
            pred->mSuccessors.emplace_back(succ);
            ++succ->mPending;
        } else if (!succ->mDependencyError)
            succ->mDependencyError = pred->mCode;
        succ->mPreds.push_back(pred);
    } catch (...) {
        return error(au::err::kErrorNoMemory);
    }

    return 0;
}

void Core::ready(Node* node)
{
    node->mState = State::Ready;
    if (mTail)
        mTail->mNext = node;
    else
        mHead = node;
    mTail = node;
}

void Core::grow()
{
    if (!mIdle && mCount < mThreads.size()) {
        const auto index = mCount + 1;
        mThreads[mCount] = std::thread([this, index] { worker(index); });
        ++mCount;
    }
}

int Core::submit(const NodePtr& root)
{
    if (root->mState != State::Created)
        return 0;
    if (root->mBuild)
        return root->mBuild;
    // Acquire placement construction before publishing callable execution.
    if (!root->mPayloadReady.load(std::memory_order_acquire))
        return au::err::kErrorNotReady;
    std::list<NodePtr> batch{root};
    // Ordinary independent tasks need only an accepted-list entry, not graph containers.
    if (!root->mPreds.empty()) {
        std::vector<NodePtr> stack(root->mPreds);
        std::set<Node*>      collected{root.get()};
        while (!stack.empty()) {
            auto n = std::move(stack.back());
            stack.pop_back();
            if (n->mState != State::Created || !collected.insert(n.get()).second)
                continue;
            if (n->mBuild)
                return n->mBuild;
            if (!n->mPayloadReady.load(std::memory_order_acquire))
                return au::err::kErrorNotReady;
            batch.push_back(n);
            stack.insert(stack.end(), n->mPreds.begin(), n->mPreds.end());
        }
    }
    if (mClosing && !gCurrent && !gTileDepth)
        return au::err::kErrorStateInvalid;
    grow();  // No accepted state if the required thread could not start.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        auto&                       n = *it;
        std::lock_guard<std::mutex> lock(n->mMutex);
        n->mAccepted = it;
        n->mState    = State::Pending;
        if (!n->mPending)
            ready(n.get());
    }

    mAccepted.splice(mAccepted.end(), batch);
    mChanged.notify_all();
    return 0;
}

TileJob* Core::tile()
{
    for (auto* j = mTiles; j; j = j->mLink)
        if (j->mHelpers.load(std::memory_order_relaxed) < kMaxTileHelpers &&
            j->mNext.load(std::memory_order_relaxed) < j->mTotal)
            return j;
    return nullptr;
}

void Core::applyAffinity(std::unique_lock<std::mutex>& lock)
{
    for (;;) {
        const auto generation = mAffinityGen.load(std::memory_order_acquire);
        if (!gWorkerIndex || gAffinity == generation || mDefaultCpus.empty())
            break;
        const auto mask     = mRequestedMask;
        const bool defaults = !mask;
        lock.unlock();
        std::array<std::uint32_t, 64> selected;
        std::size_t                   count = 0;
        if (!defaults)
            for (unsigned i = 0; i < 64; ++i)
                if (mask & (UINT64_C(1) << i))
                    selected[count++] = i;
        const int error = au::sys::setThreadAffinity(defaults ? mDefaultCpus.data() : selected.data(),
                                                     defaults ? mDefaultCpus.size() : count);
        if (error)
            XLOG_W("flow worker %zu: affinity application failed (%d)", gWorkerIndex, error);
        // One attempt per configuration, outside scheduler locks. Reconfigure to retry.
        gAffinity = generation;
        lock.lock();
        if (mAffinityGen.load(std::memory_order_acquire) == generation)
            break;
    }
}

bool Core::step(std::unique_lock<std::mutex>& lock)
{
    if (mStop)
        return false;
    applyAffinity(lock);

    if (auto* job = gWorkerIndex ? tile() : nullptr) {
        job->mHelpers.fetch_add(1, std::memory_order_relaxed);  // Job lifetime is also pinned by mMutex.
        lock.unlock();
        job->run(gWorkerIndex);
        lock.lock();
        job->mHelpers.fetch_sub(1, std::memory_order_relaxed);
        mChanged.notify_all();
        return true;
    }

    if (!mHead || gDepth >= kMaxWaitDepth)
        return false;
    NodePtr n = *mHead->mAccepted;
    mHead     = n->mNext;
    if (!mHead)
        mTail = nullptr;
    n->mNext = nullptr;
    {
        std::lock_guard<std::mutex> state(n->mMutex);
        n->mState = State::Running;
    }

    Node* parent = gCurrent;
    if (parent)
        parent->mExecuting = n.get();
    if (mHead) {
        try {
            grow();
        } catch (...) {
        }
    }

    lock.unlock();
    gCurrent = n.get();
    ++gDepth;
    int                 code  = n->mDependencyError;
    const auto          start = std::chrono::steady_clock::now();
    au::perf::PerfScope trace{};
    const bool          traced = !code && !n->mName.empty();
    if (traced)
        au::perf::traceBegin(&trace, n->mName);
    if (!code)
        code = boundary([&]() -> int { return n->mFunction(n->mPayload.get()); },
                        n->mName.empty() ? "unnamed task" : n->mName.c_str());
    if (traced)
        au::perf::traceEnd(&trace);
    const auto cost =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    n->mPayload.reset();
    --gDepth;
    gCurrent = parent;
    lock.lock();
    if (parent)
        parent->mExecuting = nullptr;
    std::vector<std::weak_ptr<Node>> successors;
    std::vector<NodePtr>             predecessors;
    {
        std::lock_guard<std::mutex> state(n->mMutex);
        n->mCode  = code;
        n->mCost  = cost;
        n->mState = State::Done;
        successors.swap(n->mSuccessors);
        predecessors.swap(n->mPreds);  // Completed dependencies no longer participate in scheduling or wait cycles.
    }

    for (auto& weak : successors)
        if (auto succ = weak.lock()) {
            {
                std::lock_guard<std::mutex> state(succ->mMutex);
                if (code && !succ->mDependencyError)
                    succ->mDependencyError = code;
                if (!--succ->mPending && succ->mState == State::Pending)
                    ready(succ.get());
            }
            lock.unlock();
            succ.reset();
            lock.lock();
        }
    mAccepted.erase(n->mAccepted);
    mChanged.notify_all();
    lock.unlock();
    predecessors.clear();
    n.reset();
    lock.lock();  // Last owners/deleters never run under scheduler locks.
    return true;
}

void Core::worker(std::size_t index)
{
    gWorkerIndex = index;
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStop) {
        const auto generation = mAffinityGen.load(std::memory_order_acquire);
        if (step(lock))
            continue;
        ++mIdle;
        mChanged.wait(lock, [&] {
            return mStop || mHead || tile() || mAffinityGen.load(std::memory_order_acquire) != generation;
        });
        --mIdle;
    }
}

int Core::wait(const NodePtr& n)
{
    if (!n)
        return au::err::kErrorInvalidParam;
    std::unique_lock<std::mutex> lock(mMutex);
    if (n->mState == State::Done)
        return n->mCode;
    if (n->mState == State::Created)
        return n->mBuild ? n->mBuild : au::err::kErrorStateInvalid;
    if (gTileDepth || gDepth >= kMaxWaitDepth || (gCurrent && reaches(n.get(), gCurrent, true)))
        return au::err::kErrorFlowWouldDeadlock;
    Node*  unused  = nullptr;
    Node*& waiting = gCurrent ? gCurrent->mWaiting : unused;

    struct Restore
    {
        Node*& mSlot;
        Node*  mOld;

        ~Restore() { mSlot = mOld; }
    } restore{waiting, waiting};

    waiting = n.get();
    while (n->mState != State::Done)
        if (!step(lock))
            mChanged.wait(lock);
    return n->mCode;
}

void TileJob::run(std::size_t worker) noexcept
{
    ++gTileDepth;
    std::size_t index = mNext.load(std::memory_order_relaxed);
    while (index < mTotal) {
        if (!mNext.compare_exchange_weak(index, index + 1, std::memory_order_relaxed))
            continue;
        const auto begin = index * mTile;
        try {
            mFunction(begin, begin + std::min(mTile, mRange - begin), worker, mData);
        } catch (...) {
            std::terminate();
        }
        index = mNext.load(std::memory_order_relaxed);
    }

    --gTileDepth;
}
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

namespace {
typedef au::flow::detail::NodePtr NodePtr;

NodePtr node(XFlowTaskHandle h) { return h ? h->mNode : NodePtr(); }

std::unique_lock<std::mutex> submissionLock(au::flow::detail::Core& c)
{
    std::unique_lock<std::mutex> lock(c.mMutex);
    c.mChanged.wait(lock, [&] { return !c.mReleasing || au::flow::detail::gCurrent || au::flow::detail::gTileDepth; });
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
        owner->mNode             = NodePtr(owner, &owner->mStorage);
        return owner.get();
    } catch (...) {
        return nullptr;
    }
}

XFlowTaskHandle au_flow_create_task_ex(const char* name, size_t size, size_t alignment, XFlowTaskFn fn,
                                       XFlowDeleterFn destroy, void** out) noexcept
{
    if (out)
        *out = nullptr;
    if (!out || !fn || !size || !alignment || (alignment & (alignment - 1)))
        return nullptr;
    try {
        auto  owner   = std::make_shared<XFlowTaskNode>(fn);
        auto& n       = owner->mStorage;
        n.mName       = name ? name : "";
        void* storage = owner->mInlinePayload;
        if (size > XFlowTaskNode::kInlinePayloadCapacity || alignment > alignof(std::max_align_t)) {
            if (alignment > alignof(std::max_align_t))
                storage = ::operator new(size, std::align_val_t(alignment));
            else
                storage = ::operator new(size);
            n.mPayload.get_deleter().mAlignment = alignment;
        }
        n.mPayload.reset(storage);
        n.mPendingDestroy = destroy;
        n.mPayloadReady.store(false, std::memory_order_relaxed);  // Not yet published to another owner.
        owner->mNode = NodePtr(owner, &n);
        *out         = storage;
        return owner.get();
    } catch (...) {
        return nullptr;
    }
}

int au_flow_commit_task_payload(XFlowTaskHandle h) noexcept
{
    return au::flow::detail::boundary([&]() -> int {
        auto n = node(h);
        if (!n)
            return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(n->mMutex);
        if (n->mState != au::flow::detail::State::Created || n->mPayloadReady.load(std::memory_order_relaxed))
            return au::err::kErrorInvalidParam;
        n->mPayload.get_deleter().mDestroy = n->mPendingDestroy;
        n->mPayloadReady.store(true, std::memory_order_release);
        return 0;
    });
}

void au_flow_retain_task(XFlowTaskHandle h) noexcept
{
    if (h)
        h->mRefs.fetch_add(1, std::memory_order_relaxed);
}

void au_flow_release_task(XFlowTaskHandle h) noexcept
{
    // The alias owns the handle, Node and inline bytes together. Accepted work
    // may outlive the last C handle; only drop its C owning pin here.
    if (h && h->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto owner = std::move(h->mNode);
    }
}

int au_flow_add_dependency(XFlowTaskHandle a, XFlowTaskHandle b) noexcept
{
    const auto pred = node(a), succ = node(b);
    return au::flow::detail::boundary([&]() -> int {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.edge(pred, succ);
    });
}

int au_flow_enqueue_task(XFlowTaskHandle h) noexcept
{
    if (!h)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        auto  l = submissionLock(c);
        return c.submit(node(h));
    });
}

int au_flow_wait_task(XFlowTaskHandle h) noexcept
{
    return au::flow::detail::boundary([&]() -> int { return au::flow::detail::core().wait(node(h)); });
}

int au_flow_get_task_result(XFlowTaskHandle h) noexcept
{
    if (!h)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        std::lock_guard<std::mutex> lock(h->mNode->mMutex);
        if (h->mNode->mState == au::flow::detail::State::Done)
            return h->mNode->mCode;
        return h->mNode->mBuild ? h->mNode->mBuild : au::err::kErrorNotReady;
    });
}

int au_flow_task_done(XFlowTaskHandle h) noexcept
{
    if (!h)
        return 0;
    try {
        std::lock_guard<std::mutex> lock(h->mNode->mMutex);
        return h->mNode->mState == au::flow::detail::State::Done;
    } catch (...) {
        return 0;
    }
}

int64_t au_flow_get_task_cost_us(XFlowTaskHandle h) noexcept
{
    if (!h)
        return 0;
    try {
        std::lock_guard<std::mutex> lock(h->mNode->mMutex);
        return h->mNode->mCost;
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
    job.mRange    = range;
    job.mTile     = tile;
    job.mTotal    = range / tile + (range % tile != 0);
    job.mFunction = fn;
    job.mData     = data;
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
    if (job.mTotal > 1) {
        job.mLink = c.mTiles;
        c.mTiles  = &job;
        for (std::size_t i = 0; i < au::flow::detail::kMaxTileHelpers; ++i) {
            try {
                c.grow();
            } catch (...) {
                break;
            }
            c.mChanged.notify_one();
        }
    }

    lock.unlock();
    job.run(0);
    lock.lock();
    if (job.mTotal > 1) {
        auto** link = &c.mTiles;
        while (*link != &job)
            link = &(*link)->mLink;
        *link = job.mLink;
        c.mChanged.wait(lock, [&] { return job.mHelpers.load(std::memory_order_relaxed) == 0; });
    }

    --c.mTileCallers;
    c.mChanged.notify_all();
}

int au_flow_wait_all() noexcept
{
    return au::flow::detail::boundary([]() -> int {
        if (au::flow::detail::gCurrent || au::flow::detail::gTileDepth)
            return au::err::kErrorFlowWouldDeadlock;
        auto&                        c = au::flow::detail::core();
        std::unique_lock<std::mutex> lock(c.mMutex);
        while (!c.mAccepted.empty() || c.mTileCallers)
            if (!c.step(lock))
                c.mChanged.wait(lock);
        return 0;
    });
}

int au_flow_release_workers() noexcept
{
    // A synchronous task/tile cannot drain itself or join its own pool thread.
    if (au::flow::detail::gCurrent || au::flow::detail::gTileDepth || au::flow::detail::gWorkerIndex)
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
        c.mCount     = 0;
        c.mIdle      = 0;
        c.mStop      = false;
        c.mReleasing = false;
        c.mChanged.notify_all();
        return 0;
    });
}

int au_flow_set_affinity_mask(uint64_t mask) noexcept
{
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        if (c.mAffinityStatus)
            return c.mAffinityStatus;
        if (mask & ~c.mDefaultMask)
            return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(c.mMutex);
        c.mRequestedMask = mask;
        c.mAffinityGen.fetch_add(1, std::memory_order_release);  // Workers acquire the accepted configuration.
        c.mChanged.notify_all();
        return 0;
    });
}

uint64_t au_flow_get_affinity_mask() noexcept
{
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.mRequestedMask ? c.mRequestedMask : c.mDefaultMask;
    } catch (...) {
        return 0;
    }
}

size_t au_flow_get_worker_count() noexcept
{
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.mCount;
    } catch (...) {
        return 0;
    }
}
}
