#ifndef AURA_PERF_XTIMER5_H_
#define AURA_PERF_XTIMER5_H_

/**
 * @file xtimer5.h
 * @brief Consolidated hierarchical performance timer for the Aura SDK (v5).
 *
 * v5 is the converged design that absorbs lessons from xtimer / xtimer0..4
 * plus the supplementary @c xperf_8_design.md whitepaper.
 *
 * Key decisions vs v4:
 *  - **Per-context aggregation**: each @c XPerfContext5 owns its private
 *    aggregate buffer and exposes a per-instance @c flushAggregated().
 *    Multiple shared libraries can each flush their own report at their
 *    own dlclose time, instead of waiting for process exit.
 *  - **Per-context TLS bucketing**: each thread keeps a separate tree per
 *    @c XPerfContext5 instance, eliminating cross-caller tree mixing.
 *  - **Level == depth collapse**: callers no longer pass an explicit
 *    @c level parameter. Each scope's level is its tree depth. The internal
 *    safety net @c kHardMaxDepth=512 caps runaway recursion.
 *    @c setTimerLevel(N) means "show only nodes whose depth ≤ N".
 *  - **std::string API**: every name parameter is @c const std::string&,
 *    leveraging SSO so 99% of hot-path scopes incur zero allocation while
 *    completely sidestepping the v3 dangling-string_view bug.
 *  - **Trimmed surface**: @c XTimer5Scoped no longer exposes
 *    @c elapsedMs() (use @c XTimer5 instead). @c IPerfWriter is removed;
 *    output is exclusively routed through @c XLOG_I.
 *  - **sub() immediate output (Release-only)**: in Release mode, @c sub(name)
 *    immediately prints the previous sub-segment's elapsed time. In Debug
 *    mode it remains a tree-aggregating operation.
 *
 * Threading & safety:
 *  - Hot path is fully lock-free (TLS pool / arena / openStack).
 *  - All atomic reads use @c std::memory_order_relaxed.
 *  - All destructors are @c noexcept; OOM degrades a node to a one-liner.
 *
 * Quick start:
 * @code
 *   auto& ctx = au::perf::XPerfContext5::defaultContext();
 *   ctx.setEnabled(true);
 *   ctx.setMode(au::perf::Mode5::Debug);
 *   ctx.setTimerLevel(3);  // show only nodes with depth ≤ 3
 *
 *   void XNet::forward() {
 *       AU_PERF5_SCOPE("XNet::forward");
 *       AU_TIMER5_SCOPE("preprocess");
 *       // ... work ...
 *   }
 * @endcode
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace au {
namespace perf {

// ---------------------------------------------------------------------------
// Level sentinels and depth cap.
// ---------------------------------------------------------------------------

/// Hard-off sentinel: never activate any scope on this channel.
constexpr int32_t kPerfLevelOff5 = -1;

/// Always-on sentinel: every scope passes the level gate (subject to
/// the depth cap below).
constexpr int32_t kPerfLevelAll5 = INT32_MAX;

/// Internal safety net: nodes deeper than this are degraded to a one-liner
/// instead of being inserted into the tree. Not user-tunable.
constexpr uint32_t kHardMaxDepth5 = 512;

// ---------------------------------------------------------------------------
// Mode selector.
// ---------------------------------------------------------------------------

enum class Mode5 : int32_t
{
    Release = 0,  ///< One-liner per scope, no tree work.
    Debug   = 1,  ///< Build a per-thread, per-context tree; flush on root close.
};

// ---------------------------------------------------------------------------
// XPerfContext5 — per-caller configuration + private aggregate buffer.
// ---------------------------------------------------------------------------

/// Forward declaration: implementation lives entirely in the .cpp.
class XPerfContext5Impl;

/**
 * @brief Per-caller configuration container with private aggregate ownership (Pimpl ABI).
 *
 * All public read/write accessors are atomic and lock-free; safe to call
 * from any thread at any time.
 *
 * Each shared library (.so) or business module is encouraged to own a
 * private instance. Doing so guarantees:
 *  1. Configuration isolation (one module's @c setTimerLevel does not
 *     affect another module).
 *  2. Aggregate-data isolation (one module's @c flushAggregated() prints
 *     only its own report).
 *  3. Lifecycle isolation (when the owning module is dlclose'd, its
 *     context auto-flushes any residual data and unregisters cleanly).
 *
 * See @c xperf_8_design.md for the full multi-.so isolation rationale.
 */
