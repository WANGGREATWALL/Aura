#include "flow/xflow_core.h"
#include "perf/xtimer.h"
#include "perf/xtracer.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif
namespace au { namespace flow AU_LOCAL { namespace detail {
thread_local Node* gCurrent = nullptr;
thread_local bool gWorker = false;
thread_local unsigned gTileDepth = 0, gDepth = 0;
thread_local std::uint64_t gAffinity = 0;
template<class F> int boundary(F fn) noexcept {
    try { return fn(); } catch (const std::bad_alloc&) { return XFLOW_ERROR_NO_MEMORY; }
    catch (...) { return XFLOW_ERROR_EXCEPTION; }
}
void discard(void*) {}
Core& core() { static Core instance; return instance; }
Core::Core() {
    (void)au::perf::Config::get(); // Perf state outlives worker teardown.
#if defined(__linux__) || defined(__ANDROID__)
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof(set), &set))
        for (unsigned i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) {
            mDefaultCpus.push_back(i);
            if (i < 64) mDefaultMask |= UINT64_C(1) << i;
        }
#endif
    mMask = mDefaultMask;
}
Core::~Core() {
    std::unique_lock<std::mutex> lock(mMutex);
    mClosing = true;
    mChanged.wait(lock, [&] { return mAccepted.empty() && !mTileCallers; });
    mStop = true;
    mChanged.notify_all();
    lock.unlock();
    for (auto& thread : mThreads) if (thread.joinable()) thread.join();
}
bool Core::reaches(Node* from, Node* target, bool waits) {
    std::vector<Node*> stack{from};
    std::set<Node*> seen;
    while (!stack.empty()) {
        auto* n = stack.back(); stack.pop_back();
        if (!n || !seen.insert(n).second) continue;
        if (n == target) return true;
        for (auto& pred : n->mPreds) stack.push_back(pred.get());
        if (waits) { stack.push_back(n->mWaiting); stack.push_back(n->mExecuting); }
    }
    return false;
}
int Core::edge(const NodePtr& pred, const NodePtr& succ) {
    if (!succ) return XFLOW_ERROR_INVALID_ARGUMENT;
    auto error = [&](int code) {
        if (succ->mState == State::Created && !succ->mBuild) succ->mBuild = code;
        return code;
    };
    if (pred == succ) { std::lock_guard<std::mutex> lock(succ->mMutex); return error(XFLOW_ERROR_CYCLE_DETECTED); }
    if (!pred) { std::lock_guard<std::mutex> lock(succ->mMutex); return error(XFLOW_ERROR_INVALID_ARGUMENT); }
    std::scoped_lock lock(pred->mMutex, succ->mMutex);
    if (succ->mState != State::Created) return XFLOW_ERROR_ALREADY_ENQUEUED;
    try {
        if (reaches(pred.get(), succ.get())) return error(XFLOW_ERROR_CYCLE_DETECTED);
        if (std::find(succ->mPreds.begin(), succ->mPreds.end(), pred) != succ->mPreds.end()) return 0;
        succ->mPreds.reserve(succ->mPreds.size() + 1);
        if (pred->mState != State::Done) {
            pred->mSuccessors.emplace_back(succ);
            ++succ->mPending;
        } else if (!succ->mDependencyError) succ->mDependencyError = pred->mCode;
        succ->mPreds.push_back(pred);
    } catch (...) { return error(XFLOW_ERROR_NO_MEMORY); }
    return 0;
}
void Core::ready(Node* node) {
    node->mState = State::Ready;
    if (mTail) mTail->mNext = node; else mHead = node;
    mTail = node;
}
void Core::grow() {
    if (!mIdle && mCount < mThreads.size()) {
        mThreads[mCount] = std::thread([this] { worker(); });
        ++mCount;
    }
}
int Core::submit(const std::vector<NodePtr>& nodes) {
    std::list<NodePtr> batch;
    for (auto& n : nodes) {
        if (n->mState != State::Created) continue;
        if (n->mBuild) return n->mBuild;
        for (auto& p : n->mPreds)
            if (p->mState == State::Created && std::find(nodes.begin(), nodes.end(), p) == nodes.end())
                return XFLOW_ERROR_INVALID_ARGUMENT;
        if (std::find(batch.begin(), batch.end(), n) == batch.end()) batch.push_back(n);
    }
    if (batch.empty()) return 0;
    if (mClosing && !gCurrent) return XFLOW_ERROR_INVALID_ARGUMENT;
    grow(); // No accepted state if the required thread could not start.
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        auto& n = *it;
        std::lock_guard<std::mutex> lock(n->mMutex);
        n->mAccepted = it;
        n->mState = State::Pending;
        if (!n->mPending) ready(n.get());
    }
    mAccepted.splice(mAccepted.end(), batch);
    mChanged.notify_all();
    return 0;
}
TileJob* Core::tile() {
    for (auto* j = mTiles; j; j = j->mLink)
        if (j->mHelpers.load(std::memory_order_relaxed) < 3 &&
            j->mNext.load(std::memory_order_relaxed) < j->mTotal) return j;
    return nullptr;
}
bool Core::step(std::unique_lock<std::mutex>& lock) {
    for (;;) {
        const auto generation = mAffinityGen.load(std::memory_order_acquire);
        if (!gWorker || gAffinity == generation) break;
        const auto mask = mMask;
        const bool defaults = mPolicy == XFLOW_AFFINITY_DEFAULT;
        lock.unlock();
#if defined(__linux__) || defined(__ANDROID__)
        cpu_set_t set; CPU_ZERO(&set);
        if (defaults) { for (auto cpu : mDefaultCpus) CPU_SET(cpu, &set); }
        else for (unsigned i = 0; i < 64; ++i) if (mask & (UINT64_C(1) << i)) CPU_SET(i, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) gAffinity = 0; else gAffinity = generation;
#else
        (void)mask; (void)defaults; gAffinity = generation;
#endif
        lock.lock();
        if (mAffinityGen.load(std::memory_order_acquire) == generation) break;
    }
    if (auto* job = tile()) {
        job->mHelpers.fetch_add(1, std::memory_order_relaxed); // Job lifetime is also pinned by mMutex.
        lock.unlock(); job->run(); lock.lock();
        job->mHelpers.fetch_sub(1, std::memory_order_relaxed);
        mChanged.notify_all();
        return true;
    }
    if (!mHead || gDepth >= 128) return false;
    NodePtr n = *mHead->mAccepted;
    mHead = n->mNext;
    if (!mHead) mTail = nullptr;
    n->mNext = nullptr;
    { std::lock_guard<std::mutex> state(n->mMutex); n->mState = State::Running; }
    Node* parent = gCurrent;
    if (parent) parent->mExecuting = n.get();
    if (mHead) { try { grow(); } catch (...) {} }
    lock.unlock();
    gCurrent = n.get(); ++gDepth;
    int code = n->mDependencyError;
    const auto start = std::chrono::steady_clock::now();
    au::perf::PerfScope trace{};
    const bool traced = !code && n->mFunction && !n->mName.empty();
    if (traced) au::perf::traceBegin(&trace, n->mName);
    if (!code && n->mFunction) code = boundary([&]() -> int { return n->mFunction(n->mPayload.get()); });
    if (traced) au::perf::traceEnd(&trace);
    const auto cost = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    n->mPayload.reset();
    --gDepth; gCurrent = parent;
    lock.lock();
    if (parent) parent->mExecuting = nullptr;
    std::vector<std::weak_ptr<Node>> successors;
    { std::lock_guard<std::mutex> state(n->mMutex);
      n->mCode = code; n->mCost = cost; n->mState = State::Done; successors.swap(n->mSuccessors); }
    if (code && !mFirstError) mFirstError = code;
    for (auto& weak : successors) if (auto succ = weak.lock()) {
        { std::lock_guard<std::mutex> state(succ->mMutex);
          if (code && !succ->mDependencyError) succ->mDependencyError = code;
          if (!--succ->mPending && succ->mState == State::Pending) ready(succ.get()); }
        lock.unlock(); succ.reset(); lock.lock();
    }
    mAccepted.erase(n->mAccepted);
    mChanged.notify_all();
    lock.unlock(); n.reset(); lock.lock(); // Last owners/deleters never run under scheduler locks.
    return true;
}
void Core::worker() {
    gWorker = true;
    std::unique_lock<std::mutex> lock(mMutex);
    while (!mStop) {
        const auto generation = mAffinityGen.load(std::memory_order_acquire);
        if (step(lock)) continue;
        ++mIdle;
        mChanged.wait(lock, [&] {
            return mStop || mHead || tile() || mAffinityGen.load(std::memory_order_acquire) != generation;
        });
        --mIdle;
    }
}
int Core::wait(const NodePtr& n) {
    if (!n) return XFLOW_ERROR_INVALID_ARGUMENT;
    std::unique_lock<std::mutex> lock(mMutex);
    if (n->mState == State::Done) return n->mCode;
    if (n->mState == State::Created) return n->mBuild ? n->mBuild : XFLOW_ERROR_NOT_READY;
    if (gTileDepth || gDepth >= 128 || (gCurrent && reaches(n.get(), gCurrent, true)))
        return XFLOW_ERROR_WOULD_DEADLOCK;
    Node* unused = nullptr;
    Node*& waiting = gCurrent ? gCurrent->mWaiting : unused;
    struct Restore { Node*& mSlot; Node* mOld; ~Restore() { mSlot = mOld; } } restore{waiting, waiting};
    waiting = n.get();
    while (n->mState != State::Done) if (!step(lock)) mChanged.wait(lock);
    return n->mCode;
}
void TileJob::run() noexcept {
    ++gTileDepth;
    std::size_t index = mNext.load(std::memory_order_relaxed);
    while (index < mTotal) {
        if (!mNext.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) continue;
        const auto begin = index * mTile;
        try { mFunction(begin, begin + std::min(mTile, mRange - begin), mData); } catch (...) { std::terminate(); }
        index = mNext.load(std::memory_order_relaxed);
    }
    --gTileDepth;
}
}}}
namespace {
typedef au::flow::detail::NodePtr NodePtr;
NodePtr node(XFlowTaskHandle h) { return h ? h->mNode : NodePtr(); }
NodePtr head(XFlowGroupHandle h) { return h ? h->mEntry : NodePtr(); }
NodePtr tail(XFlowGroupHandle h) { return h ? h->mExit : NodePtr(); }
int link(const NodePtr& a, const NodePtr& b) {
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> l(c.mMutex);
        return c.edge(a,b);
    });
}
int incoming(const NodePtr& pred, XFlowGroupHandle b) {
    if (!b) return XFLOW_ERROR_INVALID_ARGUMENT;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> lock(c.mMutex);
        for (auto& n : b->mNodes)
            if (n->mState != au::flow::detail::State::Created) return XFLOW_ERROR_ALREADY_ENQUEUED;
        return c.edge(pred, head(b));
    });
}
template<class H> void retain(H h) { if (h) h->mRefs.fetch_add(1, std::memory_order_relaxed); }
template<class H> void release(H h) { if (h && h->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete h; }
std::string quote(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\';
        if (ch == '\n') out += "\\n"; else if (ch == '\r') out += "\\r"; else out += ch;
    }
    return out;
}
}
extern "C" {
XFlowTaskHandle XFlow_CreateTask(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy) noexcept {
    try {
        auto payload = std::unique_ptr<void, XFlowDeleterFn>(data, destroy ? destroy : &au::flow::detail::discard);
        if (!fn) return nullptr;
        auto n = std::make_shared<au::flow::detail::Node>(nullptr, fn, nullptr, payload.get_deleter());
        n->mPayload = std::move(payload); n->mName = name ? name : "";
        auto h = std::make_unique<XFlowTaskNode>(); h->mNode = std::move(n); return h.release();
    } catch (...) { return nullptr; }
}
void XFlow_RetainTask(XFlowTaskHandle h) noexcept { retain(h); }
void XFlow_ReleaseTask(XFlowTaskHandle h) noexcept { release(h); }
int XFlow_AddDependency(XFlowTaskHandle a, XFlowTaskHandle b) noexcept { return link(node(a), node(b)); }
int XFlow_EnqueueTask(XFlowTaskHandle h) noexcept {
    if (!h) return XFLOW_ERROR_INVALID_ARGUMENT;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> l(c.mMutex);
        return c.submit({node(h)});
    });
}
int XFlow_WaitTask(XFlowTaskHandle h) noexcept {
    return au::flow::detail::boundary([&]() -> int { return au::flow::detail::core().wait(node(h)); });
}
int XFlow_GetTaskResult(XFlowTaskHandle h) noexcept {
    if (!h) return XFLOW_ERROR_INVALID_ARGUMENT;
    return au::flow::detail::boundary([&]() -> int {
        std::lock_guard<std::mutex> lock(h->mNode->mMutex);
        return h->mNode->mState == au::flow::detail::State::Done ? h->mNode->mCode : XFLOW_ERROR_NOT_READY;
    });
}
int64_t XFlow_GetTaskCostUs(XFlowTaskHandle h) noexcept {
    if (!h) return 0;
    try { std::lock_guard<std::mutex> lock(h->mNode->mMutex); return h->mNode->mCost; } catch (...) { return 0; }
}
XFlowGroupHandle XFlow_CreateGroup(const char* name) noexcept {
    try {
        auto g = std::make_unique<XFlowGroupNode>();
        for (int i = 0; i < 2; ++i) g->mNodes.push_back(std::make_shared<au::flow::detail::Node>(
            name, nullptr, nullptr, &au::flow::detail::discard));
        g->mEntry = g->mNodes.front(); g->mExit = g->mNodes.back();
        g->mBuild = link(head(g.get()), tail(g.get())); return g.release();
    } catch (...) { return nullptr; }
}
void XFlow_RetainGroup(XFlowGroupHandle h) noexcept { retain(h); }
void XFlow_ReleaseGroup(XFlowGroupHandle h) noexcept { release(h); }
int XFlow_GroupAddTask(XFlowGroupHandle g, XFlowTaskHandle h) noexcept {
    if (!g) return XFLOW_ERROR_INVALID_ARGUMENT;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> lock(c.mMutex);
        if (head(g)->mState != au::flow::detail::State::Created) return XFLOW_ERROR_ALREADY_ENQUEUED;
        int code = XFLOW_ERROR_INVALID_ARGUMENT;
        if (h && std::find(g->mNodes.begin(), g->mNodes.end(), node(h)) != g->mNodes.end()) return 0;
        if (h && (!node(h)->mGrouped || node(h)->mState != au::flow::detail::State::Created)) {
            try {
                g->mNodes.reserve(g->mNodes.size() + 1);
                code = node(h)->mState == au::flow::detail::State::Created ? c.edge(head(g), node(h)) : 0;
                if (!code) code = c.edge(node(h), tail(g));
                if (!code) { g->mNodes.insert(g->mNodes.end()-1, node(h)); node(h)->mGrouped = true; }
            } catch (...) { code = XFLOW_ERROR_NO_MEMORY; }
        }
        if (!g->mBuild) g->mBuild = code;
        return code;
    });
}
int XFlow_AddTaskGroupDependency(XFlowTaskHandle a, XFlowGroupHandle b) noexcept { return incoming(node(a), b); }
int XFlow_AddGroupDependency(XFlowGroupHandle a, XFlowGroupHandle b) noexcept { return incoming(tail(a), b); }
int XFlow_AddGroupTaskDependency(XFlowGroupHandle a, XFlowTaskHandle b) noexcept { return link(tail(a), node(b)); }
int XFlow_EnqueueGroup(XFlowGroupHandle g) noexcept {
    if (!g) return XFLOW_ERROR_INVALID_ARGUMENT;
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> l(c.mMutex);
        return g->mBuild ? g->mBuild : c.submit(g->mNodes);
    });
}
int XFlow_WaitGroup(XFlowGroupHandle g) noexcept {
    return au::flow::detail::boundary([&]() -> int { return au::flow::detail::core().wait(tail(g)); });
}
const char* XFlow_DumpGroupDot(XFlowGroupHandle g) noexcept {
    if (!g) return nullptr;
    try {
        auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> lock(c.mMutex);
        std::set<NodePtr> nodes(g->mNodes.begin(), g->mNodes.end());
        std::vector<NodePtr> stack(g->mNodes);
        while (!stack.empty()) {
            auto n = stack.back(); stack.pop_back();
            for (auto& p : n->mPreds) if (nodes.insert(p).second) stack.push_back(p);
        }
        std::ostringstream text; text << "digraph Flow {\n";
        const char* states[] = {"created", "pending", "ready", "running", "done"};
        for (auto& n : nodes) {
            text << 'n' << reinterpret_cast<uintptr_t>(n.get()) << " [label=\"" << quote(n->mName)
                 << "\\nstate=" << states[static_cast<int>(n->mState)] << "\\ncost_us=" << n->mCost
                 << "\\ncode=" << n->mCode << "\"];\n";
            for (auto& p : n->mPreds) text << 'n' << reinterpret_cast<uintptr_t>(p.get())
                << " -> n" << reinterpret_cast<uintptr_t>(n.get()) << ";\n";
        }
        thread_local std::string result; result = text.str() + "}\n"; return result.c_str();
    } catch (...) { return nullptr; }
}
void XFlow_ParallelizeTile(size_t range, size_t tile, XFlowTileFn fn, void* data) noexcept {
    if (!range || !fn) return;
    tile = tile ? tile : 1;
    au::flow::detail::TileJob job;
    job.mRange = range; job.mTile = tile; job.mTotal = range / tile + (range % tile != 0);
    job.mFunction = fn; job.mData = data;
    if (job.mTotal <= 1 || au::flow::detail::gTileDepth) { job.run(); return; }
    au::flow::detail::Core* runtime;
    try { runtime = &au::flow::detail::core(); } catch (...) { job.run(); return; }
    auto& c = *runtime;
    std::unique_lock<std::mutex> lock(c.mMutex);
    if (c.mClosing) { lock.unlock(); job.run(); return; }
    ++c.mTileCallers; job.mLink = c.mTiles; c.mTiles = &job;
    for (unsigned i = 0; i < 3; ++i) { try { c.grow(); } catch (...) { break; } c.mChanged.notify_one(); }
    lock.unlock(); job.run(); lock.lock();
    auto** link = &c.mTiles;
    while (*link != &job) link = &(*link)->mLink;
    *link = job.mLink;
    c.mChanged.wait(lock, [&] { return job.mHelpers.load(std::memory_order_relaxed) == 0; });
    --c.mTileCallers; c.mChanged.notify_all();
}
int XFlow_WaitAll() noexcept {
    return au::flow::detail::boundary([]() -> int {
        if (au::flow::detail::gCurrent || au::flow::detail::gTileDepth) return XFLOW_ERROR_WOULD_DEADLOCK;
        auto& c = au::flow::detail::core(); std::unique_lock<std::mutex> lock(c.mMutex);
        while (!c.mAccepted.empty()) if (!c.step(lock)) c.mChanged.wait(lock);
        return std::exchange(c.mFirstError, 0);
    });
}
int XFlow_SetAffinity(int policy, uint64_t mask) noexcept {
    return au::flow::detail::boundary([&]() -> int {
        auto& c = au::flow::detail::core();
        if (policy < XFLOW_AFFINITY_DEFAULT || policy > XFLOW_AFFINITY_CUSTOM_MASK) return XFLOW_ERROR_INVALID_ARGUMENT;
        if (!c.mDefaultMask && policy != XFLOW_AFFINITY_DEFAULT) return XFLOW_ERROR_AFFINITY_FAILED;
        if (policy == XFLOW_AFFINITY_DEFAULT) mask = c.mDefaultMask;
        else if (policy != XFLOW_AFFINITY_CUSTOM_MASK) {
            unsigned best = policy == XFLOW_AFFINITY_PERFORMANCE ? 0 : UINT32_MAX;
            mask = 0;
            for (unsigned cpu = 0; cpu < 64; ++cpu) if (c.mDefaultMask & (UINT64_C(1) << cpu)) {
                unsigned capacity = 1024;
                std::ifstream input("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
                input >> capacity;
                if ((policy == XFLOW_AFFINITY_PERFORMANCE && capacity > best) ||
                    (policy == XFLOW_AFFINITY_POWERSAVING && capacity < best)) { best = capacity; mask = 0; }
                if (capacity == best) mask |= UINT64_C(1) << cpu;
            }
        }
        if ((policy != XFLOW_AFFINITY_DEFAULT && !mask) || (mask & ~c.mDefaultMask)) return XFLOW_ERROR_AFFINITY_FAILED;
        std::lock_guard<std::mutex> lock(c.mMutex); c.mMask = mask; c.mPolicy = policy;
        c.mAffinityGen.fetch_add(1, std::memory_order_release); // Workers acquire configuration after taking work.
        c.mChanged.notify_all(); return 0;
    });
}
uint64_t XFlow_GetAffinityMask() noexcept {
    try { auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> lock(c.mMutex); return c.mMask; }
    catch (...) { return 0; }
}
size_t XFlow_GetWorkerCount() noexcept {
    try { auto& c = au::flow::detail::core(); std::lock_guard<std::mutex> lock(c.mMutex); return c.mCount; }
    catch (...) { return 0; }
}
}
