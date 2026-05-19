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
#include <unordered_map>
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

namespace au {
namespace perf {

// ===========================================================================
//  Internal node / TLS data structures (file-local)
// ===========================================================================

namespace {

/// One node in the per-thread, per-context tree. ~64 bytes on x86_64.
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

/// Per-thread, per-context tree slice. v5 partitions TLS by context to
/// avoid cross-caller tree mixing.
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
    std::unordered_map<class XPerfContext5Impl*, PerCtxTree> trees;
    std::thread::id                                          tid;
    bool                                                     inited{false};
    bool                                                     inFlush{false};
    class XPerfContext5Impl*                                 lastImpl{nullptr};
    PerCtxTree*                                              lastTree{nullptr};
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

/// Release-mode per-thread depth counter.
/// Debug mode tracks depth via tree.openStack, but Release mode bypasses
/// the tree entirely; without a separate counter the depth would always
/// read as 0, defeating the level==depth gate.
thread_local uint32_t gTimerReleaseDepth = 0;

uint64_t tidHash(std::thread::id id) noexcept { return static_cast<uint64_t>(std::hash<std::thread::id>{}(id)); }

}  // anonymous namespace

// ===========================================================================
//  XPerfContext5Impl (Pimpl) — per-context configuration + aggregate buffer
// ===========================================================================

struct FlushedTree
{
    std::vector<PerfNode> pool;
    std::vector<char>     arena;
    std::vector<int32_t>  roots;
    uint64_t              tid;
    char                  rootName[64];
};

struct AggregateData
{
    std::mutex               mMutex;
    std::vector<FlushedTree> mTrees;
};

class XPerfContext5Impl
{
public:
    std::atomic<bool>    mEnabled{true};
    std::atomic<Mode5>   mMode{Mode5::Release};
    std::atomic<int32_t> mTimerLevel{3};
    std::atomic<int32_t> mTracerLevel{kPerfLevelAll5};
    std::atomic<bool>    mAggregate{false};
    std::atomic<bool>    mAlive{true};

    std::atomic<uint32_t> mRootNameLen{4};
    char                  mRootName[64]{'p', 'e', 'r', 'f', '\0'};

    AggregateData mAggData;
};

// ===========================================================================
//  Forward declarations of file-local helpers
// ===========================================================================

namespace {

void emitFormatted(const char* fmt, ...) noexcept;
void printTree(const std::vector<PerfNode>& pool, const std::vector<char>& arena, const std::vector<int32_t>& roots,
               uint64_t tid, const char* rootName) noexcept;
void ContextRegistry_flushOne(XPerfContext5Impl* impl) noexcept;
PerCtxTree& tlsTreeFor(XPerfContext5Impl* impl) noexcept;

struct ArenaPutResult
{
    uint32_t offset;
    uint32_t len;
    bool     truncated;
};

ArenaPutResult       arenaPut(PerCtxTree& tree, const char* name, std::size_t inLen) noexcept;
std::vector<int32_t> collectRoots(const std::vector<PerfNode>& pool);

// ---------------------------------------------------------------------------
//  ContextRegistry (process-global, weak tracking)
// ---------------------------------------------------------------------------

/// Process-global registry of live XPerfContext5Impl pointers.
///
/// Lifetime caveat: this object is intentionally a *leaky* singleton (heap
/// allocated, never deleted). At-exit and dlclose callbacks may run *after*
/// arbitrary static destructors, so a Meyers singleton would leave the
/// internal `std::mutex` already destroyed by the time the callback fires —
/// triggering FORTIFY's "lock on destroyed mutex" abort. Leaking the storage
/// is the standard remedy: the callbacks always see a valid mutex, and the
/// OS reclaims the few hundred bytes when the process exits.
class ContextRegistry
{
public:
    static ContextRegistry& get() noexcept
    {
        static ContextRegistry* const instance = new ContextRegistry();
        return *instance;
    }

    void registerCtx(XPerfContext5Impl* impl) noexcept
    {
        if (impl == nullptr) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lk(mMutex);
            mAlive.push_back(impl);
            registerSafetyNetOnce();
        } catch (...) {
        }
    }

