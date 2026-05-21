#ifndef AURA_PERF_XPERF5_MACROS_H_
#define AURA_PERF_XPERF5_MACROS_H_

/**
 * @file xperf5_macros.h
 * @brief Convenience macros for the v5 perf subsystem.
 *
 * Define @c AU_PERF5_DISABLE_ALL=1 in build flags to strip every scope
 * to @c ((void)0) for shipping builds.
 */

#include "perf/xtimer5.h"
#include "perf/xtracer5.h"

#define AU_PERF5_CONCAT_(a, b) a##b
#define AU_PERF5_CONCAT(a, b) AU_PERF5_CONCAT_(a, b)
#define AU_PERF5_UNIQUE(prefix) AU_PERF5_CONCAT(prefix, __COUNTER__)

#if defined(AU_PERF5_DISABLE_ALL) && AU_PERF5_DISABLE_ALL

#define AU_TIMER5(name)       ((void)0)
#define AU_TRACE5(name)       ((void)0)
#define AU_PERF5_SCOPE(name)  ((void)0)

#else

/// Hierarchical timer scope.
#define AU_TIMER5(name) \
    ::au::perf::XTimer5Scoped AU_PERF5_UNIQUE(_auTmr5_)((name))

/// Perfetto / ftrace slice (Android-only payload).
#define AU_TRACE5(name) \
    ::au::perf::XTracer5Scoped AU_PERF5_UNIQUE(_auTrc5_)((name))

/// Composite: declare both a timer and a tracer with the same label.
#define AU_PERF5_SCOPE(name) \
    AU_TIMER5(name);         \
    AU_TRACE5(name)

#endif  // AU_PERF5_DISABLE_ALL

#endif  // AURA_PERF_XPERF5_MACROS_H_
