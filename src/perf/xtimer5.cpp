#include "perf/xtimer5.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log/xlogger.h"
#include "sys/xplatform.h"

#if AU_OS_WINDOWS
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace au {
namespace perf {

// ===========================================================================
//  Internal node / TLS data structures (file-local)
// ===========================================================================

namespace {

/// One node in the per-thread tree. ~64 bytes on x86_64.
struct PerfNode
{
    uint32_t                              nameOffset;
    uint32_t                              nameLen;
    int32_t                               parent;
    int32_t                               firstChild;
    int32_t                               lastChild;
    int32_t                               nextSibling;
    uint32_t                              depth;
    uint32_t                              flags;  ///< bit0 closed, bit1 truncated
    std::chrono::steady_clock::time_point begin;
    uint64_t                              durationNs;
};

constexpr uint32_t kFlagClosed    = 1u << 0;
constexpr uint32_t kFlagTruncated = 1u << 1;

struct PerCtxTree
{
    std::vector<PerfNode> pool;
    std::vector<char>     nameArena;
    std::vector<int32_t>  openStack;
};

constexpr std::size_t kPoolReserve   = 256;
constexpr std::size_t kArenaReserve  = 8192;
constexpr std::size_t kStackReserve  = 64;
constexpr std::size_t kMaxNameLen    = 1023;
constexpr uint32_t    kPrintNameClip = 256;

struct PerfThreadCtx
{
    PerCtxTree      tree;
    std::thread::id tid;
    bool            inited{false};
    bool            inFlush{false};
};

PerfThreadCtx& tlsCtx() noexcept
{
    thread_local PerfThreadCtx ctx;
    if (!ctx.inited) {
        ctx.tid    = std::this_thread::get_id();
        ctx.inited = true;
    }
    return ctx;
}

PerCtxTree& tlsTree() noexcept { return tlsCtx().tree; }

/// Release-mode per-thread depth counter.
thread_local uint32_t gTimerReleaseDepth = 0;

uint64_t tidHash(std::thread::id id) noexcept { return static_cast<uint64_t>(std::hash<std::thread::id>{}(id)); }

}  // anonymous namespace

// ===========================================================================
//  Aggregate buffer (global, single)
// ===========================================================================

struct FlushedTree
{
    std::vector<PerfNode> pool;
    std::vector<char>     arena;
    std::vector<int32_t>  roots;
    uint64_t              tid;
    char                  rootName[64];
};

namespace {

void emitFormatted(const char* fmt, ...) noexcept;
void printTree(const std::vector<PerfNode>& pool, const std::vector<char>& arena, const std::vector<int32_t>& roots,
               uint64_t tid, const char* rootName) noexcept;

struct AggregateData
{
    std::mutex               mMutex;
    std::vector<FlushedTree> mTrees;
};

AggregateData& gAggData() noexcept
{
    static AggregateData* const instance = new AggregateData();
    return *instance;
}

std::atomic<bool> gSafetyNetDone{false};
std::atomic<bool> gShuttingDown{false};

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
        emitFormatted("[perf5] ===== safety-net flush: %zu block(s) =====", local.size());
        for (const FlushedTree& t : local) {
            printTree(t.pool, t.arena, t.roots, t.tid, t.rootName);
        }
        emitFormatted("[perf5] ===== end =====");
    });
}

}  // anonymous namespace

// ===========================================================================
//  PerfConfig — Meyers singleton
// ===========================================================================

void PerfConfig::setEnabled(bool on) noexcept { mEnabled.store(on, std::memory_order_relaxed); }
bool PerfConfig::isEnabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }

void  PerfConfig::setMode(Mode5 mode) noexcept { mMode.store(mode, std::memory_order_relaxed); }
Mode5 PerfConfig::getMode() const noexcept { return mMode.load(std::memory_order_relaxed); }

void    PerfConfig::setTimerLevel(int32_t threshold) noexcept { mTimerLevel.store(threshold, std::memory_order_relaxed); }
int32_t PerfConfig::getTimerLevel() const noexcept { return mTimerLevel.load(std::memory_order_relaxed); }

void    PerfConfig::setTracerLevel(int32_t threshold) noexcept { mTracerLevel.store(threshold, std::memory_order_relaxed); }
int32_t PerfConfig::getTracerLevel() const noexcept { return mTracerLevel.load(std::memory_order_relaxed); }

void PerfConfig::setRootName(const std::string& name) noexcept
{
    const std::size_t cap = sizeof(mRootName) - 1;
    const std::size_t cp  = std::min(name.size(), cap);
    if (cp > 0u) {
        std::memcpy(mRootName, name.data(), cp);
    }
    mRootName[cp] = '\0';
    mRootNameLen.store(static_cast<uint32_t>(cp), std::memory_order_release);
}

