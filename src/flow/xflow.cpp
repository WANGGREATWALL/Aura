#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "flow/xflow_core.h"
#include "perf/xtimer.h"
#include "perf/xtracer.h"
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif
namespace au {
namespace flow AU_LOCAL {
namespace detail {
thread_local Node*         gCurrent     = nullptr;
thread_local std::size_t   gWorkerIndex = 0;
thread_local unsigned      gTileDepth = 0, gDepth = 0;
thread_local std::uint64_t gAffinity = 0;

template <class F>
int boundary(F fn) noexcept
{
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return au::err::kErrorNoMemory;
    } catch (...) {
        return au::err::kErrorUnknown;
    }
}

void discard(void*) {}

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
    (void)au::perf::Config::get();  // Perf state outlives worker teardown.
#if defined(__linux__) || defined(__ANDROID__)
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof(set), &set))
        for (unsigned i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) {
                mDefaultCpus.push_back(i);
                if (i < 64)
                    mDefaultMask |= UINT64_C(1) << i;
            }
#endif
    mMask = mDefaultMask;
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
    auto error = [&](int code) {
        if (succ->mState == State::Created && !succ->mBuild)
            succ->mBuild = code;
        return code;
    };

    if (pred == succ) {
        std::lock_guard<std::mutex> lock(succ->mMutex);
        return error(au::err::kErrorFlowCycleDetected);
    }

    if (!pred) {
        std::lock_guard<std::mutex> lock(succ->mMutex);
        return error(au::err::kErrorInvalidParam);
    }

    std::scoped_lock lock(pred->mMutex, succ->mMutex);
    if (succ->mState != State::Created)
        return au::err::kErrorStateInvalid;
    try {
        if (reaches(pred.get(), succ.get()))
            return error(au::err::kErrorFlowCycleDetected);
        if (std::find(succ->mPreds.begin(), succ->mPreds.end(), pred) != succ->mPreds.end())
            return 0;
        succ->mPreds.reserve(succ->mPreds.size() + 1);
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

int Core::submit(const std::vector<NodePtr>& nodes)
{
    std::list<NodePtr> batch;
    for (auto& n : nodes) {
        if (n->mState != State::Created)
            continue;
        if (n->mBuild)
            return n->mBuild;
        // Acquire placement construction before publishing callable execution.
        if (!n->mPayloadReady.load(std::memory_order_acquire))
            return au::err::kErrorNotReady;
        for (auto& p : n->mPreds)
            if (p->mState == State::Created && std::find(nodes.begin(), nodes.end(), p) == nodes.end())
                return au::err::kErrorInvalidParam;
        if (std::find(batch.begin(), batch.end(), n) == batch.end())
            batch.push_back(n);
    }

    if (batch.empty())
        return 0;
    if (mClosing && !gCurrent)
        return au::err::kErrorInvalidParam;
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
        if (j->mHelpers.load(std::memory_order_relaxed) < 3 && j->mNext.load(std::memory_order_relaxed) < j->mTotal)
            return j;
    return nullptr;
}

bool Core::step(std::unique_lock<std::mutex>& lock)
{
    if (mStop)
        return false;
    for (;;) {
        const auto generation = mAffinityGen.load(std::memory_order_acquire);
        if (!gWorkerIndex || gAffinity == generation)
            break;
        const auto mask     = mMask;
        const bool defaults = mPolicy == XFLOW_AFFINITY_DEFAULT;
        lock.unlock();
#if defined(__linux__) || defined(__ANDROID__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (defaults) {
            for (auto cpu : mDefaultCpus)
                CPU_SET(cpu, &set);
        } else
            for (unsigned i = 0; i < 64; ++i)
                if (mask & (UINT64_C(1) << i))
                    CPU_SET(i, &set);
        if (sched_setaffinity(0, sizeof(set), &set))
            gAffinity = 0;
        else
            gAffinity = generation;
#else
        (void)mask;
        (void)defaults;
        gAffinity = generation;
#endif
        lock.lock();
        if (mAffinityGen.load(std::memory_order_acquire) == generation)
            break;
    }

    if (auto* job = gWorkerIndex ? tile() : nullptr) {
        job->mHelpers.fetch_add(1, std::memory_order_relaxed);  // Job lifetime is also pinned by mMutex.
        lock.unlock();
        job->run(gWorkerIndex);
        lock.lock();
        job->mHelpers.fetch_sub(1, std::memory_order_relaxed);
        mChanged.notify_all();
        return true;
    }

    if (!mHead || gDepth >= 128)
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
    const bool          traced = !code && n->mFunction && !n->mName.empty();
    if (traced)
        au::perf::traceBegin(&trace, n->mName);
    if (!code && n->mFunction)
        code = boundary([&]() -> int { return n->mFunction(n->mPayload.get()); });
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
    {
        std::lock_guard<std::mutex> state(n->mMutex);
        n->mCode  = code;
        n->mCost  = cost;
        n->mState = State::Done;
        successors.swap(n->mSuccessors);
    }

    if (code && !mFirstError)
        mFirstError = code;
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
        return n->mBuild ? n->mBuild : au::err::kErrorNotReady;
    if (gTileDepth || gDepth >= 128 || (gCurrent && reaches(n.get(), gCurrent, true)))
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

NodePtr head(XFlowGroupHandle h) { return h ? h->mEntry : NodePtr(); }

NodePtr tail(XFlowGroupHandle h) { return h ? h->mExit : NodePtr(); }

std::unique_lock<std::mutex> submissionLock(au::flow::detail::Core& c)
{
    std::unique_lock<std::mutex> lock(c.mMutex);
    c.mChanged.wait(lock, [&] { return !c.mReleasing || au::flow::detail::gCurrent || au::flow::detail::gTileDepth; });
    return lock;
}

int link(const NodePtr& a, const NodePtr& b)
{
    return au::flow::detail::boundary([&]() -> int {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> l(c.mMutex);
        return c.edge(a, b);
    });
}

int incoming(const NodePtr& pred, XFlowGroupHandle b)
{
    if (!b)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        for (auto& n : b->mNodes)
            if (n->mState != au::flow::detail::State::Created)
                return au::err::kErrorStateInvalid;
        return c.edge(pred, head(b));
    });
}

