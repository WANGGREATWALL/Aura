#ifndef AURA_PERF_XPERF_MACROS_H_
#define AURA_PERF_XPERF_MACROS_H_

/**
 * @file xperf_macros.h
 * @brief Convenience macros for the @c au::perf subsystem.
 *
 * Provides three composable macros:
 *  - @c AU_TIMER(name)      : RAII hierarchical timer scope.
 *  - @c AU_TRACE(name)      : Perfetto / ftrace slice (Android-only payload).
 *  - @c AU_PERF_SCOPE(name) : Composite of the above two.
 *
 * Each macro generates a uniquely-named local via @c __COUNTER__, so multiple
 * macros may coexist on the same source line without collision.
 *
 * Define @c AU_PERF_DISABLE_ALL=1 in build flags to compile every scope down
 * to @c ((void)0) — useful for shipping builds where even the no-op activation
 * checks are undesirable.
 */

#include "perf/xtimer.h"
#include "perf/xtracer.h"

#define AU_PERF_CONCAT_(a, b) a##b
#define AU_PERF_CONCAT(a, b) AU_PERF_CONCAT_(a, b)
#define AU_PERF_UNIQUE(prefix) AU_PERF_CONCAT(prefix, __COUNTER__)

#if defined(AU_PERF_DISABLE_ALL) && AU_PERF_DISABLE_ALL

#define AU_TIMER(name) ((void)0)
#define AU_TRACE(name) ((void)0)
#define AU_PERF_SCOPE(name) ((void)0)

#else

/// Hierarchical timer scope.
#define AU_TIMER(name) ::au::perf::XTimerScoped AU_PERF_UNIQUE(_auTmr_)((name))

/// Perfetto / ftrace slice (Android-only payload).
#define AU_TRACE(name) ::au::perf::XTracerScoped AU_PERF_UNIQUE(_auTrc_)((name))

/// Composite: declare both a timer and a tracer with the same label.
#define AU_PERF_SCOPE(name) \
    AU_TIMER(name);         \
    AU_TRACE(name)

#endif  // AU_PERF_DISABLE_ALL

#endif  // AURA_PERF_XPERF_MACROS_H_