void PerfConfig::getRootName(char* outBuf, std::size_t bufSize) const noexcept
{
    if (outBuf == nullptr || bufSize == 0) {
        return;
    }
    const uint32_t    len = mRootNameLen.load(std::memory_order_acquire);
    const std::size_t cp  = std::min(static_cast<std::size_t>(len), bufSize - 1);
    std::memcpy(outBuf, mRootName, cp);
    outBuf[cp] = '\0';
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

    emitFormatted("[perf5] ===== aggregate flush: %zu block(s) =====", local.size());
    for (const FlushedTree& t : local) {
        printTree(t.pool, t.arena, t.roots, t.tid, t.rootName);
    }
    emitFormatted("[perf5] ===== end =====");
}

void PerfConfig::loadFromSystemProperty(const std::string& propEnabled, const std::string& propMode,
                                        const std::string& propTimerLevel,
                                        const std::string& propTracerLevel) noexcept
{
    if (!propEnabled.empty()) {
        const int v = au::sys::getSystemPropertyValue(propEnabled.c_str(), isEnabled() ? 1 : 0);
        setEnabled(v != 0);
    }
    if (!propMode.empty()) {
        const int v = au::sys::getSystemPropertyValue(propMode.c_str(), static_cast<int>(getMode()));
        setMode(v != 0 ? Mode5::Debug : Mode5::Release);
    }
    if (!propTimerLevel.empty()) {
        const int v = au::sys::getSystemPropertyValue(propTimerLevel.c_str(), getTimerLevel());
        setTimerLevel(v);
    }
    if (!propTracerLevel.empty()) {
        const int v = au::sys::getSystemPropertyValue(propTracerLevel.c_str(), getTracerLevel());
        setTracerLevel(v);
    }
}

// ===========================================================================
//  Output, printer, TLS helpers (file-local)
// ===========================================================================

