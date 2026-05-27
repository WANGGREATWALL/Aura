#include "perf/xtimer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
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

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr uint32_t kScopeMagic = 0x41555046u;  // AUPF
constexpr uint16_t kScopeAbi   = 1;
constexpr uint8_t  kKindTimer  = 1;

constexpr uint8_t kFlagActive       = 1u << 0;
constexpr uint8_t kFlagDebug        = 1u << 1;
constexpr uint8_t kFlagRoot         = 1u << 2;
constexpr uint8_t kFlagRelease      = 1u << 3;
constexpr uint8_t kFlagReleaseDepth = 1u << 4;

struct ScopeState
{
    uint32_t magic{kScopeMagic};
    uint16_t abi{kScopeAbi};
    uint8_t  kind{kKindTimer};
    uint8_t  flags{0};

    int32_t  nodeIdx{-1};
    int32_t  subNodeIdx{-1};
    int32_t  releaseIdx{-1};
    uint32_t depth{0};

    TimePoint begin{};
    TimePoint subBegin{};

    uint64_t reserved0{0};
    uint64_t reserved1{0};
};

static_assert(sizeof(ScopeState) <= sizeof(PerfScope), "PerfScope is too small for timer state");
static_assert(alignof(ScopeState) <= alignof(PerfScope), "PerfScope alignment is too small");

ScopeState& scopeState(PerfScope* scope) noexcept { return *reinterpret_cast<ScopeState*>(scope->opaque); }

ScopeState& initScopeState(PerfScope* scope) noexcept { return *new (scope->opaque) ScopeState(); }

bool isTimerScope(const PerfScope* scope) noexcept
{
    if (scope == nullptr) {
        return false;
    }
    const ScopeState& st = *reinterpret_cast<const ScopeState*>(scope->opaque);
    return st.magic == kScopeMagic && st.abi == kScopeAbi && st.kind == kKindTimer;
}

void resetScope(PerfScope* scope) noexcept
{
    if (scope != nullptr) {
        scopeState(scope) = ScopeState{};
    }
}

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
    struct ReleaseSlot
    {
        std::string name;
        std::string subName;
        TimePoint   begin{};
        TimePoint   subBegin{};
        bool        active{false};
    };

    std::vector<NodePerf>    pool;
    std::vector<char>        nameArena;
    std::vector<int32_t>     openStack;
    std::vector<ReleaseSlot> releaseSlots;
    std::vector<int32_t>     freeReleaseSlots;

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
            ctx.releaseSlots.reserve(16);
            ctx.freeReleaseSlots.reserve(16);
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

struct ConfigState
{
    std::atomic<bool>    enabled{true};
    std::atomic<bool>    debugMode{false};
    std::atomic<int32_t> timerLevel{3};
    std::atomic<int32_t> tracerLevel{LEVEL_ALL};
    std::atomic<bool>    aggregate{false};

    mutable std::mutex rootNameMutex;
    std::string        rootName{"perf"};
};

ConfigState& configState() noexcept
{
    static ConfigState state;
    return state;
}

// ---------------------------------------------------------------------------
//  Functions
// ---------------------------------------------------------------------------

int32_t allocReleaseSlot(CtxThread& ctx, const std::string& name, TimePoint begin) noexcept
{
    try {
        int32_t idx = -1;
        if (!ctx.freeReleaseSlots.empty()) {
            idx = ctx.freeReleaseSlots.back();
            ctx.freeReleaseSlots.pop_back();
        } else {
            idx = static_cast<int32_t>(ctx.releaseSlots.size());
            ctx.releaseSlots.emplace_back();
        }

        CtxThread::ReleaseSlot& slot = ctx.releaseSlots[static_cast<std::size_t>(idx)];
        slot.name                    = name;
        slot.subName.clear();
        slot.begin    = begin;
        slot.subBegin = begin;
        slot.active   = true;
        return idx;
    } catch (...) {
        return -1;
    }
}

CtxThread::ReleaseSlot* releaseSlot(CtxThread& ctx, int32_t idx) noexcept
{
    if (idx < 0 || idx >= static_cast<int32_t>(ctx.releaseSlots.size())) {
        return nullptr;
    }
    CtxThread::ReleaseSlot& slot = ctx.releaseSlots[static_cast<std::size_t>(idx)];
    return slot.active ? &slot : nullptr;
}