class XPerfContext5
{
    // ── friends with internal-impl access (same translation unit boundary) ──
    friend class XTimer5Scoped;
    friend class XTracer5Scoped;

public:
    XPerfContext5() noexcept;
    ~XPerfContext5() noexcept;

    XPerfContext5(const XPerfContext5&)            = delete;
    XPerfContext5& operator=(const XPerfContext5&) = delete;

    /// Process-wide default instance.
    /// Convenience for callers that do not need module-level isolation.
    static XPerfContext5& defaultContext() noexcept;

    // ── master switch ──

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // ── mode ──

    void  setMode(Mode5 mode) noexcept;
    Mode5 getMode() const noexcept;

    // ── level thresholds (level == depth) ──

    /// In v5, "level" == "tree depth". @p threshold means: show only nodes
    /// whose depth ≤ @p threshold. Use @c kPerfLevelAll5 to show all,
    /// @c kPerfLevelOff5 to disable entirely.
    void    setTimerLevel(int32_t threshold) noexcept;
    int32_t getTimerLevel() const noexcept;

    /// Same semantics, applied to the tracer channel.
    void    setTracerLevel(int32_t threshold) noexcept;
    int32_t getTracerLevel() const noexcept;

    // ── root header label ──

    /// Truncates to 63 chars internally. Empty string is allowed.
    void setRootName(const std::string& name) noexcept;

    /// Copy into @p outBuf (always NUL-terminated).
    void getRootName(char* outBuf, std::size_t bufSize) const noexcept;

    // ── aggregate mode ──

    /// When enabled, completed root scopes are queued into this context's
    /// private buffer instead of being flushed immediately. Call
    /// @c flushAggregated() to drain.
    void setAggregateMode(bool on) noexcept;
    bool isAggregateMode() const noexcept;

    /**
     * @brief Drain this context's private aggregate buffer (instance method).
     *
     * Idempotent: safe to call multiple times; a no-op when the buffer is empty.
     *
     * Typical call sites:
     *  - At the end of a business module's lifetime, before @c dlclose.
     *  - At a quiescent checkpoint where reporting is desired.
     *
     * Output is routed through @c XLOG_I, one tree per call.
     */
    void flushAggregated() noexcept;

    /**
     * @brief Process-exit safety net.
     *
     * Iterates every still-alive @c XPerfContext5 in the registry and
     * invokes its @c flushAggregated(). Automatically registered via
     * @c std::atexit on first aggregate scope (or via @c .fini_array
     * destructor when Aura is built as a shared library).
     *
     * Callers should prefer the instance method @c flushAggregated();
     * this static is the last-resort fallback.
     */
    static void flushAllAggregatedForExit() noexcept;

    // ── system property loader ──

    /**
     * Load configuration from system properties (Android) or environment
     * variables (other platforms).
     *
     * Pass an empty string to skip an entry.
     *
     * Property values:
     *  - @p propEnabled     : "0" or "1"
     *  - @p propMode        : "0" (Release) or "1" (Debug)
     *  - @p propTimerLevel  : signed int as string
     *  - @p propTracerLevel : signed int as string
     */
    void loadFromSystemProperty(const std::string& propEnabled, const std::string& propMode,
                                const std::string& propTimerLevel, const std::string& propTracerLevel) noexcept;

private:
    XPerfContext5Impl* mImpl;  // Pimpl: hides std::atomic / containers from the public ABI.
};

// ---------------------------------------------------------------------------
// XTimer5 — bare stopwatch.
// ---------------------------------------------------------------------------