namespace {

void emitFormatted(const char* fmt, ...) noexcept
{
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (static_cast<std::size_t>(n) >= sizeof(buf)) {
        buf[sizeof(buf) - 1] = '\0';
    }
    XLOG_I("%s", buf);
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

void printNodeLine(const std::vector<PerfNode>& pool, const std::vector<char>& arena, int32_t idx,
                   const std::string& prefix, bool isLast) noexcept
{
    const PerfNode& n    = pool[static_cast<std::size_t>(idx)];
    const char*     name = (n.nameLen == 0) ? "" : (arena.data() + n.nameOffset);

    const char* branch = prefix.empty() ? "" : (isLast ? "`-- " : "|-- ");

    const uint32_t printLen  = (n.nameLen > kPrintNameClip) ? kPrintNameClip : n.nameLen;
    const bool     printClip = (n.nameLen > kPrintNameClip);
    const bool     truncFlag = (n.flags & kFlagTruncated) != 0u;

    const float ms = (n.flags & kFlagClosed) ? static_cast<float>(static_cast<double>(n.durationNs) / 1.0e6) : 0.0f;
    const char* openMark  = (n.flags & kFlagClosed) ? "" : " (open)";
    const char* truncMark = (truncFlag || printClip) ? " (truncated)" : "";

    emitFormatted("%s%s%.*s: %.3f ms%s%s", prefix.c_str(), branch, static_cast<int>(printLen), name, ms, openMark,
                  truncMark);
}

void printTree(const std::vector<PerfNode>& pool, const std::vector<char>& arena, const std::vector<int32_t>& roots,
               uint64_t tid, const char* rootName) noexcept
{
    if (roots.empty()) {
        return;
    }

    emitFormatted("[perf5][tid=0x%llx] %s", static_cast<unsigned long long>(tid),
                  (rootName != nullptr && rootName[0] != '\0') ? rootName : "perf");

    for (std::size_t i = 0; i < roots.size(); ++i) {
        const bool rootIsLast = (i + 1 == roots.size());

        struct Frame
        {
            int32_t     idx;
            std::string prefix;
            bool        isLast;
        };
        std::vector<Frame> dfs;
        dfs.push_back({roots[i], std::string(), rootIsLast});

        while (!dfs.empty()) {
            Frame cur = std::move(dfs.back());
            dfs.pop_back();

            printNodeLine(pool, arena, cur.idx, cur.prefix, cur.isLast);

            const PerfNode&      n = pool[static_cast<std::size_t>(cur.idx)];
            std::vector<int32_t> children;
            for (int32_t c = n.firstChild; c != -1; c = pool[static_cast<std::size_t>(c)].nextSibling) {
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

struct ArenaPutResult
{
    uint32_t offset;
    uint32_t len;
    bool     truncated;
};

ArenaPutResult arenaPut(PerCtxTree& tree, const char* name, std::size_t inLen) noexcept
{
    const bool     truncated = inLen > kMaxNameLen;
    const uint32_t len       = static_cast<uint32_t>(truncated ? kMaxNameLen : inLen);
    const uint32_t offset    = static_cast<uint32_t>(tree.nameArena.size());
    try {
        tree.nameArena.insert(tree.nameArena.end(), name, name + len);
    } catch (...) {
        return {0u, 0u, false};
    }
    return {offset, len, truncated};
}

int32_t appendNodeUnsafe(PerCtxTree& tree, int32_t parent, const char* name, std::size_t nameLen, uint32_t depth,
                         std::chrono::steady_clock::time_point beginTp) noexcept
{
    try {
        const int32_t        idx      = static_cast<int32_t>(tree.pool.size());
        const ArenaPutResult arenaRet = arenaPut(tree, name, nameLen);

        PerfNode node{};
        node.nameOffset  = arenaRet.offset;
        node.nameLen     = arenaRet.len;
        node.parent      = parent;
        node.firstChild  = -1;
        node.lastChild   = -1;
        node.nextSibling = -1;
        node.depth       = depth;
        node.flags       = arenaRet.truncated ? kFlagTruncated : 0u;
        node.begin       = beginTp;
        node.durationNs  = 0;
        tree.pool.push_back(node);

        if (parent == -1) {
            for (int32_t i = idx - 1; i >= 0; --i) {
                if (tree.pool[static_cast<std::size_t>(i)].parent == -1) {
                    tree.pool[static_cast<std::size_t>(i)].nextSibling = idx;
                    break;
                }
            }
        } else {
            PerfNode& parentNode = tree.pool[static_cast<std::size_t>(parent)];
            if (parentNode.firstChild == -1) {
                parentNode.firstChild = idx;
            } else {
                tree.pool[static_cast<std::size_t>(parentNode.lastChild)].nextSibling = idx;
            }
            parentNode.lastChild = idx;
        }
        return idx;
    } catch (...) {
        return -1;
    }
}

}  // anonymous namespace

// ===========================================================================
//  XTimer5 — bare stopwatch + static helpers
// ===========================================================================

void XTimer5::sleepFor(int64_t ms) noexcept
{
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string XTimer5::getTimeFormatted(const std::string& fmt) noexcept
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

float XTimer5::elapsedMs() const noexcept
{
    return std::chrono::duration<float, std::milli>(Clock::now() - mBegin).count();
}

// ===========================================================================
//  XTimer5Scoped
// ===========================================================================

XTimer5Scoped::XTimer5Scoped(const std::string& name) noexcept { begin(name); }

void XTimer5Scoped::begin(const std::string& name) noexcept
{
    mNodeIdx          = -1;
    mSubNodeIdx       = -1;
    mDepth            = 0;
    mIsRoot           = false;
    mNameLen          = 0;
    mNameInline[0]    = '\0';
    mSubNameLen       = 0;
    mSubNameInline[0] = '\0';
    mBegin            = std::chrono::steady_clock::now();
    mSubBegin         = mBegin;

    if (!name.empty()) {
        const std::size_t cp = std::min(name.size(), kInlineNameCap - 1);
        std::memcpy(mNameInline, name.data(), cp);
        mNameInline[cp] = '\0';
        mNameLen        = static_cast<uint32_t>(cp);
    }

    PerfConfig& cfg = PerfConfig::get();
    if (!cfg.isEnabled()) {
        return;
    }

    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    const Mode5 mode = cfg.getMode();

    const uint32_t depth =
        (mode == Mode5::Release) ? gTimerReleaseDepth : static_cast<uint32_t>(tlsTree().openStack.size());

    if (depth >= kHardMaxDepth5) {
        return;
    }

    const int32_t threshold = cfg.getTimerLevel();
    if (threshold == kPerfLevelOff5 || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    if (mode == Mode5::Release) {
        mNodeIdx = -2;
        mDepth   = depth;
        ++gTimerReleaseDepth;
        return;
    }

    // -- Debug path --
    PerCtxTree&   tree   = tlsTree();
    const int32_t parent = tree.openStack.empty() ? -1 : tree.openStack.back();
    const int32_t idx    = appendNodeUnsafe(tree, parent, name.data(), name.size(), depth, mBegin);
    if (idx < 0) {
        mNodeIdx = -2;
        mDepth   = depth;
        return;
    }
    try {
        tree.openStack.push_back(idx);
    } catch (...) {
        mNodeIdx = -2;
        mDepth   = depth;
        return;
    }
    mNodeIdx = idx;
    mDepth   = depth;
    mIsRoot  = (depth == 0);
}

XTimer5Scoped::~XTimer5Scoped() noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    const auto     now = std::chrono::steady_clock::now();
    const uint64_t ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mBegin).count());
    const float msF = static_cast<float>(static_cast<double>(ns) / 1.0e6);

    // -- Release / degraded path: one-liner --
    if (mNodeIdx == -2) {
        if (mSubNameLen > 0u) {
            const uint64_t subNs =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mSubBegin).count());
            const float subMs = static_cast<float>(static_cast<double>(subNs) / 1.0e6);
            emitFormatted("[perf5] %.*s: %.3f ms", static_cast<int>(mSubNameLen), mSubNameInline, subMs);
        }
        emitFormatted("[perf5] %.*s: %.3f ms", static_cast<int>(mNameLen), mNameInline, msF);
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
    PerCtxTree& tree = tlsTree();

    if (mNodeIdx >= 0 && mNodeIdx < static_cast<int32_t>(tree.pool.size())) {
        PerfNode& node  = tree.pool[static_cast<std::size_t>(mNodeIdx)];
        node.durationNs = ns;
        node.flags |= kFlagClosed;
    }
    if (!tree.openStack.empty() && tree.openStack.back() == mNodeIdx) {
        tree.openStack.pop_back();
    }

    if (!mIsRoot || !tree.openStack.empty()) {
        return;
    }

    // -- Outermost scope: flush this thread's tree --
    tls.inFlush = true;

    PerfConfig& cfg = PerfConfig::get();
    const bool  aggregate = cfg.isAggregateMode();
    char        rootName[64];
    cfg.getRootName(rootName, sizeof(rootName));

    try {
        if (aggregate) {
            FlushedTree snap;
            snap.pool  = tree.pool;
            snap.arena = tree.nameArena;
            snap.roots = collectRoots(tree.pool);
            snap.tid   = tidHash(tls.tid);
            std::memcpy(snap.rootName, rootName, sizeof(snap.rootName));

            std::lock_guard<std::mutex> lk(gAggData().mMutex);
            gAggData().mTrees.emplace_back(std::move(snap));
            registerSafetyNetOnce();
        } else {
            const auto roots = collectRoots(tree.pool);
            printTree(tree.pool, tree.nameArena, roots, tidHash(tls.tid), rootName);
        }
    } catch (...) {
    }

    tree.pool.clear();
    tree.nameArena.clear();
    tree.openStack.clear();
    tls.inFlush = false;
}

std::chrono::steady_clock::time_point XTimer5Scoped::closeOpenSub() noexcept
{
    const auto now = std::chrono::steady_clock::now();

    // ── Release path ──
    if (mNodeIdx == -2) {
        if (mSubNameLen > 0u) {
            const uint64_t ns =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mSubBegin).count());
            const float ms = static_cast<float>(static_cast<double>(ns) / 1.0e6);
            emitFormatted("[perf5] %.*s: %.3f ms", static_cast<int>(mSubNameLen), mSubNameInline, ms);
            mSubNameLen       = 0;
            mSubNameInline[0] = '\0';
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
    PerCtxTree& tree = tlsTree();

    if (mSubNodeIdx < static_cast<int32_t>(tree.pool.size())) {
        PerfNode& prev = tree.pool[static_cast<std::size_t>(mSubNodeIdx)];
        if ((prev.flags & kFlagClosed) == 0) {
            prev.durationNs =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev.begin).count());
            prev.flags |= kFlagClosed;
        }
    }
    if (!tree.openStack.empty() && tree.openStack.back() == mSubNodeIdx) {
        tree.openStack.pop_back();
    }
    mSubNodeIdx = -1;
    return now;
}

void XTimer5Scoped::sub(const std::string& name) noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    const auto now = closeOpenSub();

