#ifndef AURA_PERF_XPERF5_MACROS_H_
#define AURA_PERF_XPERF5_MACROS_H_

/**
 * @file xperf5_macros.h
 * @brief Convenience macros for the v5 perf subsystem.
 *
 * v5 macro surface:
 *  - Drops the @c _L (explicit level) variants. v5 collapses
 *    "level == tree depth", so per-scope level is no longer a parameter.
 *  - Drops the @c _CFG (per-context) variants. v5 uses global unified
 *    configuration; every scope reads the same global config.
 *  - The composite @c AU_PERF5_SCOPE wraps both timer and tracer with the
 *    same label, ensuring perfetto slices and hierarchical log entries
 *    stay aligned.
 *
 * Define @c AU_PERF5_DISABLE_ALL=1 in build flags to strip every scope
 * down to @c ((void)0). This eliminates both the @c .text and the runtime
 * cost in shipping builds where instrumentation is undesired.
 */

#include "perf/xtimer5.h"
#include "perf/xtracer5.h"

#define AU_PERF5_CONCAT_(a, b) a##b
#define AU_PERF5_CONCAT(a, b) AU_PERF5_CONCAT_(a, b)
#define AU_PERF5_UNIQUE(prefix) AU_PERF5_CONCAT(prefix, __COUNTER__)

#if defined(AU_PERF5_DISABLE_ALL) && AU_PERF5_DISABLE_ALL

#define AU_TIMER5(name) ((void)0)
#define AU_TRACE5(name) ((void)0)
#define AU_PERF5_SCOPE(name) ((void)0)

#else

/// Hierarchical timer scope.
#define AU_TIMER5(name) ::au::perf::XTimer5Scoped AU_PERF5_UNIQUE(_auTmr5_)((name))

/// Perfetto / ftrace slice (Android-only payload).
#define AU_TRACE5(name) ::au::perf::XTracer5Scoped AU_PERF5_UNIQUE(_auTrc5_)((name))

/// Composite: declare both a tree timer and a perfetto slice with the
/// same label in one source line.
#define AU_PERF5_SCOPE(name) \
    AU_TIMER5(name);         \
    AU_TRACE5(name)

#endif  // AU_PERF5_DISABLE_ALL

#endif  // AURA_PERF_XPERF5_MACROS_H_