    void unregisterCtx(XPerfContext5Impl* impl) noexcept
    {
        if (impl == nullptr) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lk(mMutex);
            auto                        it = std::find(mAlive.begin(), mAlive.end(), impl);
            if (it != mAlive.end()) {
                mAlive.erase(it);
            }
        } catch (...) {
        }
    }

    void flushAllAlive() noexcept
    {
        // Re-entrancy / late-shutdown guard: once the at-exit hook has fired
        // it is unsafe to assume the rest of the runtime is still healthy.
        if (mShuttingDown.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::vector<XPerfContext5Impl*> snapshot;
        try {
            std::lock_guard<std::mutex> lk(mMutex);
            snapshot = mAlive;
        } catch (...) {
            return;
        }
        for (XPerfContext5Impl* impl : snapshot) {
            ContextRegistry_flushOne(impl);
        }
    }

private:
    ContextRegistry() = default;

    void registerSafetyNetOnce() noexcept
    {
        if (mSafetyNetDone.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::atexit(&safetyNetHook);
    }

    static void safetyNetHook() noexcept { get().flushAllAlive(); }

    std::mutex                      mMutex;
    std::vector<XPerfContext5Impl*> mAlive;
    std::atomic<bool>               mSafetyNetDone{false};
    std::atomic<bool>               mShuttingDown{false};
};

// Note: a __attribute__((destructor)) hook was considered for .so unload
// scenarios but removed because (a) its execution order relative to static
// destructors is unspecified, frequently racing with our own Meyers
// singletons, and (b) std::atexit already covers the process-exit flush.
// .so-unload safety is preserved by XPerfContext5's own dtor calling
// unregisterCtx + flushAggregated before destroying its mImpl.

}  // anonymous namespace

// ===========================================================================
//  XPerfContext5 — public surface
// ===========================================================================

XPerfContext5::XPerfContext5() noexcept : mImpl(new(std::nothrow) XPerfContext5Impl)
{
    // OOM at startup is tolerated: every accessor below treats null mImpl as
    // "permanently disabled" instead of crashing.
    if (mImpl != nullptr) {
        ContextRegistry::get().registerCtx(mImpl);
    }
}

XPerfContext5::~XPerfContext5() noexcept
{
    if (mImpl == nullptr) {
        return;
    }
    // Order matters: unregister first so the safety net cannot race with
    // our delete; then drain residual aggregate data; then mark the impl
    // dead so any in-flight scopes degrade to a no-op; finally release.
    ContextRegistry::get().unregisterCtx(mImpl);

    flushAggregated();

    mImpl->mAlive.store(false, std::memory_order_release);

    delete mImpl;
    mImpl = nullptr;
}

XPerfContext5& XPerfContext5::defaultContext() noexcept
{
    // Intentional leaky singleton: the process-global default context must
    // never have its destructor called during the static-destruction phase.
    //
    // On Android, vendor shared libraries (e.g. libvivo.mempool.so) may
    // install global malloc/free hooks in their constructors and uninstall
    // them in their destructors. The relative destruction order between those
    // library dtors and our Meyers singleton dtor is unspecified by the C++
    // standard and non-deterministic in bionic. If the vendor dtor fires first,
    // the subsequent `delete mImpl` inside ~XPerfContext5() would call free()
    // through a broken allocator path, causing an intermittent SIGSEGV.
    //
    // Leaking avoids the delete entirely. The OS reclaims the memory on exit.
    // Residual aggregate data is still flushed by the ContextRegistry atexit
    // safety-net hook, which runs before shared-library .fini_array teardown.
    static XPerfContext5* const instance = new XPerfContext5();
    return *instance;
}

void XPerfContext5::setEnabled(bool on) noexcept
{
    if (mImpl != nullptr) {
        mImpl->mEnabled.store(on, std::memory_order_relaxed);
    }
}

bool XPerfContext5::isEnabled() const noexcept
{
    return mImpl != nullptr && mImpl->mEnabled.load(std::memory_order_relaxed);
}

void XPerfContext5::setMode(Mode5 mode) noexcept
{
    if (mImpl != nullptr) {
        mImpl->mMode.store(mode, std::memory_order_relaxed);
    }
}

Mode5 XPerfContext5::getMode() const noexcept
{
    return mImpl != nullptr ? mImpl->mMode.load(std::memory_order_relaxed) : Mode5::Release;
}

void XPerfContext5::setTimerLevel(int32_t threshold) noexcept
{
    if (mImpl != nullptr) {
        mImpl->mTimerLevel.store(threshold, std::memory_order_relaxed);
    }
}

int32_t XPerfContext5::getTimerLevel() const noexcept
{
    return mImpl != nullptr ? mImpl->mTimerLevel.load(std::memory_order_relaxed) : kPerfLevelOff5;
}

void XPerfContext5::setTracerLevel(int32_t threshold) noexcept
{
    if (mImpl != nullptr) {
        mImpl->mTracerLevel.store(threshold, std::memory_order_relaxed);
    }
}

int32_t XPerfContext5::getTracerLevel() const noexcept
{
    return mImpl != nullptr ? mImpl->mTracerLevel.load(std::memory_order_relaxed) : kPerfLevelOff5;
}

void XPerfContext5::setRootName(const std::string& name) noexcept
{
    if (mImpl == nullptr) {
        return;
    }
    const std::size_t cap = sizeof(mImpl->mRootName) - 1;
    const std::size_t cp  = std::min(name.size(), cap);
    if (cp > 0u) {
        std::memcpy(mImpl->mRootName, name.data(), cp);
    }
    mImpl->mRootName[cp] = '\0';
    mImpl->mRootNameLen.store(static_cast<uint32_t>(cp), std::memory_order_release);
}

void XPerfContext5::getRootName(char* outBuf, std::size_t bufSize) const noexcept
{
    if (outBuf == nullptr || bufSize == 0) {
        return;
    }
    if (mImpl == nullptr) {
        outBuf[0] = '\0';
        return;
    }
    const uint32_t    len = mImpl->mRootNameLen.load(std::memory_order_acquire);
    const std::size_t cp  = std::min(static_cast<std::size_t>(len), bufSize - 1);
    std::memcpy(outBuf, mImpl->mRootName, cp);
    outBuf[cp] = '\0';
}

void XPerfContext5::setAggregateMode(bool on) noexcept
{
    if (mImpl != nullptr) {
        mImpl->mAggregate.store(on, std::memory_order_relaxed);
    }
}

bool XPerfContext5::isAggregateMode() const noexcept
{
    return mImpl != nullptr && mImpl->mAggregate.load(std::memory_order_relaxed);
}

void XPerfContext5::flushAggregated() noexcept
{
    if (mImpl == nullptr) {
        return;
    }

    std::vector<FlushedTree> local;
    try {
        std::lock_guard<std::mutex> lk(mImpl->mAggData.mMutex);
        local.swap(mImpl->mAggData.mTrees);
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

void XPerfContext5::flushAllAggregatedForExit() noexcept { ContextRegistry::get().flushAllAlive(); }

void XPerfContext5::loadFromSystemProperty(const std::string& propEnabled, const std::string& propMode,
                                           const std::string& propTimerLevel,
                                           const std::string& propTracerLevel) noexcept
{
    if (mImpl == nullptr) {
        return;
    }

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

/// Format then route through XLOG_I. Truncates to 512 bytes.
/// XLOG_I appends its own newline framing, so the format string must NOT
/// embed a trailing '\n'.
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

/// Emit one node line. v5 deliberately drops the v4 two-pass column
/// alignment: lines are "<prefix><branch><name>: <duration> ms".
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

PerCtxTree& tlsTreeFor(XPerfContext5Impl* impl) noexcept
{
    PerfThreadCtx& ctx = tlsCtx();

    // Fast-path: same impl as last call (>99% hit rate in single-ctx workloads).
    if (ctx.lastImpl == impl && ctx.lastTree != nullptr) {
        return *ctx.lastTree;
    }

    auto it = ctx.trees.find(impl);
    if (it == ctx.trees.end()) {
        it = ctx.trees.emplace(impl, PerCtxTree{}).first;
        try {
            it->second.pool.reserve(kPoolReserve);
            it->second.nameArena.reserve(kArenaReserve);
            it->second.openStack.reserve(kStackReserve);
        } catch (...) {
            // Reserve failures are tolerated; subsequent push_back may throw,
            // and that path is caught locally in begin()/sub().
        }
    }
    ctx.lastImpl = impl;
    ctx.lastTree = &it->second;
    return it->second;
}

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

/// Append one node to the per-thread tree slice and link it under @p parent
/// (parent == -1 → root, with sibling chain auto-fixed). Common to both
/// @c XTimer5Scoped::begin() and @c sub(name) Debug paths.
///
/// Returns the new node's index, or -1 on OOM. Caller is responsible for
/// pushing into @c openStack and updating its own bookkeeping (mNodeIdx /
/// mSubNodeIdx / mIsRoot).
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
            // Root: stitch sibling chain to the previous root, if any.
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

void ContextRegistry_flushOne(XPerfContext5Impl* impl) noexcept
{
    if (impl == nullptr || !impl->mAlive.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<FlushedTree> local;
    try {
        std::lock_guard<std::mutex> lk(impl->mAggData.mMutex);
        local.swap(impl->mAggData.mTrees);
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

XTimer5Scoped::XTimer5Scoped(const std::string& name) noexcept { begin(XPerfContext5::defaultContext(), name); }

XTimer5Scoped::XTimer5Scoped(XPerfContext5& ctx, const std::string& name) noexcept { begin(ctx, name); }

void XTimer5Scoped::begin(XPerfContext5& ctx, const std::string& name) noexcept
{
    mCtx              = &ctx;
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

    // Always copy the inline name first — even on the inactive path the
    // caller may pass a temporary std::string.
    if (!name.empty()) {
        const std::size_t cp = std::min(name.size(), kInlineNameCap - 1);
        std::memcpy(mNameInline, name.data(), cp);
        mNameInline[cp] = '\0';
        mNameLen        = static_cast<uint32_t>(cp);
    }

    if (!ctx.isEnabled()) {
        return;
    }

    XPerfContext5Impl* impl = ctx.mImpl;
    if (impl == nullptr || !impl->mAlive.load(std::memory_order_acquire)) {
        return;
    }

    PerfThreadCtx& tls = tlsCtx();
    if (tls.inFlush) {
        return;
    }

    const Mode5 mode = impl->mMode.load(std::memory_order_relaxed);

    // Depth source differs by mode: Release uses a lightweight per-thread
    // counter; Debug derives it from the per-context openStack. Both must
    // pass the same level==depth gate for behaviour parity across modes.
    const uint32_t depth =
        (mode == Mode5::Release) ? gTimerReleaseDepth : static_cast<uint32_t>(tlsTreeFor(impl).openStack.size());

    // Hard depth cap (internal safety net, not user-tunable).
    if (depth >= kHardMaxDepth5) {
        return;
    }

    // Level gate: depth must be ≤ ctx.getTimerLevel(). v5 collapses
    // "level == depth": the per-scope level parameter is gone.
    const int32_t threshold = impl->mTimerLevel.load(std::memory_order_relaxed);
    if (threshold == kPerfLevelOff5 || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    if (mode == Mode5::Release) {
        // Release path: no tree work, destructor prints a single line.
        // Sentinel -2 distinguishes "active but tree-less" from inactive (-1).
        mNodeIdx = -2;
        mDepth   = depth;
        ++gTimerReleaseDepth;
        return;
    }

    // -- Debug path: append to the per-context TLS pool --
    PerCtxTree&   tree   = tlsTreeFor(impl);
    const int32_t parent = tree.openStack.empty() ? -1 : tree.openStack.back();
    const int32_t idx    = appendNodeUnsafe(tree, parent, name.data(), name.size(), depth, mBegin);
    if (idx < 0) {
        // OOM → degrade to one-liner.
        mNodeIdx = -2;
        mDepth   = depth;
        return;
    }
    try {
        tree.openStack.push_back(idx);
    } catch (...) {
        // openStack push failed: leave node in pool but degrade scope to one-liner.
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
    if (mCtx == nullptr || mNodeIdx == -1) {
        return;
    }

    const auto     now = std::chrono::steady_clock::now();
    const uint64_t ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mBegin).count());
    const float msF = static_cast<float>(static_cast<double>(ns) / 1.0e6);

    XPerfContext5Impl* impl = mCtx->mImpl;
    if (impl == nullptr || !impl->mAlive.load(std::memory_order_acquire)) {
        return;
    }

    // -- Release / degraded path: one-liner --
    if (mNodeIdx == -2) {
        // If a Release-mode sub() segment is still open, flush it first so
        // the user sees the trailing phase before the outer scope's summary.
        if (mSubNameLen > 0u) {
            const uint64_t subNs =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - mSubBegin).count());
            const float subMs = static_cast<float>(static_cast<double>(subNs) / 1.0e6);
            emitFormatted("[perf5] %.*s: %.3f ms", static_cast<int>(mSubNameLen), mSubNameInline, subMs);
        }
        emitFormatted("[perf5] %.*s: %.3f ms", static_cast<int>(mNameLen), mNameInline, msF);
        // Pair with the ++ in begin(): only Release scopes that truly entered
        // (mNodeIdx == -2) bumped the counter; degraded-from-OOM scopes also
        // came through the Debug catch-block which does not increment, but
        // they still set mNodeIdx = -2. To avoid underflow, guard with > 0.
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
    PerCtxTree& tree = tlsTreeFor(impl);

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

    // -- Outermost scope: flush this thread's slice for this context --
    tls.inFlush = true;

    const bool aggregate = mCtx->isAggregateMode();
    char       rootName[64];
    mCtx->getRootName(rootName, sizeof(rootName));

    try {
        if (aggregate) {
            FlushedTree snap;
            snap.pool  = tree.pool;
            snap.arena = tree.nameArena;
            snap.roots = collectRoots(tree.pool);
            snap.tid   = tidHash(tls.tid);
            std::memcpy(snap.rootName, rootName, sizeof(snap.rootName));

            std::lock_guard<std::mutex> lk(impl->mAggData.mMutex);
            impl->mAggData.mTrees.emplace_back(std::move(snap));
        } else {
            const auto roots = collectRoots(tree.pool);
            printTree(tree.pool, tree.nameArena, roots, tidHash(tls.tid), rootName);
        }
    } catch (...) {
        // Perf must never kill the host.
    }

    tree.pool.clear();
    tree.nameArena.clear();
    tree.openStack.clear();
    tls.inFlush = false;
}

std::chrono::steady_clock::time_point XTimer5Scoped::closeOpenSub() noexcept
{
    const auto now = std::chrono::steady_clock::now();

    // Caller has already validated mCtx, mNodeIdx, and impl liveness.
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
    XPerfContext5Impl* impl = mCtx->mImpl;
    PerCtxTree&        tree = tlsTreeFor(impl);

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
    if (mCtx == nullptr || mNodeIdx == -1) {
        return;
    }

    XPerfContext5Impl* impl = mCtx->mImpl;
    if (impl == nullptr || !impl->mAlive.load(std::memory_order_acquire)) {
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
    const int32_t threshold = impl->mTimerLevel.load(std::memory_order_relaxed);
    if (threshold == kPerfLevelOff5 || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    PerCtxTree&   tree = tlsTreeFor(impl);
    const int32_t idx  = appendNodeUnsafe(tree, mNodeIdx, name.data(), name.size(), depth, now);
    if (idx < 0) {
        return;  // Drop sub silently on OOM.
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
    if (mCtx == nullptr || mNodeIdx == -1) {
        return;
    }

    XPerfContext5Impl* impl = mCtx->mImpl;
    if (impl == nullptr || !impl->mAlive.load(std::memory_order_acquire)) {
        return;
    }

    (void)closeOpenSub();
}

}  // namespace perf
}  // namespace au