#include "perf/xtimer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include "log/xlogger.h"

#if AU_OS_WINDOWS
#include <windows.h>
#else
#include <time.h>
#endif

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
struct PerfNode
{
    uint32_t                              nameOffset;
    uint32_t                              nameLen;
    int32_t                               parent;
    int32_t                               firstChild;
    int32_t                               lastChild;
    int32_t                               nextSibling;
    uint32_t                              depth;
    float                                 durationMs;  ///< -1.0f = open sentinel
    std::chrono::steady_clock::time_point begin;
};

struct PerfThreadCtx
{
    std::vector<PerfNode> pool;
    std::vector<char>     nameArena;
    std::vector<int32_t>  openStack;

    std::thread::id tid;
    bool            inited{false};
    bool            inFlush{false};
};

struct FlushedTree
{
    std::vector<PerfNode> pool;
    std::vector<char>     nameArena;
    std::vector<int32_t>  roots;
    uint64_t              tid;
    std::string           rootName;
};

// Intentional-leak singleton: the atexit safety-net fires before static
// destructors, so AggregateData's mutex and vector must remain valid then.
struct AggregateData
{
    std::mutex               mMutex;
    std::vector<FlushedTree> mTrees;
};

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------

std::mutex            gRootNameMutex;
thread_local uint32_t gTimerReleaseDepth = 0;  ///< Release-mode per-thread depth counter.
std::atomic<bool>     gSafetyNetDone{false};
std::atomic<bool>     gShuttingDown{false};

// ---------------------------------------------------------------------------
//  Functions
// ---------------------------------------------------------------------------

PerfThreadCtx& tlsCtx() noexcept
{
    thread_local PerfThreadCtx ctx;
    if (!ctx.inited) {
        ctx.tid    = std::this_thread::get_id();
        ctx.inited = true;
        ctx.pool.reserve(128);
        ctx.nameArena.reserve(2048);
        ctx.openStack.reserve(16);
    }
    return ctx;
}

uint64_t tidHash(std::thread::id id) noexcept { return static_cast<uint64_t>(std::hash<std::thread::id>{}(id)); }