void freeReleaseSlot(CtxThread& ctx, int32_t idx) noexcept
{
    if (idx < 0 || idx >= static_cast<int32_t>(ctx.releaseSlots.size())) {
        return;
    }
    try {
        CtxThread::ReleaseSlot& slot = ctx.releaseSlots[static_cast<std::size_t>(idx)];
        slot.name.clear();
        slot.subName.clear();
        slot.active = false;
        ctx.freeReleaseSlots.push_back(idx);
    } catch (...) {
    }
}

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
//  Global configuration free functions
// ===========================================================================

void setEnabled(bool on) noexcept { configState().enabled.store(on, std::memory_order_relaxed); }

bool isEnabled() noexcept { return configState().enabled.load(std::memory_order_relaxed); }

void setDebugMode(bool on) noexcept { configState().debugMode.store(on, std::memory_order_relaxed); }

bool isDebugMode() noexcept { return configState().debugMode.load(std::memory_order_relaxed); }

void setTimerLevel(int32_t threshold) noexcept { configState().timerLevel.store(threshold, std::memory_order_relaxed); }

int32_t getTimerLevel() noexcept { return configState().timerLevel.load(std::memory_order_relaxed); }

void setTracerLevel(int32_t threshold) noexcept
{
    configState().tracerLevel.store(threshold, std::memory_order_relaxed);
}

int32_t getTracerLevel() noexcept { return configState().tracerLevel.load(std::memory_order_relaxed); }

void setRootName(const std::string& name) noexcept
{
    std::lock_guard<std::mutex> lk(configState().rootNameMutex);
    configState().rootName = name;
}

std::string getRootName() noexcept
{
    std::lock_guard<std::mutex> lk(configState().rootNameMutex);
    return configState().rootName;
}

void setAggregateMode(bool on) noexcept { configState().aggregate.store(on, std::memory_order_relaxed); }

bool isAggregateMode() noexcept { return configState().aggregate.load(std::memory_order_relaxed); }

void flushAggregated() noexcept
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
//  Stopwatch free functions
// ===========================================================================

