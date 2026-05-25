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
 *    An internal hard depth cap (@c PerfConfig::HARD_MAX_DEPTH = 512) guards
 *    against runaway recursion.
 *  - Name lifetime: the constructor copies @p name into @c mName (both
 *    modes); Debug mode additionally stores into the TLS arena.
 *    Temporary @c std::string is safe.
 *  - @c sub() immediate output (Release-only): each @c sub(name) immediately
 *    prints the just-closed segment. Debug mode defers output to root flush.
 *  - All destructors are @c noexcept; OOM degrades a node to a one-liner.
 *
 * Quick start:
 * @code
 *   auto& cfg = au::perf::PerfConfig::get();
 *   cfg.setDebugMode(true);
 *   cfg.setTimerLevel(3);
 *
 *   void XNet::forward() {
 *       AU_PERF_SCOPE("XNet::forward");
 *       // ... work ...
 *   }
 * @endcode
 *
 * Android system property override (optional):
 * @code
 *   // adb shell setprop debug.aura.perf.enabled 1
 *   // adb shell setprop debug.aura.perf.mode 1 # 1 = Debug
 *   // adb shell setprop debug.aura.perf.timer 5
 *   // Read via au::sys::getSystemPropertyValue() and call the setters above.
 * @endcode
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace au {
namespace perf {

// ---------------------------------------------------------------------------
//  PerfConfig — Meyers singleton
// ---------------------------------------------------------------------------

/**
 * @brief Global performance configuration singleton.
 *
 * Hot-path accessors (@c isEnabled, @c isDebugMode, @c getTimerLevel, etc.)
 * are atomic and lock-free. @c setRootName / @c getRootName are guarded by
 * an internal mutex hidden in the implementation; they are not on the hot
 * path and safe to call from any thread.
 */
class PerfConfig
{
public:
    /// Hard-off sentinel: pass to @c setTimerLevel / @c setTracerLevel to
    /// disable the channel entirely.
    static constexpr int32_t LEVEL_OFF = -1;

    /// Always-on sentinel: every scope passes the level gate.
    static constexpr int32_t LEVEL_ALL = INT32_MAX;

    /// Maximum tree depth before a new scope is silently dropped.
    static constexpr uint32_t HARD_MAX_DEPTH = 512;

    static PerfConfig& get() noexcept
    {
        static PerfConfig instance;
        return instance;
    }

    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    /// Enable (@c true) or disable (@c false) Debug tree mode.
    /// Debug builds a per-thread tree and flushes on root close.
    /// Release (default) emits a one-liner per scope.
    void setDebugMode(bool on) noexcept;
    bool isDebugMode() const noexcept;

    void    setTimerLevel(int32_t threshold) noexcept;
    int32_t getTimerLevel() const noexcept;

    void    setTracerLevel(int32_t threshold) noexcept;
    int32_t getTracerLevel() const noexcept;

    void        setRootName(const std::string& name) noexcept;
    std::string getRootName() const noexcept;

    void setAggregateMode(bool on) noexcept;
    bool isAggregateMode() const noexcept;

    /// Drain the global aggregate buffer. Idempotent.
    void flushAggregated() noexcept;

private:
    PerfConfig()                             = default;
    PerfConfig(const PerfConfig&)            = delete;
    PerfConfig& operator=(const PerfConfig&) = delete;

    std::atomic<bool>    mEnabled{true};
    std::atomic<bool>    mDebugMode{false};
    std::atomic<int32_t> mTimerLevel{3};
    std::atomic<int32_t> mTracerLevel{LEVEL_ALL};
    std::atomic<bool>    mAggregate{false};

    std::string mRootName{"perf"};  ///< guarded by gRootNameMutex (xtimer.cpp)
};

// ---------------------------------------------------------------------------
//  XTimer — bare stopwatch
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
//  XTimerScoped — RAII hierarchical timer
// ---------------------------------------------------------------------------

/**
 * @brief RAII scoped timer with optional thread-local tree building.
 *
 * Activation rules (evaluated once at construction):
 *  - @c PerfConfig::get().isEnabled() must be true
 *  - the scope's tree depth must be ≤ @c getTimerLevel()
 *  - the depth must be < @c PerfConfig::HARD_MAX_DEPTH
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
    std::chrono::steady_clock::time_point closeOpenSub() noexcept;

    int32_t                               mNodeIdx;     ///< -1 inactive, -2 active-no-tree
    int32_t                               mSubNodeIdx;  ///< -1 = no open sub
    uint32_t                              mDepth;
    bool                                  mIsRoot;
    std::chrono::steady_clock::time_point mBegin;
    std::chrono::steady_clock::time_point mSubBegin;
    std::string                           mName;
    std::string                           mSubName;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTIMER_H_