int32_t emplaceNode(PerfThreadCtx& ctx, int32_t parent, const std::string& name, uint32_t depth,
                    std::chrono::steady_clock::time_point beginTp) noexcept
{
    try {
        const int32_t idx = static_cast<int32_t>(ctx.pool.size());

        PerfNode node{};
        node.nameOffset  = static_cast<uint32_t>(ctx.nameArena.size());
        node.nameLen     = static_cast<uint32_t>(name.size());
        node.parent      = parent;
        node.firstChild  = -1;
        node.lastChild   = -1;
        node.nextSibling = -1;
        node.depth       = depth;
        node.durationMs  = -1.0f;  // open sentinel
        node.begin       = beginTp;
        ctx.pool.push_back(node);
        ctx.nameArena.insert(ctx.nameArena.end(), name.begin(), name.end());

        // Root nodes are collected lazily via collectRoots() at flush time;
        // their nextSibling field is never traversed, so we only maintain
        // the sibling chain for non-root nodes.
        if (parent != -1) {
            PerfNode& parentNode = ctx.pool[static_cast<std::size_t>(parent)];
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

std::vector<int32_t> collectRoots(const std::vector<PerfNode>& pool)
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

void printNodeLine(const FlushedTree& t, int32_t idx, const std::string& prefix, bool isLast) noexcept
{
    const PerfNode& n    = t.pool[static_cast<std::size_t>(idx)];
    const char*     name = (n.nameLen == 0) ? "" : (t.nameArena.data() + n.nameOffset);

    const bool  closed   = (n.durationMs >= 0.0f);
    const char* branch   = prefix.empty() ? "" : (isLast ? "`-- " : "|-- ");
    const float ms       = closed ? n.durationMs : 0.0f;
    const char* openMark = closed ? "" : " (open)";

    XLOG_I("%s%s%.*s: %.3f ms%s\n", prefix.c_str(), branch, static_cast<int>(n.nameLen), name, ms, openMark);
}

void printTree(const FlushedTree& t) noexcept
{
    if (t.roots.empty()) {
        return;
    }

    XLOG_I("[perf][tid=0x%llx] %s\n", static_cast<unsigned long long>(t.tid),
           t.rootName.empty() ? "perf" : t.rootName.c_str());

    for (std::size_t i = 0; i < t.roots.size(); ++i) {
        const bool rootIsLast = (i + 1 == t.roots.size());

        struct Frame
        {
            int32_t     idx;
            std::string prefix;
            bool        isLast;
        };
        std::vector<Frame> dfs;
        dfs.push_back({t.roots[i], std::string(), rootIsLast});

        while (!dfs.empty()) {
            Frame cur = std::move(dfs.back());
            dfs.pop_back();

            printNodeLine(t, cur.idx, cur.prefix, cur.isLast);

            const PerfNode&      n = t.pool[static_cast<std::size_t>(cur.idx)];
            std::vector<int32_t> children;
            for (int32_t c = n.firstChild; c != -1; c = t.pool[static_cast<std::size_t>(c)].nextSibling) {
                children.push_back(c);
            }
            const std::string childPrefix = cur.prefix + (cur.isLast ? "    " : "|   ");
            for (std::size_t k = children.size(); k-- > 0;) {
                const bool last = (k == children.size() - 1);
                dfs.push_back({children[k], childPrefix, last});
            }
        }
    }
}

AggregateData& gAggData() noexcept
{
    static AggregateData* const instance = new AggregateData();
    return *instance;
}

void registerSafetyNetOnce() noexcept
{
    if (gSafetyNetDone.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::atexit([]() noexcept {
        if (gShuttingDown.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::vector<FlushedTree> local;
        try {
            std::lock_guard<std::mutex> lk(gAggData().mMutex);
            local.swap(gAggData().mTrees);
        } catch (...) {
            return;
        }
        if (local.empty()) {
            return;
        }
        std::stable_sort(local.begin(), local.end(),
                         [](const FlushedTree& a, const FlushedTree& b) { return a.tid < b.tid; });
        XLOG_I("[perf] ===== safety-net flush: %zu block(s) =====\n", local.size());
        for (const FlushedTree& t : local) {
            printTree(t);
        }
        XLOG_I("[perf] ===== end =====\n");
    });
}

}  // anonymous namespace

// ===========================================================================
//  PerfConfig — Meyers singleton
// ===========================================================================

void PerfConfig::setEnabled(bool on) noexcept { mEnabled.store(on, std::memory_order_relaxed); }

bool PerfConfig::isEnabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }

void PerfConfig::setDebugMode(bool on) noexcept { mDebugMode.store(on, std::memory_order_relaxed); }

bool PerfConfig::isDebugMode() const noexcept { return mDebugMode.load(std::memory_order_relaxed); }

void PerfConfig::setTimerLevel(int32_t threshold) noexcept { mTimerLevel.store(threshold, std::memory_order_relaxed); }

int32_t PerfConfig::getTimerLevel() const noexcept { return mTimerLevel.load(std::memory_order_relaxed); }

void PerfConfig::setTracerLevel(int32_t threshold) noexcept
{
    mTracerLevel.store(threshold, std::memory_order_relaxed);
}

int32_t PerfConfig::getTracerLevel() const noexcept { return mTracerLevel.load(std::memory_order_relaxed); }

void PerfConfig::setRootName(const std::string& name) noexcept
{
    std::lock_guard<std::mutex> lk(gRootNameMutex);
    mRootName = name;
}

std::string PerfConfig::getRootName() const noexcept
{
    std::lock_guard<std::mutex> lk(gRootNameMutex);
    return mRootName;
}

void PerfConfig::setAggregateMode(bool on) noexcept { mAggregate.store(on, std::memory_order_relaxed); }

bool PerfConfig::isAggregateMode() const noexcept { return mAggregate.load(std::memory_order_relaxed); }

void PerfConfig::flushAggregated() noexcept
{
    std::vector<FlushedTree> local;
    try {
        std::lock_guard<std::mutex> lk(gAggData().mMutex);
        local.swap(gAggData().mTrees);
    } catch (...) {
        return;
    }
    if (local.empty()) {
        return;
    }
    std::stable_sort(local.begin(), local.end(),
                     [](const FlushedTree& a, const FlushedTree& b) { return a.tid < b.tid; });
    XLOG_I("[perf] ===== aggregate flush: %zu block(s) =====\n", local.size());
    for (const FlushedTree& t : local) {
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
    PerfConfig& cfg = PerfConfig::get();
    if (!cfg.isEnabled()) {
        return;
    }

    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    const bool     debugMode = cfg.isDebugMode();
    const uint32_t depth     = debugMode ? static_cast<uint32_t>(tls.openStack.size()) : gTimerReleaseDepth;

    if (depth >= PerfConfig::HARD_MAX_DEPTH) {
        return;
    }

    const int32_t threshold = cfg.getTimerLevel();
    if (threshold == PerfConfig::LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    if (!debugMode) {
        mNodeIdx = -2;
        mDepth   = depth;
        ++gTimerReleaseDepth;
        return;
    }

    // -- Debug path --
    const int32_t parent = tls.openStack.empty() ? -1 : tls.openStack.back();
    const int32_t idx    = emplaceNode(tls, parent, mName, depth, mBegin);
    if (idx < 0) {
        mNodeIdx = -2;
        mDepth   = depth;
        return;
    }
    try {
        tls.openStack.push_back(idx);
    } catch (...) {
        mNodeIdx = -2;
        mDepth   = depth;
        return;
    }
    mNodeIdx = idx;
    mDepth   = depth;
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
        if (gTimerReleaseDepth > 0u) {
            --gTimerReleaseDepth;
        }
        return;
    }

    // -- Debug path --
    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    if (mNodeIdx >= 0 && mNodeIdx < static_cast<int32_t>(tls.pool.size())) {
        tls.pool[static_cast<std::size_t>(mNodeIdx)].durationMs = msF;
    }
    if (!tls.openStack.empty() && tls.openStack.back() == mNodeIdx) {
        tls.openStack.pop_back();
    }

    if (!mIsRoot || !tls.openStack.empty()) {
        return;
    }

    // -- Outermost scope: flush this thread's tree --
    tls.inFlush = true;

    PerfConfig& cfg      = PerfConfig::get();
    std::string rootName = cfg.getRootName();

    try {
        // Build a snapshot by moving TLS data — O(1), no copies.
        FlushedTree snap;
        snap.pool      = std::move(tls.pool);
        snap.nameArena = std::move(tls.nameArena);
        snap.roots     = collectRoots(snap.pool);
        snap.tid       = tidHash(tls.tid);
        snap.rootName  = std::move(rootName);

        if (cfg.isAggregateMode()) {
            std::lock_guard<std::mutex> lk(gAggData().mMutex);
            gAggData().mTrees.emplace_back(std::move(snap));
            registerSafetyNetOnce();
        } else {
            printTree(snap);
        }
    } catch (...) {
    }

    tls.pool.clear();
    tls.nameArena.clear();
    tls.openStack.clear();
    tls.inFlush = false;
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
    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return now;
    }

    if (mSubNodeIdx < static_cast<int32_t>(tls.pool.size())) {
        PerfNode& prev = tls.pool[static_cast<std::size_t>(mSubNodeIdx)];
        // Guard against double-close: only write if still open.
        if (prev.durationMs < 0.0f) {
            prev.durationMs = std::chrono::duration<float, std::milli>(now - prev.begin).count();
        }
    }
    if (!tls.openStack.empty() && tls.openStack.back() == mSubNodeIdx) {
        tls.openStack.pop_back();
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
    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    const uint32_t depth = mDepth + 1;
    if (depth >= PerfConfig::HARD_MAX_DEPTH) {
        return;
    }
    PerfConfig&   cfg       = PerfConfig::get();
    const int32_t threshold = cfg.getTimerLevel();
    if (threshold == PerfConfig::LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    const int32_t idx = emplaceNode(tls, mNodeIdx, name, depth, now);
    if (idx < 0) {
        return;
    }
    try {
        tls.openStack.push_back(idx);
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