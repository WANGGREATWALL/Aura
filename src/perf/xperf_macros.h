#ifndef AURA_PERF_XPERF_MACROS_H_
#define AURA_PERF_XPERF_MACROS_H_

/**
 * @file xperf_macros.h
 * @brief Convenience macros for the v5 perf subsystem.
 *
 * Define @c AU_PERF_DISABLE_ALL=1 in build flags to strip every scope
 * to @c ((void)0) for shipping builds.
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