template <class H>
void retain(H h)
{
    if (h)
        h->mRefs.fetch_add(1, std::memory_order_relaxed);
}

template <class H>
void release(H h)
{
    if (h && h->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete h;
}

std::string quote(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\')
            out += '\\';
        if (ch == '\n')
            out += "\\n";
        else if (ch == '\r')
            out += "\\r";
        else
            out += ch;
    }

    return out;
}
}  // namespace

extern "C" {
XFlowTaskHandle au_flow_create_task(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy) noexcept
{
    try {
        auto payload = std::unique_ptr<void, au::flow::detail::PayloadDeleter>(
            data, {destroy ? destroy : &au::flow::detail::discard, 0});
        if (!fn)
            return nullptr;
        auto owner               = std::make_shared<XFlowTaskNode>(fn, nullptr);
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
        auto  owner = std::make_shared<XFlowTaskNode>(fn, nullptr);
        auto& n     = owner->mStorage;
        n.mName     = name ? name : "";
        owner->mUsesInlinePayload =
            size <= XFlowTaskNode::kInlinePayloadCapacity && alignment <= alignof(std::max_align_t);
        void* storage = owner->mInlinePayload;
        if (!owner->mUsesInlinePayload) {
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

void au_flow_retain_task(XFlowTaskHandle h) noexcept { retain(h); }

void au_flow_release_task(XFlowTaskHandle h) noexcept
{
    // The alias owns the handle, Node and inline bytes together. Accepted work
    // may outlive the last C handle; only drop its C owning pin here.
    if (h && h->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto owner = std::move(h->mNode);
    }
}

int au_flow_add_dependency(XFlowTaskHandle a, XFlowTaskHandle b) noexcept { return link(node(a), node(b)); }

int au_flow_enqueue_task(XFlowTaskHandle h) noexcept
{
    if (!h)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        auto  l = submissionLock(c);
        return c.submit({node(h)});
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
        return h->mNode->mState == au::flow::detail::State::Done ? h->mNode->mCode : au::err::kErrorNotReady;
    });
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

XFlowGroupHandle au_flow_create_group(const char* name) noexcept
{
    try {
        auto g = std::make_unique<XFlowGroupNode>();
        for (int i = 0; i < 2; ++i)
            g->mNodes.push_back(
                std::make_shared<au::flow::detail::Node>(name, nullptr, nullptr, &au::flow::detail::discard));
        g->mEntry = g->mNodes.front();
        g->mExit  = g->mNodes.back();
        g->mBuild = link(head(g.get()), tail(g.get()));
        return g.release();
    } catch (...) {
        return nullptr;
    }
}

void au_flow_retain_group(XFlowGroupHandle h) noexcept { retain(h); }

void au_flow_release_group(XFlowGroupHandle h) noexcept { release(h); }

int au_flow_group_add_task(XFlowGroupHandle g, XFlowTaskHandle h) noexcept
{
    if (!g)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        if (head(g)->mState != au::flow::detail::State::Created)
            return au::err::kErrorStateInvalid;
        int code = au::err::kErrorInvalidParam;
        if (h && std::find(g->mNodes.begin(), g->mNodes.end(), node(h)) != g->mNodes.end())
            return 0;
        if (h && (!node(h)->mGrouped || node(h)->mState != au::flow::detail::State::Created)) {
            try {
                g->mNodes.reserve(g->mNodes.size() + 1);
                code = node(h)->mState == au::flow::detail::State::Created ? c.edge(head(g), node(h)) : 0;
                if (!code)
                    code = c.edge(node(h), tail(g));
                if (!code) {
                    g->mNodes.insert(g->mNodes.end() - 1, node(h));
                    node(h)->mGrouped = true;
                }
            } catch (...) {
                code = au::err::kErrorNoMemory;
            }
        }
        if (!g->mBuild)
            g->mBuild = code;
        return code;
    });
}

int au_flow_add_task_group_dependency(XFlowTaskHandle a, XFlowGroupHandle b) noexcept { return incoming(node(a), b); }

int au_flow_add_group_dependency(XFlowGroupHandle a, XFlowGroupHandle b) noexcept { return incoming(tail(a), b); }

int au_flow_add_group_task_dependency(XFlowGroupHandle a, XFlowTaskHandle b) noexcept { return link(tail(a), node(b)); }

int au_flow_enqueue_group(XFlowGroupHandle g) noexcept
{
    if (!g)
        return au::err::kErrorInvalidParam;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        auto  l = submissionLock(c);
        return g->mBuild ? g->mBuild : c.submit(g->mNodes);
    });
}

