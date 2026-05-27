#ifndef AURA_PERF_XPERF_MACROS_H_
#define AURA_PERF_XPERF_MACROS_H_

#include "perf/xtimer.h"
#include "perf/xtracer.h"

#define AU_PERF_CONCAT_(a, b) a##b
#define AU_PERF_CONCAT(a, b) AU_PERF_CONCAT_(a, b)

#if defined(__COUNTER__)
#define AU_PERF_UNIQUE(prefix) AU_PERF_CONCAT(prefix, __COUNTER__)
#else
#define AU_PERF_UNIQUE(prefix) AU_PERF_CONCAT(prefix, __LINE__)
#endif

#if defined(AU_PERF_DISABLE_ALL) && AU_PERF_DISABLE_ALL

#define AU_TIMER(name) ((void)0)
#define AU_TRACE(name) ((void)0)
#define AU_PERF_SCOPE(name) ((void)0)

#else

#define AU_TIMER(name) ::au::perf::XTimerScoped AU_PERF_UNIQUE(_auTmr_)((name))
#define AU_TRACE(name) ::au::perf::XTracerScoped AU_PERF_UNIQUE(_auTrc_)((name))
#define AU_PERF_SCOPE(name) \
    AU_TIMER(name);         \
    AU_TRACE(name)

#endif  // AU_PERF_DISABLE_ALL

#endif  // AURA_PERF_XPERF_MACROS_H_
