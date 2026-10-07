#ifndef AURA_PERF_XPERF_CONFIG_H_
#define AURA_PERF_XPERF_CONFIG_H_

/**
 * @file xperf_config.h
 * @brief Global configuration facade for the au::perf subsystem.
 *
 * @c Config is the shared configuration layer above the timer and the tracer.
 * It owns the process-wide knobs (enable / debug mode / level thresholds /
 * aggregate mode / root name) that both @c XTimerScoped and @c XTracerScoped
 * read on their hot paths. Because it serves both channels, it lives in its
 * own header — independent of @c xtimer.h and @c xtracer.h — so that neither
 * wrapper has to include the other just to reach the configuration.
 *
 * Layering
 * --------
 * @code
 *   xperf_types.h       (C-compatible primitives: AuPerfScope, level constants)
 *   xtimer_api.h        (C ABI: timer scope + shared config C functions)
 *   xtracer_api.h       (C ABI: tracer scope)
 *       |
 *       v
 *   xperf_config.h      (this file — Config singleton facade, header-only)
 *       |
 *       v
 *   xtimer.h / xtracer.h (C++ RAII wrappers)
 * @endcode
 *
 * @c Config is header-only: every setter / getter is an inline call to the
 * underlying @c au_perf_* C function declared in @c xtimer_api.h. Hot-path
 * getters are backed by lock-free atomics in the implementation, so they are
 * safe to call on every frame.
 *
 * Quick start
 * -----------
 * @code
 *   au::perf::Config& cfg = au::perf::Config::get();
 *   cfg.setEnabled(true);
 *   cfg.setDebugMode(false);
 *   cfg.setTimerLevel(3);
 *   cfg.setTracerLevel(au::perf::Config::LEVEL_ALL);
 * @endcode
 *
 * @note Config is non-copyable and non-movable; always access it via get().
 * @note All setters are @c noexcept; failures in the underlying C functions
 *       are silently swallowed to maintain crash-safety.
 */

#include <cstdint>
#include <string>

#include "perf/xtimer_api.h" // au_perf_* config C ABI

namespace au {
namespace perf {

// ============================================================================
//  Config — global configuration singleton
// ============================================================================

/**
 * @brief Singleton facade for the global au::perf configuration.
 *
 * All setters route to the underlying @c au_perf_* C functions via the
 * @c xtimer_api.h narrow waist. Hot-path getters are backed by lock-free
 * atomics in the implementation, making them safe to call on every frame.
 *
 * @note Config is non-copyable and non-movable; always access it via get().
 * @note All setters are @c noexcept; failures in the underlying C functions
 *       are silently swallowed to maintain crash-safety.
 */
class Config
{
public:
    /// Level constant: disables the corresponding channel entirely.
    static constexpr int32_t LEVEL_OFF = AU_PERF_LEVEL_OFF;
    /// Level constant: records every nesting depth.
    static constexpr int32_t LEVEL_ALL = AU_PERF_LEVEL_ALL;
    /// Hard ceiling on nesting depth: scopes beyond this are silently dropped.
    static constexpr uint32_t HARD_MAX_DEPTH = AU_PERF_HARD_MAX_DEPTH;

    /**
     * @brief Return the process-wide Config singleton.
     * @return Reference to the singleton (Meyers' singleton, thread-safe init).
     */
    static Config& get() noexcept
    {
        static Config instance;
        return instance;
    }

    // ------------------------------------------------------------------------
    //  Global enable / disable
    // ------------------------------------------------------------------------

    /**
     * @brief Enable or disable the entire perf subsystem.
     * @param on @c true to enable (default); @c false to suppress all output.
     */
    void setEnabled(bool on) noexcept { au_perf_set_enabled(on ? 1 : 0); }

    /** @brief Return @c true if the perf subsystem is currently enabled. */
    bool isEnabled() const noexcept { return au_perf_is_enabled() != 0; }

    // ------------------------------------------------------------------------
    //  Debug / release mode
    // ------------------------------------------------------------------------

    /**
     * @brief Toggle between debug (tree) and release (flat) output mode.
     *
     * In debug mode the implementation builds a per-thread tree and prints
     * the complete hierarchy when the outermost scope closes. In release mode
     * (default) each scope emits a single flat log line.
     *
     * @param on @c true to enable debug mode.
     */
    void setDebugMode(bool on) noexcept { au_perf_set_debug_mode(on ? 1 : 0); }

    /** @brief Return @c true if debug mode is currently active. */
    bool isDebugMode() const noexcept { return au_perf_is_debug_mode() != 0; }

    // ------------------------------------------------------------------------
    //  Level thresholds
    // ------------------------------------------------------------------------

    /**
     * @brief Set the maximum timer nesting depth recorded.
     *
     * Scopes at depth > @p threshold are silently dropped. Use @c LEVEL_OFF
     * to suppress all timers or @c LEVEL_ALL to record every depth.
     * Default: 3.
     *
     * @param threshold  Inclusive depth ceiling (depth 0 = outermost scope).
     */
    void setTimerLevel(int32_t threshold) noexcept { au_perf_set_timer_level(threshold); }

    /** @brief Return the current timer level threshold. */
    int32_t getTimerLevel() const noexcept { return au_perf_get_timer_level(); }

    /**
     * @brief Set the maximum tracer nesting depth recorded.
     * @param threshold  Inclusive depth ceiling. Default: @c LEVEL_ALL.
     */
    void setTracerLevel(int32_t threshold) noexcept { au_perf_set_tracer_level(threshold); }

    /** @brief Return the current tracer level threshold. */
    int32_t getTracerLevel() const noexcept { return au_perf_get_tracer_level(); }

    // ------------------------------------------------------------------------
    //  Root name
    // ------------------------------------------------------------------------

    /**
     * @brief Set the root label printed at the head of each tree flush.
     * @param name  Label string (empty string clears the label).
     */
    void setRootName(const std::string& name) noexcept { au_perf_set_root_name(name.c_str()); }

    /**
     * @brief Return the current root label.
     * @return Label string, or an empty string if none has been set.
     */
    std::string getRootName() const noexcept
    {
        char buf[256];
        buf[0] = '\0';
        const int n = au_perf_get_root_name(buf, static_cast<int>(sizeof(buf)));
        return (n > 0) ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
    }

    // ------------------------------------------------------------------------
    //  Aggregate mode
    // ------------------------------------------------------------------------

    /**
     * @brief Enable or disable deferred aggregate flush mode.
     *
     * When enabled, trees are queued instead of printed at scope close.
     * Call @c flushAggregated() to drain and print them in thread-id order.
     *
     * @param on @c true to enable aggregate mode.
     */
    void setAggregateMode(bool on) noexcept { au_perf_set_aggregate_mode(on ? 1 : 0); }

    /** @brief Return @c true if aggregate mode is currently active. */
    bool isAggregateMode() const noexcept { return au_perf_is_aggregate_mode() != 0; }

    /**
     * @brief Drain and print all accumulated trees from the aggregate buffer.
     *
     * Trees are sorted by thread-id before printing. Idempotent.
     */
    void flushAggregated() noexcept { au_perf_flush_aggregated(); }

private:
    Config()                        = default;
    Config(const Config&)           = delete;
    Config& operator=(const Config&) = delete;
};

} // namespace perf
} // namespace au

#endif // AURA_PERF_XPERF_CONFIG_H_