void sleepFor(int64_t ms) noexcept
{
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string getTimeFormatted(const std::string& fmt) noexcept
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

uint64_t timerNowNs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

float timerElapsedMs(uint64_t beginNs) noexcept
{
    const uint64_t nowNs = timerNowNs();
    return static_cast<float>(static_cast<double>(nowNs - beginNs) / 1000000.0);
}

// ===========================================================================
//  Timer scope free functions
// ===========================================================================

TimePoint closeOpenSub(ScopeState& st) noexcept
{
    const auto now = Clock::now();

    if ((st.flags & kFlagRelease) != 0u) {
        CtxThread::ReleaseSlot* slot = releaseSlot(CtxThread::get(), st.releaseIdx);
        if (slot != nullptr && !slot->subName.empty()) {
            const float ms = std::chrono::duration<float, std::milli>(now - slot->subBegin).count();
            XLOG_I("[perf] %s: %.3f ms\n", slot->subName.c_str(), ms);
            slot->subName.clear();
        }
        return now;
    }

    if (st.subNodeIdx < 0 || CtxThread::get().inFlush) {
        return now;
    }

    if (st.subNodeIdx < static_cast<int32_t>(CtxThread::get().pool.size())) {
        NodePerf& prev = CtxThread::get().pool[static_cast<std::size_t>(st.subNodeIdx)];
        if (prev.durationMs < 0.0f) {
            prev.durationMs = std::chrono::duration<float, std::milli>(now - prev.begin).count();
        }
    }
    if (!CtxThread::get().openStack.empty() && CtxThread::get().openStack.back() == st.subNodeIdx) {
        CtxThread::get().openStack.pop_back();
    }
    st.subNodeIdx = -1;
    return now;
}

void timerBegin(PerfScope* scope, const std::string& name) noexcept
{
    if (scope == nullptr) {
        return;
    }

    ScopeState& st = initScopeState(scope);
    st.begin       = Clock::now();
    st.subBegin    = st.begin;

    if (!isEnabled() || CtxThread::get().inFlush) {
        return;
    }

    const bool     debugMode = isDebugMode();
    const uint32_t depth =
        debugMode ? static_cast<uint32_t>(CtxThread::get().openStack.size()) : CtxThread::get().releaseDepth;
    const int32_t threshold = getTimerLevel();

    if (depth >= HARD_MAX_DEPTH || threshold == LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    st.flags = kFlagActive;
    st.depth = depth;

    if (!debugMode) {
        st.releaseIdx = allocReleaseSlot(CtxThread::get(), name, st.begin);
        if (st.releaseIdx < 0) {
            st.flags = 0;
            return;
        }
        st.flags |= kFlagRelease | kFlagReleaseDepth;
        ++CtxThread::get().releaseDepth;
        return;
    }

    const int32_t parent = CtxThread::get().openStack.empty() ? -1 : CtxThread::get().openStack.back();
    const int32_t idx    = emplaceNode(CtxThread::get(), parent, name, depth, st.begin);
    if (idx < 0) {
        st.releaseIdx = allocReleaseSlot(CtxThread::get(), name, st.begin);
        st.flags      = (st.releaseIdx >= 0) ? static_cast<uint8_t>(kFlagActive | kFlagRelease) : 0;
        return;
    }

    try {
        CtxThread::get().openStack.push_back(idx);
    } catch (...) {
        st.releaseIdx = allocReleaseSlot(CtxThread::get(), name, st.begin);
        st.flags      = (st.releaseIdx >= 0) ? static_cast<uint8_t>(kFlagActive | kFlagRelease) : 0;
        return;
    }

    st.nodeIdx = idx;
    st.flags |= kFlagDebug;
    if (depth == 0u) {
        st.flags |= kFlagRoot;
    }
}

void timerSubBegin(PerfScope* scope, const std::string& name) noexcept
{
    if (!isTimerScope(scope)) {
        return;
    }

    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u) {
        return;
    }

    const auto now = closeOpenSub(st);

    if ((st.flags & kFlagRelease) != 0u) {
        CtxThread::ReleaseSlot* slot = releaseSlot(CtxThread::get(), st.releaseIdx);
        if (slot == nullptr) {
            return;
        }
        try {
            slot->subName  = name;
            slot->subBegin = now;
        } catch (...) {
            slot->subName.clear();
        }
        return;
    }

    if (CtxThread::get().inFlush) {
        return;
    }

    const uint32_t depth = st.depth + 1u;
    if (depth >= HARD_MAX_DEPTH) {
        return;
    }
    const int32_t threshold = getTimerLevel();
    if (threshold == LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    const int32_t idx = emplaceNode(CtxThread::get(), st.nodeIdx, name, depth, now);
    if (idx < 0) {
        return;
    }
    try {
        CtxThread::get().openStack.push_back(idx);
    } catch (...) {
        return;
    }
    st.subNodeIdx = idx;
}

void timerSubEnd(PerfScope* scope) noexcept
{
    if (!isTimerScope(scope)) {
        return;
    }
    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u) {
        return;
    }
    (void)closeOpenSub(st);
}

void timerEnd(PerfScope* scope) noexcept
{
    if (!isTimerScope(scope)) {
        return;
    }

    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u) {
        resetScope(scope);
        return;
    }

    const auto  now = closeOpenSub(st);
    const float msF = std::chrono::duration<float, std::milli>(now - st.begin).count();

    if ((st.flags & kFlagRelease) != 0u) {
        CtxThread::ReleaseSlot* slot = releaseSlot(CtxThread::get(), st.releaseIdx);
        if (slot != nullptr) {
            XLOG_I("[perf] %s: %.3f ms\n", slot->name.c_str(), msF);
            freeReleaseSlot(CtxThread::get(), st.releaseIdx);
        }
        if ((st.flags & kFlagReleaseDepth) != 0u && CtxThread::get().releaseDepth > 0u) {
            --CtxThread::get().releaseDepth;
        }
        resetScope(scope);
        return;
    }

    if (CtxThread::get().inFlush) {
        resetScope(scope);
        return;
    }

    if (st.nodeIdx >= 0 && st.nodeIdx < static_cast<int32_t>(CtxThread::get().pool.size())) {
        CtxThread::get().pool[static_cast<std::size_t>(st.nodeIdx)].durationMs = msF;
    }
    if (!CtxThread::get().openStack.empty() && CtxThread::get().openStack.back() == st.nodeIdx) {
        CtxThread::get().openStack.pop_back();
    }

    if ((st.flags & kFlagRoot) == 0u || !CtxThread::get().openStack.empty()) {
        resetScope(scope);
        return;
    }

    CtxThread::get().inFlush = true;

    try {
        TreeSnapThread snap;
        snap.pool      = std::move(CtxThread::get().pool);
        snap.nameArena = std::move(CtxThread::get().nameArena);
        snap.roots     = collectRoots(snap.pool);
        snap.tid       = CtxThread::get().tid;
        snap.rootName  = getRootName();

        if (isAggregateMode()) {
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
    resetScope(scope);
}

}  // namespace perf
}  // namespace au
