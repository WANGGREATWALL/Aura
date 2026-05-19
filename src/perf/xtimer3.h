#ifndef AURA_PERF_XTIMER3_H_
#define AURA_PERF_XTIMER3_H_

/**
 * @file xtimer3.h
 * @brief Cross-platform hierarchical performance timer (v3).
 *
 * Features:
 *  - Cross-platform (Windows / Linux / Android / macOS / iOS).
 *  - Per-scope RAII timer with optional level gating (integer, not enum).
 *  - Release mode (default): zero tree, instant log at scope end.
 *  - Debug mode: thread-local hierarchical tree, flushed on outermost scope exit.
 *  - Aggregate mode: accumulate per-thread trees into a process-wide collector,
 *    printed together at program exit (or via XPerfContext3::flushAggregated).
 *  - Thread-safe: each thread owns its tree (no locks on hot path).
 *  - Crash-safe: all destructors noexcept; nothing runs in namespace-scope
 *    static destructors.
 *  - Multi-instance config: callers can own a private XPerfContext3 so multiple
 *    algorithm libraries shipped in the same process don't stomp each other.
 *
 * Quick start:
 * @code
 *   // In algorithm-library init:
 *   auto& cfg = au::perf::XPerfContext3::defaultConfig();
 *   cfg.loadFromSystemProperty(
 *       "vendor.algo.perf.enabled",
 *       "vendor.algo.perf.mode",
 *       "vendor.algo.perf.timer_lv",
 *       "vendor.algo.perf.tracer_lv");
 *
 *   // Anywhere in code:
 *   void XNet::forward() {
 *       AU_PERF3_SCOPE("XNet::forward");            // timer + tracer (level 0)
 *       AU_TIMER3_L("preprocess", 2);              // only timer, level 2
 *       // ...
 *   }
 * @endcode
 *
 * Level semantics (integer, not enum):
 *  - Scopes carry an integer @p level. Smaller = more important.
 *  - A scope activates if its level <= XPerfContext3 threshold.
 *  - Default threshold values and sentinels:
 *      kPerfLevelOff = -1   : never activate (hard off for the module)
 *      kPerfLevelAll = INT32_MAX : always activate
 *    Defaults: timerLevel = 3, tracerLevel = kPerfLevelAll.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace au::perf {

// ---------------------------------------------------------------------------
// Level sentinels (plain integers, not enum -- level can be any int32_t).
// ---------------------------------------------------------------------------

constexpr int32_t kPerfLevelOff = -1;
constexpr int32_t kPerfLevelAll = INT32_MAX;

// ---------------------------------------------------------------------------
// Mode selector.
// ---------------------------------------------------------------------------

enum class Mode : int32_t
{
    Release = 0,  ///< Default. No tree. Scope end prints a one-liner.
    Debug   = 1,  ///< Build thread-local tree; flush on outermost scope.
};

/// Per-caller configuration bundle for the perf subsystem.
///
/// Safe for concurrent reads on the hot path (all fields are std::atomic).
/// Writes are expected from an init phase. Callers shipping a dynamic library
/// can own their own XPerfContext3 instance so multiple libraries in the same
/// process don't share a singleton.
class XPerfContext3
{
public:
    /// Process-wide default instance. Used when a scope is constructed
    /// without an explicit XPerfContext3 reference.
    static XPerfContext3& defaultConfig() noexcept;

    XPerfContext3() noexcept;
    ~XPerfContext3() = default;

    XPerfContext3(const XPerfContext3&)            = delete;
    XPerfContext3& operator=(const XPerfContext3&) = delete;

    // ---------------- master switch ----------------

    /// Enable / disable the whole perf pipeline owned by this config.
    void setEnabled(bool on) noexcept { mEnabled.store(on, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }

    // ---------------- mode ----------------

    void setMode(Mode mode) noexcept { mMode.store(mode, std::memory_order_relaxed); }
    Mode getMode() const noexcept { return mMode.load(std::memory_order_relaxed); }

    // ---------------- level thresholds ----------------
    //
    // A scope with `level` activates iff `level <= threshold`.
    // Pass kPerfLevelOff to disable the whole channel; kPerfLevelAll to accept
    // every level.

    void    setTimerLevel(int32_t threshold) noexcept { mTimerLevel.store(threshold, std::memory_order_relaxed); }
    int32_t getTimerLevel() const noexcept { return mTimerLevel.load(std::memory_order_relaxed); }

    void    setTracerLevel(int32_t threshold) noexcept { mTracerLevel.store(threshold, std::memory_order_relaxed); }
    int32_t getTracerLevel() const noexcept { return mTracerLevel.load(std::memory_order_relaxed); }

    // ---------------- depth guard ----------------

    /// Cap recorded tree depth to prevent log spam on pathological call sites.
    /// Default 64. Nodes deeper than this still measure their own duration
    /// but are not inserted into the tree.
    void     setMaxTreeDepth(uint32_t depth) noexcept { mMaxDepth.store(depth, std::memory_order_relaxed); }
    uint32_t getMaxTreeDepth() const noexcept { return mMaxDepth.load(std::memory_order_relaxed); }

    // ---------------- root name (Debug header) ----------------

    void setRootName(std::string_view name) noexcept;
    /// Copies the current root name into @p outBuf (NUL-terminated, truncated).
    void getRootName(char* outBuf, size_t bufSize) const noexcept;

    // ---------------- multi-thread aggregate mode ----------------

    /// When enabled, per-thread Debug trees are not flushed at the outermost
    /// scope exit; they are instead appended to a process-global collector.
    /// Call flushAggregated() (or rely on atexit) to emit grouped output.
    void setAggregateMode(bool on) noexcept { mAggregate.store(on, std::memory_order_relaxed); }
    bool isAggregateMode() const noexcept { return mAggregate.load(std::memory_order_relaxed); }

    /// Flush the process-global collector now. Idempotent.
    /// Also registered via std::atexit the first time aggregate mode is turned on.
    static void flushAggregated() noexcept;

    // ---------------- Android system property helper ----------------
    //
    // Pass nullptr to skip an entry. On non-Android platforms the call is a
    // no-op (system properties do not exist there).
    //
    // Expected value types:
    //   propEnabled      : "0" or "1"
    //   propMode         : "0" (Release) or "1" (Debug)
    //   propTimerLevel   : signed int (e.g. "-1", "3", "2147483647")
    //   propTracerLevel  : signed int

    void loadFromSystemProperty(const char* propEnabled, const char* propMode, const char* propTimerLevel,
                                const char* propTracerLevel) noexcept;

private:
    std::atomic<bool>     mEnabled{true};
    std::atomic<Mode>     mMode{Mode::Release};
    std::atomic<int32_t>  mTimerLevel{3};
    std::atomic<int32_t>  mTracerLevel{kPerfLevelAll};
    std::atomic<uint32_t> mMaxDepth{64};
    std::atomic<bool>     mAggregate{false};

    // Root name: protected by a coarse mutex, but only touched during init.
    // Using fixed-size buffer avoids std::string allocations in static-dtor
    // windows.
    mutable std::atomic<uint32_t> mRootNameLen{4};
    char                          mRootName[64]{'p', 'e', 'r', 'f', '\0'};
};

/// Lightweight stopwatch. Cheap to construct; cheap to read.
class XTimer3
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Sleep helper (portable). Negative or zero duration returns immediately.
    static void sleepFor(std::chrono::milliseconds ms) noexcept;

    /// Thread-safe wrapper around localtime (uses localtime_r / localtime_s).
    /// Returns "<formatted>_<ms>" where <ms> is the sub-second component.
    static std::string getTimeFormatted(std::string_view fmt = "%Y-%m-%d-%H-%M-%S") noexcept;

    XTimer3() noexcept : mBegin(Clock::now()) {}

    /// Reset begin time to now.
    void restart() noexcept { mBegin = Clock::now(); }

    /// Milliseconds since last restart (or construction).
    float elapsedMs() const noexcept { return std::chrono::duration<float, std::milli>(Clock::now() - mBegin).count(); }

private:
    TimePoint mBegin;
};

/// RAII scoped timer. Constructs = push; destructs = pop + optional flush.
///
/// Activation rules (evaluated once at construction):
///  - cfg.isEnabled()             must be true;
///  - level <= cfg.getTimerLevel() must hold.
/// If not active, all member functions become no-ops with no heap allocation.
///
/// Thread-local: every thread owns its own node pool; no locks on hot path.
/// Outermost active scope on each thread flushes that thread's subtree on
/// destruction (Release: nothing to flush; Debug: DFS-print or aggregate).
class XTimer3Scoped
{
public:
    /// Use the default process config.
    explicit XTimer3Scoped(std::string_view name, int32_t level = 0) noexcept;

    /// Use a caller-specific config (e.g. one owned by an algorithm library).
    XTimer3Scoped(XPerfContext3& cfg, std::string_view name, int32_t level = 0) noexcept;

    ~XTimer3Scoped() noexcept;

    XTimer3Scoped(const XTimer3Scoped&)            = delete;
    XTimer3Scoped& operator=(const XTimer3Scoped&) = delete;

    /// End the last sub-node (if any) and open a new sibling with @p name.
    /// No-op if this scope is inactive.
    void sub(std::string_view name) noexcept;

    /// End the last sub-node (if any). No-op if none is open or scope inactive.
    void sub() noexcept;

private:
    /// Shared constructor helper.
    void begin(std::string_view name, int32_t level) noexcept;

    XPerfContext3* mCfg{nullptr};
    int32_t        mNodeIdx{-1};     ///< index into thread-local pool; -1 = inactive
    int32_t        mSubNodeIdx{-1};  ///< currently-open sub, -1 = none
    uint32_t       mDepth{0};
    bool           mIsRoot{false};

    // Release-mode fast path: we need name + begin time without touching the
    // tree. Debug-mode path stores these in the pool node; these members are
    // still populated for simplicity and remain cheap.
    std::chrono::steady_clock::time_point mBegin;
    std::string_view                      mName;
};

}  // namespace au::perf

// ---------------------------------------------------------------------------
// Public macros.
// ---------------------------------------------------------------------------
//
// All macros use __COUNTER__ so multiple perf scopes may coexist on the same
// source line without identifier collisions.

#define AU_PERF_CONCAT_INNER(a, b) a##b
#define AU_PERF_CONCAT(a, b) AU_PERF_CONCAT_INNER(a, b)
#define AU_PERF_UNIQUE(prefix) AU_PERF_CONCAT(prefix, __COUNTER__)

#define AU_TIMER3(name) ::au::perf::XTimer3Scoped AU_PERF_UNIQUE(_auTimer3_)((name))
#define AU_TIMER3_L(name, lv) ::au::perf::XTimer3Scoped AU_PERF_UNIQUE(_auTimer3_)((name), (lv))
#define AU_TIMER3_CFG(cfg, name, lv) ::au::perf::XTimer3Scoped AU_PERF_UNIQUE(_auTimer3_)((cfg), (name), (lv))

// Composite macros (AU_PERF3_SCOPE etc.) are defined in xtracer3.h.

#endif  // AURA_PERF_XTIMER3_H_