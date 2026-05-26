#include "perf/xtimer.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include "log/xlogger.h"
#include "sys/xplatform.h"

namespace au {
namespace perf {

// ===========================================================================
//  File-local types, globals, and helpers
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
//  Data structures
// ---------------------------------------------------------------------------

/// One node in the per-thread tree.
/// durationMs == -1.0f  →  node is still open (not yet closed).
/// durationMs >= 0.0f   →  node is closed; value is elapsed milliseconds.
struct NodePerf
{
    uint32_t                              nameOffset{0};
    uint32_t                              nameLen{0};
    int32_t                               parent{-1};
    int32_t                               firstChild{-1};
    int32_t                               lastChild{-1};
    int32_t                               nextSibling{-1};
    uint32_t                              depth{0};
    float                                 durationMs{-1.0f};  ///< -1.0f = open sentinel
    std::chrono::steady_clock::time_point begin{};
};

struct CtxThread
{
    std::vector<NodePerf> pool;
    std::vector<char>     nameArena;
    std::vector<int32_t>  openStack;

    uint64_t tid{0};
    bool     inited{false};
    bool     inFlush{false};
    uint32_t releaseDepth{0};  ///< Release-mode per-thread depth counter.

    /// Meyers-style thread-local accessor; lazy-initialises on first call per thread.
    static CtxThread& get() noexcept
    {
        thread_local CtxThread ctx;
        if (!ctx.inited) {
            ctx.tid    = au::sys::getCurrentThreadId();
            ctx.inited = true;
            ctx.pool.reserve(128);
            ctx.nameArena.reserve(2048);
            ctx.openStack.reserve(16);
        }
        return ctx;
    }
};

/// Snapshot of one thread's complete timing tree, captured at outermost-scope close.
struct TreeSnapThread
{
    std::vector<NodePerf> pool;
    std::vector<char>     nameArena;
    std::vector<int32_t>  roots;
    uint64_t              tid;
    std::string           rootName;
};

struct TreesSnap
{
    std::mutex                  mMutex;
    std::vector<TreeSnapThread> mTrees;

    /// Meyers singleton; owns the aggregate buffer for all threads.
    static TreesSnap& get() noexcept
    {
        static TreesSnap instance;
        return instance;
    }
};

// ---------------------------------------------------------------------------
//  Functions
// ---------------------------------------------------------------------------

int32_t emplaceNode(CtxThread& ctx, int32_t parent, const std::string& name, uint32_t depth,
                    std::chrono::steady_clock::time_point beginTp) noexcept
{
    try {
        const int32_t idx = static_cast<int32_t>(ctx.pool.size());

        NodePerf node{};
        node.nameOffset = static_cast<uint32_t>(ctx.nameArena.size());
        node.nameLen    = static_cast<uint32_t>(name.size());
        node.parent     = parent;
        node.depth      = depth;
        node.begin      = beginTp;
        ctx.pool.push_back(node);
        ctx.nameArena.insert(ctx.nameArena.end(), name.begin(), name.end());

        // Root nodes are collected lazily via collectRoots() at flush time;
        // their nextSibling field is never traversed, so we only maintain
        // the sibling chain for non-root nodes.
        if (parent != -1) {
            NodePerf& parentNode = ctx.pool[static_cast<std::size_t>(parent)];
            if (parentNode.firstChild == -1) {
                parentNode.firstChild = idx;
            } else {
                ctx.pool[static_cast<std::size_t>(parentNode.lastChild)].nextSibling = idx;
            }
            parentNode.lastChild = idx;
        }
        return idx;
    } catch (...) {
        return -1;
    }
}

std::vector<int32_t> collectRoots(const std::vector<NodePerf>& pool)
{
    std::vector<int32_t> roots;
    roots.reserve(8);
    for (int32_t i = 0; i < static_cast<int32_t>(pool.size()); ++i) {
        if (pool[static_cast<std::size_t>(i)].parent == -1) {
            roots.push_back(i);
        }
    }
    return roots;
}

// Recursively print one node and all its descendants.
// isLast     — whether this node is the last child of its parent (drives branch glyph).
// prefix     — the indentation string inherited from all ancestor levels.
// childIsLast is derived directly from nextSibling == -1, so no child-list allocation is needed.
void printNode(const TreeSnapThread& t, int32_t idx, const std::string& prefix, bool isLast) noexcept
{
    const NodePerf& n    = t.pool[static_cast<std::size_t>(idx)];
    const char*     name = (n.nameLen == 0) ? "" : (t.nameArena.data() + n.nameOffset);

    const bool  closed   = (n.durationMs >= 0.0f);
    const char* branch   = prefix.empty() ? "" : (isLast ? "`-- " : "|-- ");
    const float ms       = closed ? n.durationMs : 0.0f;
    const char* openMark = closed ? "" : " (open)";

    XLOG_I("%s%s%.*s: %.3f ms%s\n", prefix.c_str(), branch, static_cast<int>(n.nameLen), name, ms, openMark);

    const std::string childPrefix = prefix + (isLast ? "    " : "|   ");
    for (int32_t c = n.firstChild; c != -1; c = t.pool[static_cast<std::size_t>(c)].nextSibling) {
        const bool childIsLast = (t.pool[static_cast<std::size_t>(c)].nextSibling == -1);
        printNode(t, c, childPrefix, childIsLast);
    }
}

void printTree(const TreeSnapThread& t) noexcept
{
    if (t.roots.empty()) {
        return;
    }

    XLOG_I("[perf][tid=0x%llx] %s\n", static_cast<unsigned long long>(t.tid),
           t.rootName.empty() ? "perf" : t.rootName.c_str());

    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        printNode(t, t.roots[i], std::string(), i + 1 == t.roots.size());
    }
}

}  // anonymous namespace

