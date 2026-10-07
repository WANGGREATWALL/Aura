#ifndef AURA_PERF_XPERF_H_
#define AURA_PERF_XPERF_H_

/**
 * @file xperf.h
 * @brief Composite RAII scope combining @c XTimerScoped and @c XTracerScoped.
 *
 * @c XPerfScoped is a thin facade that owns one timer scope and one tracer
 * scope sharing the same label. A single call to @c sub() advances both the
 * timer sub-segment and the tracer sub-slice, so callers instrument a scope
 * once and get the hierarchical timing tree (from the timer) and the
 * Perfetto / ftrace slice (from the tracer) together.
 *
 * Destruction order
 * -----------------
 * Members are declared @c mTimer first, @c mTracer second, so destruction runs
 * in reverse: the tracer scope closes first (emits the matching "E" event),
 * then the timer scope closes (records elapsed time and, in debug mode, flushes
 * the per-thread timing tree). This ordering is deliberate — trace events close
 * before the timer tree is printed — and must be preserved.
 *
 * Activation independence
 * -----------------------
 * The timer is gated by @c Config::getTimerLevel() and the tracer by
 * @c Config::getTracerLevel(); the two thresholds are independent. When the
 * levels differ, @c sub() still calls both — the inactive side is a safe
 * no-op but a given sub-segment may appear in only one of the two outputs.
 * This is by design; the two channels remain independently controllable.
 *
 * Quick start
 * -----------
 * @code
 *   void XNet::forward() {
 *       au::perf::XPerfScoped scope("XNet::forward");
 *       // ... shared preamble ...
 *       scope.sub("preprocess");
 *       // ... preprocessing ...
 *       scope.sub("inference");
 *       // ... inference ...
 *   }  // ~XPerfScoped: closes last sub on both channels, then closes the scope
 * @endcode
 *
 * @note This class is non-copyable and non-movable; always declare it as a
 *       named local variable.
 * @note For timer-only or tracer-only instrumentation, use @c XTimerScoped /
 *       @c XTracerScoped directly — @c XPerfScoped always opens both channels.
 */

#include <string>

#include "perf/xtimer.h"
#include "perf/xtracer.h"

namespace au {
namespace perf {

// ============================================================================
//  XPerfScoped — composite timer + tracer RAII scope
// ============================================================================

/**
 * @brief RAII scope that drives @c XTimerScoped and @c XTracerScoped together.
 *
 * Constructs both sub-objects with the same @p name; the destructor closes
 * the tracer first, then the timer (see the file header for the rationale).
 */
class XPerfScoped
{
public:
    /**
     * @brief Open a composite scope named @p name on both channels.
     * @param name  Scope label shared by the timer and the tracer.
     */
    explicit XPerfScoped(const std::string& name) noexcept
        : mTimer(name), mTracer(name) {}

    /**
     * @brief Close any open sub-segment / sub-slice and open a new one.
     *
     * Forwards to @c XTimerScoped::sub(name) and @c XTracerScoped::sub(name).
     * Each side independently applies its own activation gating; an inactive
     * side is a no-op.
     *
     * @param name  Sub-segment / sub-slice label.
     */
    void sub(const std::string& name) noexcept
    {
        mTimer.sub(name);
        mTracer.sub(name);
    }

    /**
     * @brief Close any open sub-segment / sub-slice without opening a new one.
     *
     * Forwards to @c XTimerScoped::sub() and @c XTracerScoped::sub().
     */
    void sub() noexcept
    {
        mTimer.sub();
        mTracer.sub();
    }

    XPerfScoped(const XPerfScoped&)            = delete;
    XPerfScoped& operator=(const XPerfScoped&) = delete;
    XPerfScoped(XPerfScoped&&)                 = delete;
    XPerfScoped& operator=(XPerfScoped&&)      = delete;

private:
    XTimerScoped  mTimer;  ///< Hierarchical timing tree + log output.
    XTracerScoped mTracer; ///< Perfetto / ftrace slice. Destroyed first (reverse order).
};

} // namespace perf
} // namespace au

#endif // AURA_PERF_XPERF_H_