int au_flow_wait_group(XFlowGroupHandle g) noexcept
{
    return au::flow::detail::boundary([&]() -> int { return au::flow::detail::core().wait(tail(g)); });
}

const char* au_flow_dump_group_dot(XFlowGroupHandle g) noexcept
{
    if (!g)
        return nullptr;
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        std::set<NodePtr>           nodes(g->mNodes.begin(), g->mNodes.end());
        std::vector<NodePtr>        stack(g->mNodes);
        while (!stack.empty()) {
            auto n = stack.back();
            stack.pop_back();
            for (auto& p : n->mPreds)
                if (nodes.insert(p).second)
                    stack.push_back(p);
        }
        std::ostringstream text;
        text << "digraph Flow {\n";
        const char* states[] = {"created", "pending", "ready", "running", "done"};
        for (auto& n : nodes) {
            text << 'n' << reinterpret_cast<uintptr_t>(n.get()) << " [label=\"" << quote(n->mName)
                 << "\\nstate=" << states[static_cast<int>(n->mState)] << "\\ncost_us=" << n->mCost
                 << "\\ncode=" << n->mCode << "\"];\n";
            for (auto& p : n->mPreds)
                text << 'n' << reinterpret_cast<uintptr_t>(p.get()) << " -> n" << reinterpret_cast<uintptr_t>(n.get())
                     << ";\n";
        }
        thread_local std::string result;
        result = text.str() + "}\n";
        return result.c_str();
    } catch (...) {
        return nullptr;
    }
}