// ===========================================================================
//  Config — Meyers singleton
// ===========================================================================

void Config::setEnabled(bool on) noexcept { mEnabled.store(on, std::memory_order_relaxed); }

bool Config::isEnabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }

void Config::setDebugMode(bool on) noexcept { mDebugMode.store(on, std::memory_order_relaxed); }

bool Config::isDebugMode() const noexcept { return mDebugMode.load(std::memory_order_relaxed); }

void Config::setTimerLevel(int32_t threshold) noexcept { mTimerLevel.store(threshold, std::memory_order_relaxed); }

int32_t Config::getTimerLevel() const noexcept { return mTimerLevel.load(std::memory_order_relaxed); }

void Config::setTracerLevel(int32_t threshold) noexcept { mTracerLevel.store(threshold, std::memory_order_relaxed); }

int32_t Config::getTracerLevel() const noexcept { return mTracerLevel.load(std::memory_order_relaxed); }

void Config::setRootName(const std::string& name) noexcept
{
    std::lock_guard<std::mutex> lk(mRootNameMutex);
    mRootName = name;
}

std::string Config::getRootName() const noexcept
{
    std::lock_guard<std::mutex> lk(mRootNameMutex);
    return mRootName;
}

void Config::setAggregateMode(bool on) noexcept { mAggregate.store(on, std::memory_order_relaxed); }

bool Config::isAggregateMode() const noexcept { return mAggregate.load(std::memory_order_relaxed); }

void Config::flushAggregated() noexcept
{
    std::vector<TreeSnapThread> local;
    try {
        TreesSnap&                  agg = TreesSnap::get();
        std::lock_guard<std::mutex> lk(agg.mMutex);
        local.swap(agg.mTrees);
    } catch (...) {
        return;
    }
    if (local.empty()) {
        return;
    }
    std::stable_sort(local.begin(), local.end(),
                     [](const TreeSnapThread& a, const TreeSnapThread& b) { return a.tid < b.tid; });
    XLOG_I("[perf] ===== aggregate flush: %zu block(s) =====\n", local.size());
    for (const TreeSnapThread& t : local) {
        printTree(t);
    }
    XLOG_I("[perf] ===== end =====\n");
}

// ===========================================================================
//  XTimer — bare stopwatch + static helpers
// ===========================================================================