/// Lightweight stopwatch. Trivially copyable; ~ns construction.
/// Use this when you need an explicit elapsed-millisecond reading;
/// @c XTimer5Scoped no longer exposes that accessor.
class XTimer5
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Sleep helper (portable). Non-positive @p ms returns immediately.
    static void sleepFor(int64_t ms) noexcept;

    /// Thread-safe wrapper around @c localtime. Default format
    /// "%Y-%m-%d-%H-%M-%S". Returns "<formatted>_<ms>" with sub-second tail.
    static std::string getTimeFormatted(const std::string& fmt = "%Y-%m-%d-%H-%M-%S") noexcept;

    XTimer5() noexcept : mBegin(Clock::now()) {}

    void  restart() noexcept { mBegin = Clock::now(); }
    float elapsedMs() const noexcept;

private:
    TimePoint mBegin;
};

// ---------------------------------------------------------------------------
// XTimer5Scoped — RAII hierarchical timer.
// ---------------------------------------------------------------------------

/**
 * @brief RAII scoped timer with optional thread-local tree building.
 *
 * Activation rules (evaluated once at construction):
 *  - @c ctx.isEnabled() must be true
 *  - the scope's tree depth must be ≤ @c ctx.getTimerLevel()
 *  - the depth must be < @c kHardMaxDepth5 (internal safety net)
 *
 * If inactive, every member is a no-op with zero allocation.
 *
 * Name lifetime: the constructor copies @p name into either an inline
 * buffer (Release path) or the TLS arena (Debug path); the caller may
 * safely pass a temporary @c std::string.
 *
 * @note @c XTimer5Scoped intentionally does not expose @c elapsedMs().
 *       Use @c XTimer5 if explicit measurement is needed.
 */
class XTimer5Scoped
{
public:
    /// Use the default process context.
    explicit XTimer5Scoped(const std::string& name) noexcept;

    /// Use a caller-specific context.
    XTimer5Scoped(XPerfContext5& ctx, const std::string& name) noexcept;

    ~XTimer5Scoped() noexcept;

    XTimer5Scoped(const XTimer5Scoped&)            = delete;
    XTimer5Scoped& operator=(const XTimer5Scoped&) = delete;

    /**
     * @brief End the previous sub-segment (if any) and open a new one.
     *
     * Mode behavior:
     *  - Release: immediately prints "[perf5] <name>: X.XXX ms" for the
     *    just-closed segment via @c XLOG_I, then opens the new segment.
     *  - Debug: closes the previous sub-node in the tree and opens a new
     *    sibling under this scope; output is deferred to root flush.
     */
    void sub(const std::string& name) noexcept;

    /**
     * @brief End the previous sub-segment (if any). No-op when none is open.
     *
     * Mode behavior:
     *  - Release: immediately prints the just-closed segment.
     *  - Debug: closes the sub-node in the tree.
     */
    void sub() noexcept;

private:
    /// Shared constructor helper.
    void begin(XPerfContext5& ctx, const std::string& name) noexcept;

    /// Close the currently-open sub segment (if any) for both Release and
    /// Debug paths. Used by @c sub(name) before opening a new segment and
    /// by @c sub() to terminate the trailing segment. Returns the closing
    /// timestamp so callers may reuse it without re-sampling the clock.
    std::chrono::steady_clock::time_point closeOpenSub() noexcept;

    XPerfContext5* mCtx;

    int32_t                               mNodeIdx;     ///< -1 inactive, -2 active-no-tree
    int32_t                               mSubNodeIdx;  ///< -1 = no open sub
    uint32_t                              mDepth;
    bool                                  mIsRoot;
    std::chrono::steady_clock::time_point mBegin;
    std::chrono::steady_clock::time_point mSubBegin;  ///< Release sub() timing.

    /// Inline name buffer for the active-no-tree path. Anything longer
    /// is truncated. Debug path uses the TLS arena via offset/len pair
    /// stored in the pool node.
    static constexpr std::size_t kInlineNameCap = 96;
    char                         mNameInline[kInlineNameCap];
    uint32_t                     mNameLen;

    /// Inline buffer for the currently-open Release sub-segment name.
    char     mSubNameInline[kInlineNameCap];
    uint32_t mSubNameLen;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTIMER5_H_