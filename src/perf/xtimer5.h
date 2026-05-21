#ifndef AURA_PERF_XTIMER5_H_
#define AURA_PERF_XTIMER5_H_

/**
 * @file xtimer5.h
 * @brief Consolidated hierarchical performance timer for the Aura SDK (v5).
 *
 * v5 is the converged design that absorbs lessons from xtimer / xtimer0..4.
 *
 * Key design decisions:
 *  - **Global unified configuration**: every scope reads the same global
 *    config (setEnabled / setMode / setTimerLevel / …). No per-caller
 *    context instances.
 *  - **Level == depth collapse**: callers no longer pass an explicit
 *    @c level parameter. Each scope's level is its tree depth. The internal
 *    safety net @c kHardMaxDepth=512 caps runaway recursion.
 *    @c setTimerLevel(N) means "show only nodes whose depth ≤ N".
 *  - **std::string API**: every name parameter is @c const std::string&,
 *    leveraging SSO so 99% of hot-path scopes incur zero allocation while
 *    completely sidestepping the v3 dangling-string_view bug.
 *  - **Trimmed surface**: @c XTimer5Scoped no longer exposes
 *    @c elapsedMs() (use @c XTimer5 instead).
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
 *   au::perf::setEnabled(true);
 *   au::perf::setMode(au::perf::Mode5::Debug);
 *   au::perf::setTimerLevel(3);  // show only nodes with depth ≤ 3
 *
 *   void XNet::forward() {
 *       AU_PERF5_SCOPE("XNet::forward");
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
    Debug   = 1,  ///< Build a per-thread tree; flush on root close.
};

// ---------------------------------------------------------------------------
// Global configuration (free functions, lock-free reads)
// ---------------------------------------------------------------------------

void setEnabled(bool on) noexcept;
bool isEnabled() noexcept;

void  setMode(Mode5 mode) noexcept;
Mode5 getMode() noexcept;

void    setTimerLevel(int32_t threshold) noexcept;
int32_t getTimerLevel() noexcept;

void    setTracerLevel(int32_t threshold) noexcept;
int32_t getTracerLevel() noexcept;

void setRootName(const std::string& name) noexcept;
void getRootName(char* outBuf, std::size_t bufSize) noexcept;

void setAggregateMode(bool on) noexcept;
bool isAggregateMode() noexcept;

/// Drain the global aggregate buffer. Idempotent — safe to call multiple
/// times; a no-op when empty.
void flushAggregated() noexcept;

/// Load configuration from system properties (Android) or environment
/// variables (other platforms). Pass an empty string to skip an entry.
void loadFromSystemProperty(const std::string& propEnabled, const std::string& propMode,
                            const std::string& propTimerLevel, const std::string& propTracerLevel) noexcept;

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
 *  - global isEnabled() must be true
 *  - the scope's tree depth must be ≤ getTimerLevel()
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
    explicit XTimer5Scoped(const std::string& name) noexcept;
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
    void begin(const std::string& name) noexcept;

    std::chrono::steady_clock::time_point closeOpenSub() noexcept;

    int32_t                               mNodeIdx;     ///< -1 inactive, -2 active-no-tree
    int32_t                               mSubNodeIdx;  ///< -1 = no open sub
    uint32_t                              mDepth;
    bool                                  mIsRoot;
    std::chrono::steady_clock::time_point mBegin;
    std::chrono::steady_clock::time_point mSubBegin;  ///< Release sub() timing.

    static constexpr std::size_t kInlineNameCap = 96;
    char                         mNameInline[kInlineNameCap];
    uint32_t                     mNameLen;

    char     mSubNameInline[kInlineNameCap];
    uint32_t mSubNameLen;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTIMER5_H_
