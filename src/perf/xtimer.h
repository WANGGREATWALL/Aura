#ifndef AURA_PERF_XTIMER_H_
#define AURA_PERF_XTIMER_H_

/**
 * @file xtimer.h
 * @brief Hierarchical performance timer with Release/Debug dual-mode output.
 *
 * Key design:
 *  - Global unified configuration via @c PerfConfig::get() (Meyers singleton).
 *  - Level == tree depth: no caller-supplied level parameter.
 *    @c setTimerLevel(N) means "show only nodes whose depth ≤ N".
 *    The internal safety net @c kHardMaxDepth=512 caps runaway recursion.
 *  - Name lifetime: the constructor copies @p name into an inline buffer
 *    (Release) or the TLS arena (Debug); temporary @c std::string is safe.
 *  - @c sub() immediate output (Release-only): each @c sub(name) immediately
 *    prints the just-closed segment. Debug mode defers output to root flush.
 *  - All destructors are @c noexcept; OOM degrades a node to a one-liner.
 *
 * Quick start:
 * @code
 *   auto& cfg = au::perf::PerfConfig::get();
 *   cfg.setMode(au::perf::Mode::Debug);
 *   cfg.setTimerLevel(3);
 *
 *   void XNet::forward() {
 *       AU_PERF_SCOPE("XNet::forward");
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
// Level sentinels and depth cap
// ---------------------------------------------------------------------------

/// Hard-off sentinel: never activate any scope on this channel.
constexpr int32_t kPerfLevelOff = -1;

/// Always-on sentinel: every scope passes the level gate.
constexpr int32_t kPerfLevelAll = INT32_MAX;

/// Internal safety net: nodes deeper than this are degraded to a one-liner.
constexpr uint32_t kHardMaxDepth = 512;

// ---------------------------------------------------------------------------
// Mode selector
// ---------------------------------------------------------------------------

enum class Mode : int32_t
{
    Release = 0,  ///< One-liner per scope, no tree work.
    Debug   = 1,  ///< Build a per-thread tree; flush on root close.
};

// ---------------------------------------------------------------------------
// PerfConfig — Meyers singleton, lock-free reads
// ---------------------------------------------------------------------------

/**
 * @brief Global performance configuration singleton.
 *
 * All read/write accessors are atomic and lock-free; safe to call from
 * any thread at any time.
 */
class PerfConfig
{
public:
    static PerfConfig& get() noexcept
    {
        static PerfConfig instance;
        return instance;
    }

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    void  setMode(Mode mode) noexcept;
    Mode getMode() const noexcept;

    void    setTimerLevel(int32_t threshold) noexcept;
    int32_t getTimerLevel() const noexcept;

    void    setTracerLevel(int32_t threshold) noexcept;
    int32_t getTracerLevel() const noexcept;

    void setRootName(const std::string& name) noexcept;

    /// Copy into @p outBuf (always NUL-terminated).
    void getRootName(char* outBuf, std::size_t bufSize) const noexcept;

    void setAggregateMode(bool on) noexcept;
    bool isAggregateMode() const noexcept;

    /// Drain the global aggregate buffer. Idempotent.
    void flushAggregated() noexcept;

    /// Load configuration from system properties (Android) or environment
    /// variables (other platforms). Pass an empty string to skip an entry.
    void loadFromSystemProperty(const std::string& propEnabled,
                                const std::string& propMode,
                                const std::string& propTimerLevel,
                                const std::string& propTracerLevel) noexcept;

private:
    PerfConfig()                             = default;
    PerfConfig(const PerfConfig&)            = delete;
    PerfConfig& operator=(const PerfConfig&) = delete;

    std::atomic<bool>    mEnabled{true};
    std::atomic<Mode>   mMode{Mode::Release};
    std::atomic<int32_t> mTimerLevel{3};
    std::atomic<int32_t> mTracerLevel{kPerfLevelAll};
    std::atomic<bool>    mAggregate{false};

    std::atomic<uint32_t> mRootNameLen{4};
    char                  mRootName[64]{'p', 'e', 'r', 'f', '\0'};
};

// ---------------------------------------------------------------------------
// XTimer — bare stopwatch
// ---------------------------------------------------------------------------

/// Lightweight stopwatch. Use for explicit elapsed-millisecond readings.
class XTimer
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Sleep helper (portable).
    static void sleepFor(int64_t ms) noexcept;

    /// Thread-safe wrapper around localtime. Returns "<formatted>_<ms>".
    static std::string getTimeFormatted(const std::string& fmt = "%Y-%m-%d-%H-%M-%S") noexcept;

    XTimer() noexcept : mBegin(Clock::now()) {}

    void  restart() noexcept { mBegin = Clock::now(); }
    float elapsedMs() const noexcept;

private:
    TimePoint mBegin;
};

// ---------------------------------------------------------------------------
// XTimerScoped — RAII hierarchical timer
// ---------------------------------------------------------------------------

/**
 * @brief RAII scoped timer with optional thread-local tree building.
 *
 * Activation rules (evaluated once at construction):
 *  - @c PerfConfig::get().isEnabled() must be true
 *  - the scope's tree depth must be ≤ @c getTimerLevel()
 *  - the depth must be < @c kHardMaxDepth
 *
 * If inactive, every member is a no-op with zero allocation.
 *
 * @note This class intentionally does not expose @c elapsedMs().
 *       Use @c XTimer for explicit measurement.
 */
class XTimerScoped
{
public:
    explicit XTimerScoped(const std::string& name) noexcept;
    ~XTimerScoped() noexcept;

    XTimerScoped(const XTimerScoped&)            = delete;
    XTimerScoped& operator=(const XTimerScoped&) = delete;

    void sub(const std::string& name) noexcept;
    void sub() noexcept;

private:
    void begin(const std::string& name) noexcept;

    std::chrono::steady_clock::time_point closeOpenSub() noexcept;

    int32_t                               mNodeIdx;     ///< -1 inactive, -2 active-no-tree
    int32_t                               mSubNodeIdx;  ///< -1 = no open sub
    uint32_t                              mDepth;
    bool                                  mIsRoot;
    std::chrono::steady_clock::time_point mBegin;
    std::chrono::steady_clock::time_point mSubBegin;

    static constexpr std::size_t kInlineNameCap = 96;
    char                         mNameInline[kInlineNameCap];
    uint32_t                     mNameLen;

    char     mSubNameInline[kInlineNameCap];
    uint32_t mSubNameLen;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTIMER_H_