    // ── Release path: arm the new inline segment ──
    if (mNodeIdx == -2) {
        const std::size_t cp = std::min(name.size(), kInlineNameCap - 1);
        if (cp > 0u) {
            std::memcpy(mSubNameInline, name.data(), cp);
        }
        mSubNameInline[cp] = '\0';
        mSubNameLen        = static_cast<uint32_t>(cp);
        mSubBegin          = now;
        return;
    }

    // ── Debug path: open a new sub-node under the outer scope ──
    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    const uint32_t depth = mDepth + 1;
    if (depth >= kHardMaxDepth5) {
        return;
    }
    PerfConfig& cfg      = PerfConfig::get();
    const int32_t threshold = cfg.getTimerLevel();
    if (threshold == kPerfLevelOff5 || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    PerCtxTree&   tree = tlsTree();
    const int32_t idx  = appendNodeUnsafe(tree, mNodeIdx, name.data(), name.size(), depth, now);
    if (idx < 0) {
        return;
    }
    try {
        tree.openStack.push_back(idx);
    } catch (...) {
        return;
    }
    mSubNodeIdx = idx;
}

void XTimer5Scoped::sub() noexcept
{
    if (mNodeIdx == -1) {
        return;
    }

    (void)closeOpenSub();
}

}  // namespace perf
}  // namespace au