void XTimer::sleepFor(int64_t ms) noexcept
{
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string XTimer::getTimeFormatted(const std::string& fmt) noexcept
{
    using namespace std::chrono;
    const auto  now = system_clock::now();
    std::time_t t   = system_clock::to_time_t(now);

    std::tm tm{};
#if AU_OS_WINDOWS
    if (localtime_s(&tm, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &tm) == nullptr) {
        return {};
    }
#endif

    std::array<char, 128> outBuf{};
    const char*           fmtPtr  = fmt.empty() ? "%Y-%m-%d-%H-%M-%S" : fmt.c_str();
    const std::size_t     written = std::strftime(outBuf.data(), outBuf.size(), fmtPtr, &tm);
    if (written == 0) {
        return {};
    }

    const auto subMs = duration_cast<milliseconds>(now.time_since_epoch()).count() -
                       duration_cast<seconds>(now.time_since_epoch()).count() * 1000;

    std::string out(outBuf.data(), written);
    out.push_back('_');
    out.append(std::to_string(subMs));
    return out;
}

float XTimer::elapsedMs() const noexcept
{
    return std::chrono::duration<float, std::milli>(Clock::now() - mBegin).count();
}

// ===========================================================================
//  XTimerScoped
// ===========================================================================

XTimerScoped::XTimerScoped(const std::string& name) noexcept
    : mNodeIdx(-1),
      mSubNodeIdx(-1),
      mDepth(0),
      mIsRoot(false),
      mBegin(std::chrono::steady_clock::now()),
      mSubBegin(mBegin),
      mName(name)
{
    if (!Config::get().isEnabled())
        return;

    if (CtxThread::get().inFlush)
        return;

    const bool     debugMode = Config::get().isDebugMode();
    const uint32_t depth =
        debugMode ? static_cast<uint32_t>(CtxThread::get().openStack.size()) : CtxThread::get().releaseDepth;
    const int32_t threshold = Config::get().getTimerLevel();

    if (depth >= Config::HARD_MAX_DEPTH || threshold == Config::LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    // All active paths need depth; assign once after all guards pass.
    mDepth = depth;

    if (!debugMode) {
        mNodeIdx = -2;
        ++CtxThread::get().releaseDepth;
        return;
    }

    // -- Debug path --
    const int32_t parent = CtxThread::get().openStack.empty() ? -1 : CtxThread::get().openStack.back();
    const int32_t idx    = emplaceNode(CtxThread::get(), parent, mName, depth, mBegin);
    if (idx < 0) {
        mNodeIdx = -2;
        return;
    }

    try {
        CtxThread::get().openStack.push_back(idx);
    } catch (...) {
        mNodeIdx = -2;
        return;
    }
    mNodeIdx = idx;
    mIsRoot  = (depth == 0);
}

XTimerScoped::~XTimerScoped() noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    const auto  now = std::chrono::steady_clock::now();
    const float msF = std::chrono::duration<float, std::milli>(now - mBegin).count();

    // -- Release / degraded path: one-liner --
    if (mNodeIdx == -2) {
        if (!mSubName.empty()) {
            const float subMs = std::chrono::duration<float, std::milli>(now - mSubBegin).count();
            XLOG_I("[perf] %s: %.3f ms\n", mSubName.c_str(), subMs);
        }
        XLOG_I("[perf] %s: %.3f ms\n", mName.c_str(), msF);
        if (CtxThread::get().releaseDepth > 0u) {
            --CtxThread::get().releaseDepth;
        }
        return;
    }

    // -- Debug path --
    if (CtxThread::get().inFlush) {
        return;
    }

    if (mNodeIdx >= 0 && mNodeIdx < static_cast<int32_t>(CtxThread::get().pool.size())) {
        CtxThread::get().pool[static_cast<std::size_t>(mNodeIdx)].durationMs = msF;
    }
    if (!CtxThread::get().openStack.empty() && CtxThread::get().openStack.back() == mNodeIdx) {
        CtxThread::get().openStack.pop_back();
    }

    if (!mIsRoot || !CtxThread::get().openStack.empty()) {
        return;
    }

    // -- Outermost scope: flush this thread's tree --
    CtxThread::get().inFlush = true;

    try {
        // Build a snapshot by moving TLS data — O(1), no copies.
        TreeSnapThread snap;
        snap.pool      = std::move(CtxThread::get().pool);
        snap.nameArena = std::move(CtxThread::get().nameArena);
        snap.roots     = collectRoots(snap.pool);
        snap.tid       = CtxThread::get().tid;
        snap.rootName  = Config::get().getRootName();

        if (Config::get().isAggregateMode()) {
            TreesSnap&                  agg = TreesSnap::get();
            std::lock_guard<std::mutex> lk(agg.mMutex);
            agg.mTrees.emplace_back(std::move(snap));
        } else {
            printTree(snap);
        }
    } catch (...) {
    }

    CtxThread::get().pool.clear();
    CtxThread::get().nameArena.clear();
    CtxThread::get().openStack.clear();
    CtxThread::get().inFlush = false;
}

std::chrono::steady_clock::time_point XTimerScoped::closeOpenSub() noexcept
{
    const auto now = std::chrono::steady_clock::now();

    // ── Release path ──
    if (mNodeIdx == -2) {
        if (!mSubName.empty()) {
            const float ms = std::chrono::duration<float, std::milli>(now - mSubBegin).count();
            XLOG_I("[perf] %s: %.3f ms\n", mSubName.c_str(), ms);
            mSubName.clear();
        }
        return now;
    }

    // ── Debug path ──
    if (mSubNodeIdx < 0) {
        return now;
    }
    if (CtxThread::get().inFlush) {
        return now;
    }

    if (mSubNodeIdx < static_cast<int32_t>(CtxThread::get().pool.size())) {
        NodePerf& prev = CtxThread::get().pool[static_cast<std::size_t>(mSubNodeIdx)];
        // Guard against double-close: only write if still open.
        if (prev.durationMs < 0.0f) {
            prev.durationMs = std::chrono::duration<float, std::milli>(now - prev.begin).count();
        }
    }
    if (!CtxThread::get().openStack.empty() && CtxThread::get().openStack.back() == mSubNodeIdx) {
        CtxThread::get().openStack.pop_back();
    }
    mSubNodeIdx = -1;
    return now;
}

void XTimerScoped::sub(const std::string& name) noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    const auto now = closeOpenSub();

    // ── Release path: arm the new sub-segment ──
    if (mNodeIdx == -2) {
        mSubName  = name;
        mSubBegin = now;
        return;
    }

    // ── Debug path: open a new sub-node under the outer scope ──
    if (CtxThread::get().inFlush) {
        return;
    }

    const uint32_t depth = mDepth + 1;
    if (depth >= Config::HARD_MAX_DEPTH) {
        return;
    }
    const int32_t threshold = Config::get().getTimerLevel();
    if (threshold == Config::LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    const int32_t idx = emplaceNode(CtxThread::get(), mNodeIdx, name, depth, now);
    if (idx < 0) {
        return;
    }
    try {
        CtxThread::get().openStack.push_back(idx);
    } catch (...) {
        return;
    }
    mSubNodeIdx = idx;
}

void XTimerScoped::sub() noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    (void)closeOpenSub();
}

}  // namespace perf
}  // namespace au