void au_flow_parallelize_tile(size_t range, size_t tile, XFlowTileFn fn, void* data) noexcept
{
    if (!fn)
        return;

    struct Callback
    {
        XFlowTileFn mFunction;
        void*       mData;
    } callback{fn, data};

    au_flow_parallelize_tile_ex(
        range, tile,
        [](size_t begin, size_t end, size_t, void* context) {
            auto& c = *static_cast<Callback*>(context);
            c.mFunction(begin, end, c.mData);
        },
        &callback);
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
        for (unsigned i = 0; i < 3; ++i) {
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
        return std::exchange(c.mFirstError, 0);
    });
}

void au_flow_release_idle_workers() noexcept
{
    // A synchronous task/tile cannot drain itself or join its own pool thread.
    if (au::flow::detail::gCurrent || au::flow::detail::gTileDepth || au::flow::detail::gWorkerIndex)
        return;
    (void)au::flow::detail::boundary([]() -> int {
        auto&                        c = au::flow::detail::core();
        std::array<std::thread, 8>   retired;
        std::unique_lock<std::mutex> lock(c.mMutex);
        c.mChanged.wait(lock, [&] { return !c.mReleasing; });
        if (c.mClosing)
            return au::err::kErrorInvalidParam;
        c.mReleasing = true;
        lock.unlock();
        (void)au_flow_wait_all();
        lock.lock();
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

int au_flow_set_affinity(int policy, uint64_t mask) noexcept
{
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        if (policy < XFLOW_AFFINITY_DEFAULT || policy > XFLOW_AFFINITY_CUSTOM_MASK)
            return au::err::kErrorInvalidParam;
        if (!c.mDefaultMask && policy != XFLOW_AFFINITY_DEFAULT)
            return au::err::kErrorPlatformAPI;
        if (policy == XFLOW_AFFINITY_DEFAULT)
            mask = c.mDefaultMask;
        else if (policy != XFLOW_AFFINITY_CUSTOM_MASK) {
            unsigned best = policy == XFLOW_AFFINITY_PERFORMANCE ? 0 : UINT32_MAX;
            mask          = 0;
            for (unsigned cpu = 0; cpu < 64; ++cpu)
                if (c.mDefaultMask & (UINT64_C(1) << cpu)) {
                    unsigned      capacity = 1024;
                    std::ifstream input("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
                    input >> capacity;
                    if ((policy == XFLOW_AFFINITY_PERFORMANCE && capacity > best) ||
                        (policy == XFLOW_AFFINITY_POWERSAVING && capacity < best)) {
                        best = capacity;
                        mask = 0;
                    }
                    if (capacity == best)
                        mask |= UINT64_C(1) << cpu;
                }
        }
        if ((policy != XFLOW_AFFINITY_DEFAULT && !mask) || (mask & ~c.mDefaultMask))
            return au::err::kErrorPlatformAPI;
        std::lock_guard<std::mutex> lock(c.mMutex);
        c.mMask   = mask;
        c.mPolicy = policy;
        c.mAffinityGen.fetch_add(1, std::memory_order_release);  // Workers acquire configuration after taking work.
        c.mChanged.notify_all();
        return 0;
    });
}

uint64_t au_flow_get_affinity_mask() noexcept
{
    try {
        auto&                       c = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(c.mMutex);
        return c.mMask;
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
