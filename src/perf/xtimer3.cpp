#include "perf/xtimer3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "log/xlogger.h"
#include "sys/xplatform.h"

#if AU_OS_WINDOWS
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace au::perf {

// ===========================================================================
// XPerfContext3
// ===========================================================================

XPerfContext3::XPerfContext3() noexcept = default;

XPerfContext3& XPerfContext3::defaultConfig() noexcept
{
    // Constructed on first use, destroyed after main. Accessing the singleton
    // from XTimer3Scoped destructors running in namespace-scope static cleanup
    // is technically undefined; we mitigate this by making such destructors
    // no-op when the TLS context has already been cleared.
    static XPerfContext3 instance;
    return instance;
}

void XPerfContext3::setRootName(std::string_view name) noexcept
{
    const size_t cap = sizeof(mRootName) - 1;
    const size_t len = std::min(name.size(), cap);
    std::memcpy(mRootName, name.data(), len);
    mRootName[len] = '\0';
    mRootNameLen.store(static_cast<uint32_t>(len), std::memory_order_release);
}

void XPerfContext3::getRootName(char* outBuf, size_t bufSize) const noexcept
{
    if (outBuf == nullptr || bufSize == 0) {
        return;
    }
    const uint32_t len = mRootNameLen.load(std::memory_order_acquire);
    const size_t   cp  = std::min(static_cast<size_t>(len), bufSize - 1);
    std::memcpy(outBuf, mRootName, cp);
    outBuf[cp] = '\0';
}

void XPerfContext3::loadFromSystemProperty(const char* propEnabled, const char* propMode, const char* propTimerLevel,
                                           const char* propTracerLevel) noexcept
{
    if (propEnabled != nullptr) {
        const int val = au::sys::getSystemPropertyValue(propEnabled, isEnabled() ? 1 : 0);
        setEnabled(val != 0);
    }
    if (propMode != nullptr) {
        const int val = au::sys::getSystemPropertyValue(propMode, static_cast<int>(getMode()));
        setMode(val != 0 ? Mode::Debug : Mode::Release);
    }
    if (propTimerLevel != nullptr) {
        const int val = au::sys::getSystemPropertyValue(propTimerLevel, getTimerLevel());
        setTimerLevel(val);
    }
    if (propTracerLevel != nullptr) {
        const int val = au::sys::getSystemPropertyValue(propTracerLevel, getTracerLevel());
        setTracerLevel(val);
    }
}

// ===========================================================================
// Internal tree data structures (file-local)
// ===========================================================================

namespace {

/// One node in the thread-local tree.
struct PerfNode
{
    uint32_t                              nameOffset;  ///< offset into PerfTreeContext::nameArena
    uint32_t                              nameLen;
    int32_t                               parent;       ///< -1 for root-level nodes
    int32_t                               firstChild;   ///< -1 if leaf
    int32_t                               lastChild;    ///< -1 if leaf
    int32_t                               nextSibling;  ///< -1 if last child
    uint32_t                              depth;        ///< 0 means first active node on this thread
    std::chrono::steady_clock::time_point begin;
    float                                 durationMs;  ///< filled on destructor
    bool                                  closed;      ///< true after duration is filled
};

/// Per-thread tree context. Stored as thread_local, never shared.
struct PerfTreeContext
{
    std::vector<PerfNode> pool;
    std::vector<char>     nameArena;

    /// Stack of currently-open nodes (indices into pool). Top = innermost scope.
    std::vector<int32_t> openStack;

    std::thread::id tid;
    bool            initialised{false};
    bool            inFlush{false};
};

constexpr size_t kPoolReserve  = 256;
constexpr size_t kArenaReserve = 8192;
constexpr size_t kMaxNameLen   = 255;  ///< hard ceiling to avoid accidental giant names

/// Get or lazily create the TLS tree context.
PerfTreeContext& tlsTree() noexcept
{
    thread_local PerfTreeContext ctx;
    if (!ctx.initialised) {
        try {
            ctx.pool.reserve(kPoolReserve);
            ctx.nameArena.reserve(kArenaReserve);
            ctx.openStack.reserve(64);
        } catch (...) {
            // tolerate OOM: leave vectors empty, allocations will retry below.
        }
        ctx.tid         = std::this_thread::get_id();
        ctx.initialised = true;
    }
    return ctx;
}

/// Append @p name into the TLS name arena and return (offset, len).
/// Silently truncates names longer than kMaxNameLen.
std::pair<uint32_t, uint32_t> arenaPut(PerfTreeContext& ctx, std::string_view name) noexcept
{
    const uint32_t len    = static_cast<uint32_t>(std::min(name.size(), kMaxNameLen));
    const uint32_t offset = static_cast<uint32_t>(ctx.nameArena.size());

    try {
        ctx.nameArena.insert(ctx.nameArena.end(), name.data(), name.data() + len);
    } catch (...) {
        return {0u, 0u};  // caller must treat (0,0) as empty name
    }
    return {offset, len};
}

/// Convert thread::id to 64-bit for printing.
uint64_t tidHash(std::thread::id id) noexcept { return static_cast<uint64_t>(std::hash<std::thread::id>{}(id)); }

// ---------------------------------------------------------------------------
// Tree printer (ASCII only, no locale-specific chars).
// ---------------------------------------------------------------------------

/// Snapshot of a flushed subtree, used by the aggregate collector.
struct FlushedTree
{
    std::vector<PerfNode> pool;   ///< copy of pool at flush time
    std::vector<char>     arena;  ///< copy of name arena
    std::vector<int32_t>  roots;  ///< indices of top-level nodes in flush order
    uint64_t              tid;
    char                  rootName[64];
};

/// Collect top-level root indices in chronological (construction) order.
std::vector<int32_t> collectRoots(const std::vector<PerfNode>& pool)
{
    std::vector<int32_t> roots;
    roots.reserve(8);
    for (int32_t i = 0; i < static_cast<int32_t>(pool.size()); ++i) {
        if (pool[static_cast<size_t>(i)].parent == -1) {
            roots.push_back(i);
        }
    }
    return roots;
}

/// Compute column width for the name column (ASCII prefix + name) so that
/// duration values line up. Walks the subtree once.
uint32_t computeNameColumnWidth(const std::vector<PerfNode>& pool, const std::vector<char>& arena, int32_t rootIdx,
                                uint32_t rootPrefixLen) noexcept
{
    uint32_t maxWidth = 0;
    // Iterative DFS.
    std::vector<std::pair<int32_t, uint32_t>> stack;
    stack.reserve(32);
    stack.emplace_back(rootIdx, rootPrefixLen);
    while (!stack.empty()) {
        const auto [idx, prefixLen] = stack.back();
        stack.pop_back();
        const PerfNode& n = pool[static_cast<size_t>(idx)];
        const uint32_t  w = prefixLen + n.nameLen;
        if (w > maxWidth) {
            maxWidth = w;
        }
        // child prefix adds 4 chars ("|-- " or "`-- ")
        for (int32_t c = n.firstChild; c != -1; c = pool[static_cast<size_t>(c)].nextSibling) {
            stack.emplace_back(c, prefixLen + 4);
        }
        (void)arena;  // unused here; present for future formatting options
    }
    return maxWidth;
}

/// Print one node line. @p prefix is the ASCII branch drawing already built.
void printNodeLine(const std::vector<PerfNode>& pool, const std::vector<char>& arena, int32_t idx,
                   const std::string& prefix, bool isLast, uint32_t nameColumnWidth) noexcept
{
    const PerfNode& n    = pool[static_cast<size_t>(idx)];
    const char*     name = (n.nameLen == 0) ? "" : (arena.data() + n.nameOffset);

    // Compose the line into a stack buffer to avoid std::string surprises.
    std::array<char, 512> line{};
    const char*           branch    = prefix.empty() ? "" : (isLast ? "`-- " : "|-- ");
    const uint32_t        prefixLen = static_cast<uint32_t>(prefix.size()) + (prefix.empty() ? 0u : 4u);
    int padding = static_cast<int>(nameColumnWidth) - static_cast<int>(prefixLen) - static_cast<int>(n.nameLen);
    if (padding < 0) {
        padding = 0;
    }

    const float ms         = n.closed ? n.durationMs : 0.0f;
    const char* closedMark = n.closed ? "" : " (open)";
    std::snprintf(line.data(), line.size(), "%s%s%.*s%*s : %8.3f ms%s", prefix.c_str(), branch,
                  static_cast<int>(n.nameLen), name, padding, "", ms, closedMark);
    XLOG_I("%s\n", line.data());
}

/// Print the full tree header + every root subtree in this snapshot.
void printTree(const std::vector<PerfNode>& pool, const std::vector<char>& arena, const std::vector<int32_t>& roots,
               uint64_t tid, const char* rootName) noexcept
{
    if (roots.empty()) {
        return;
    }

    // Header.
    XLOG_I("[perf][tid=0x%llx] %s\n", static_cast<unsigned long long>(tid),
           (rootName != nullptr && rootName[0] != '\0') ? rootName : "perf");

    // Compute uniform column width across all roots for visual alignment.
    uint32_t nameCol = 0;
    for (int32_t r : roots) {
        const uint32_t w = computeNameColumnWidth(pool, arena, r, 4u /* "|-- " */);
        if (w > nameCol) {
            nameCol = w;
        }
    }

    for (size_t i = 0; i < roots.size(); ++i) {
        const bool isLast = (i + 1 == roots.size());
        struct Frame
        {
            int32_t     idx;
            std::string prefix;
            bool        isLast;
        };
        std::vector<Frame> dfs;
        dfs.push_back({roots[i], "", isLast});

        while (!dfs.empty()) {
            Frame cur = std::move(dfs.back());
            dfs.pop_back();

            printNodeLine(pool, arena, cur.idx, cur.prefix, cur.isLast, nameCol);

            const PerfNode&      n = pool[static_cast<size_t>(cur.idx)];
            std::vector<int32_t> children;
            for (int32_t c = n.firstChild; c != -1; c = pool[static_cast<size_t>(c)].nextSibling) {
                children.push_back(c);
            }
            const std::string childPrefix = cur.prefix + (cur.isLast ? "    " : "|   ");
            for (size_t k = children.size(); k-- > 0;) {
                const bool last = (k == children.size() - 1);
                dfs.push_back({children[k], childPrefix, last});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Process-global aggregate collector.
// ---------------------------------------------------------------------------

class AggregateCollector
{
public:
    static AggregateCollector& get() noexcept
    {
        static AggregateCollector instance;
        return instance;
    }

    /// Append a snapshot; thread-safe. Returns true if accepted.
    bool append(FlushedTree&& tree) noexcept
    {
        try {
            std::lock_guard<std::mutex> lk(mMutex);
            mTrees.emplace_back(std::move(tree));
            registerAtexitOnce();
            return true;
        } catch (...) {
            return false;
        }
    }

    /// Emit all queued trees and clear the queue.
    void flush() noexcept
    {
        std::vector<FlushedTree> local;
        {
            try {
                std::lock_guard<std::mutex> lk(mMutex);
                local.swap(mTrees);
            } catch (...) {
                return;
            }
        }
        if (local.empty()) {
            return;
        }
        // Stable sort: group by tid so each thread's blocks stay contiguous.
        std::stable_sort(local.begin(), local.end(),
                         [](const FlushedTree& a, const FlushedTree& b) { return a.tid < b.tid; });

        XLOG_I("[perf] ===== aggregate flush: %zu block(s) =====\n", local.size());
        for (const FlushedTree& t : local) {
            printTree(t.pool, t.arena, t.roots, t.tid, t.rootName);
        }
        XLOG_I("[perf] ===== end =====\n");
    }

private:
    AggregateCollector() = default;

    void registerAtexitOnce() noexcept
    {
        if (!mAtexitRegistered.exchange(true, std::memory_order_acq_rel)) {
            std::atexit(&atexitHook);
        }
    }

    static void atexitHook() noexcept { get().flush(); }

    std::mutex               mMutex;
    std::vector<FlushedTree> mTrees;
    std::atomic<bool>        mAtexitRegistered{false};
};

}  // namespace

void XPerfContext3::flushAggregated() noexcept { AggregateCollector::get().flush(); }

// ===========================================================================
// XTimer3 static helpers
// ===========================================================================

void XTimer3::sleepFor(std::chrono::milliseconds ms) noexcept
{
    if (ms.count() <= 0) {
        return;
    }
    std::this_thread::sleep_for(ms);
}

std::string XTimer3::getTimeFormatted(std::string_view fmt) noexcept
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);

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

    // Copy fmt into a NUL-terminated buffer for strftime.
    std::array<char, 128> fmtBuf{};
    const size_t          flen = std::min(fmt.size(), fmtBuf.size() - 1);
    std::memcpy(fmtBuf.data(), fmt.data(), flen);
    fmtBuf[flen] = '\0';

    std::array<char, 128> outBuf{};
    const size_t          written = std::strftime(outBuf.data(), outBuf.size(), fmtBuf.data(), &tm);
    if (written == 0) {
        return {};
    }

    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() -
                    duration_cast<seconds>(now.time_since_epoch()).count() * 1000;

    std::string out(outBuf.data(), written);
    out.push_back('_');
    out.append(std::to_string(ms));
    return out;
}

// ===========================================================================
// XTimer3Scoped
// ===========================================================================

XTimer3Scoped::XTimer3Scoped(std::string_view name, int32_t level) noexcept
{
    mCfg = &XPerfContext3::defaultConfig();
    begin(name, level);
}

XTimer3Scoped::XTimer3Scoped(XPerfContext3& cfg, std::string_view name, int32_t level) noexcept
{
    mCfg = &cfg;
    begin(name, level);
}

void XTimer3Scoped::begin(std::string_view name, int32_t level) noexcept
{
    mBegin = std::chrono::steady_clock::now();
    mName  = name;

    if (mCfg == nullptr || !mCfg->isEnabled()) {
        return;
    }
    if (level > mCfg->getTimerLevel()) {
        return;
    }

    if (mCfg->getMode() == Mode::Release) {
        // Release: activate but keep mNodeIdx = -1 so destructor prints a one-liner.
        mNodeIdx = -2;  // sentinel "active, no tree"
        return;
    }

    // Debug: insert a node into TLS pool (unless depth exceeded or TLS broken).
    PerfTreeContext& ctx = tlsTree();
    if (ctx.inFlush) {
        return;  // a log callback from flush re-entered us; stay silent
    }

    const uint32_t depth  = static_cast<uint32_t>(ctx.openStack.size());
    const uint32_t maxDep = mCfg->getMaxTreeDepth();
    if (depth >= maxDep) {
        mNodeIdx = -2;  // exceed depth: degrade to one-liner in destructor
        return;
    }

    try {
        const int32_t idx = static_cast<int32_t>(ctx.pool.size());
        PerfNode      node{};
        const auto [off, len] = arenaPut(ctx, name);
        node.nameOffset       = off;
        node.nameLen          = len;
        node.parent           = ctx.openStack.empty() ? -1 : ctx.openStack.back();
        node.firstChild       = -1;
        node.lastChild        = -1;
        node.nextSibling      = -1;
        node.depth            = depth;
        node.begin            = mBegin;
        node.durationMs       = 0.0f;
        node.closed           = false;
        ctx.pool.push_back(node);

        if (node.parent == -1) {
            // Root-level: link into previous root's sibling chain (scan linearly;
            // root count is tiny, usually 1).
            int32_t prevRoot = -1;
            for (int32_t i = idx - 1; i >= 0; --i) {
                if (ctx.pool[static_cast<size_t>(i)].parent == -1) {
                    prevRoot = i;
                    break;
                }
            }
            if (prevRoot != -1) {
                ctx.pool[static_cast<size_t>(prevRoot)].nextSibling = idx;
            }
        } else {
            PerfNode& parent = ctx.pool[static_cast<size_t>(node.parent)];
            if (parent.firstChild == -1) {
                parent.firstChild = idx;
            } else {
                ctx.pool[static_cast<size_t>(parent.lastChild)].nextSibling = idx;
            }
            parent.lastChild = idx;
        }

        ctx.openStack.push_back(idx);
        mNodeIdx = idx;
        mDepth   = depth;
        mIsRoot  = (depth == 0);
    } catch (...) {
        mNodeIdx = -2;  // degrade on OOM
    }
}

XTimer3Scoped::~XTimer3Scoped() noexcept
{
    if (mCfg == nullptr || mNodeIdx == -1) {
        return;  // scope was not activated
    }

    const auto  now = std::chrono::steady_clock::now();
    const float ms  = std::chrono::duration<float, std::milli>(now - mBegin).count();

    // Release-mode or degraded path: one-liner log, no tree work.
    if (mNodeIdx == -2) {
        const int nameLen = static_cast<int>(std::min(mName.size(), static_cast<size_t>(200)));
        XLOG_I("[perf] %.*s : %.3f ms\n", nameLen, mName.data(), ms);
        return;
    }

    // Debug path: close node and, if this is the outermost active scope, flush.
    PerfTreeContext& ctx = tlsTree();
    if (ctx.inFlush) {
        return;
    }

    if (mNodeIdx >= 0 && mNodeIdx < static_cast<int32_t>(ctx.pool.size())) {
        PerfNode& node  = ctx.pool[static_cast<size_t>(mNodeIdx)];
        node.durationMs = ms;
        node.closed     = true;
    }
    if (!ctx.openStack.empty() && ctx.openStack.back() == mNodeIdx) {
        ctx.openStack.pop_back();
    }

    if (!mIsRoot || !ctx.openStack.empty()) {
        return;  // not the outermost active scope yet
    }

    // Outermost scope: flush this thread's pool.
    ctx.inFlush = true;

    const bool aggregate = mCfg->isAggregateMode();
    char       rootName[64];
    mCfg->getRootName(rootName, sizeof(rootName));

    try {
        if (aggregate) {
            FlushedTree snap;
            snap.pool  = ctx.pool;
            snap.arena = ctx.nameArena;
            snap.roots = collectRoots(ctx.pool);
            snap.tid   = tidHash(ctx.tid);
            std::memcpy(snap.rootName, rootName, sizeof(snap.rootName));
            AggregateCollector::get().append(std::move(snap));
        } else {
            const auto roots = collectRoots(ctx.pool);
            printTree(ctx.pool, ctx.nameArena, roots, tidHash(ctx.tid), rootName);
        }
    } catch (...) {
        // swallow; perf must never kill the host.
    }

    ctx.pool.clear();
    ctx.nameArena.clear();
    ctx.openStack.clear();
    ctx.inFlush = false;
}

void XTimer3Scoped::sub(std::string_view name) noexcept
{
    if (mCfg == nullptr || mNodeIdx < 0) {
        return;  // inactive or release/degraded — sub() is a tree-only feature
    }

    PerfTreeContext& ctx = tlsTree();
    if (ctx.inFlush) {
        return;
    }

    // Close previous sub (if any) by popping it from the open stack.
    if (mSubNodeIdx >= 0 && mSubNodeIdx < static_cast<int32_t>(ctx.pool.size())) {
        PerfNode& prev = ctx.pool[static_cast<size_t>(mSubNodeIdx)];
        if (!prev.closed) {
            const auto now  = std::chrono::steady_clock::now();
            prev.durationMs = std::chrono::duration<float, std::milli>(now - prev.begin).count();
            prev.closed     = true;
        }
        if (!ctx.openStack.empty() && ctx.openStack.back() == mSubNodeIdx) {
            ctx.openStack.pop_back();
        }
        mSubNodeIdx = -1;
    }

    // Open a new sub as a child of this scope's node.
    const uint32_t maxDep = mCfg->getMaxTreeDepth();
    const uint32_t depth  = mDepth + 1;
    if (depth >= maxDep) {
        return;
    }

    try {
        const int32_t idx = static_cast<int32_t>(ctx.pool.size());
        PerfNode      node{};
        const auto [off, len] = arenaPut(ctx, name);
        node.nameOffset       = off;
        node.nameLen          = len;
        node.parent           = mNodeIdx;
        node.firstChild       = -1;
        node.lastChild        = -1;
        node.nextSibling      = -1;
        node.depth            = depth;
        node.begin            = std::chrono::steady_clock::now();
        node.durationMs       = 0.0f;
        node.closed           = false;
        ctx.pool.push_back(node);

        PerfNode& parent = ctx.pool[static_cast<size_t>(mNodeIdx)];
        if (parent.firstChild == -1) {
            parent.firstChild = idx;
        } else {
            ctx.pool[static_cast<size_t>(parent.lastChild)].nextSibling = idx;
        }
        parent.lastChild = idx;

        ctx.openStack.push_back(idx);
        mSubNodeIdx = idx;
    } catch (...) {
        // ignore
    }
}

void XTimer3Scoped::sub() noexcept
{
    if (mCfg == nullptr || mNodeIdx < 0 || mSubNodeIdx < 0) {
        return;
    }

    PerfTreeContext& ctx = tlsTree();
    if (ctx.inFlush) {
        return;
    }

    if (mSubNodeIdx < static_cast<int32_t>(ctx.pool.size())) {
        PerfNode& prev = ctx.pool[static_cast<size_t>(mSubNodeIdx)];
        if (!prev.closed) {
            const auto now  = std::chrono::steady_clock::now();
            prev.durationMs = std::chrono::duration<float, std::milli>(now - prev.begin).count();
            prev.closed     = true;
        }
    }
    if (!ctx.openStack.empty() && ctx.openStack.back() == mSubNodeIdx) {
        ctx.openStack.pop_back();
    }
    mSubNodeIdx = -1;
}

}  // namespace au::perf