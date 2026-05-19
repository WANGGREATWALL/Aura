#include "perf/xtracer5.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sys/xplatform.h"

#if AU_OS_ANDROID
#include <fcntl.h>
#include <unistd.h>
#endif

namespace au {
namespace perf {

// ===========================================================================
//  Process-wide trace_marker fd helper (Android only)
// ===========================================================================

namespace {

#if AU_OS_ANDROID

/// Lazily-opened process-wide trace_marker fd. C++11 static-init is
/// thread-safe. The fd is intentionally never closed — the kernel reclaims
/// it on process exit, and O_CLOEXEC keeps fork+exec children clean.
int getTraceFd() noexcept
{
    static int fd = []() {
        int f = ::open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY | O_CLOEXEC);
        if (f < 0) {
            f = ::open("/sys/kernel/tracing/trace_marker", O_WRONLY | O_CLOEXEC);
        }
        return f;
    }();
    return fd;
}

/// One trace_marker write. Format: "B|pid|name" or "E|pid".
/// The kernel guarantees per-write atomicity up to one page, which is
/// already far above our 256-byte cap.
void writeTraceMarker(char mode, int pid, const char* name, std::size_t nameLen) noexcept
{
    int fd = getTraceFd();
    if (fd < 0) {
        return;
    }

    char        buf[256];
    std::size_t n = 0;

    if (mode == 'B') {
        const int rc = std::snprintf(buf, sizeof(buf), "B|%d|%.*s", pid, static_cast<int>(nameLen), name);
        if (rc <= 0) {
            return;
        }
        n = (static_cast<std::size_t>(rc) >= sizeof(buf)) ? sizeof(buf) - 1 : static_cast<std::size_t>(rc);
    } else {
        const int rc = std::snprintf(buf, sizeof(buf), "E|%d", pid);
        if (rc <= 0) {
            return;
        }
        n = static_cast<std::size_t>(rc);
    }

    (void)::write(fd, buf, n);
}

#endif  // AU_OS_ANDROID

/// Per-thread tracer-depth counter (mirrors xtimer5's openStack depth but
/// kept independent so timer / tracer macros can be used in isolation).
/// File-level thread_local so begin() and ~XTracer5Scoped() share storage.
thread_local uint32_t gTracerDepth = 0;

}  // anonymous namespace

// ===========================================================================
//  XTracer5Scoped
// ===========================================================================

XTracer5Scoped::XTracer5Scoped(const std::string& name) noexcept { begin(XPerfContext5::defaultContext(), name); }

XTracer5Scoped::XTracer5Scoped(XPerfContext5& ctx, const std::string& name) noexcept { begin(ctx, name); }

void XTracer5Scoped::begin(XPerfContext5& ctx, const std::string& name) noexcept
{
    mCtx     = &ctx;
    mActive  = false;
    mSubOpen = false;
    mNameLen = 0;
    mName[0] = '\0';

    if (!ctx.isEnabled()) {
        return;
    }

    // v5 collapses level == depth. The tracer maintains its own per-thread
    // depth counter (gTracerDepth, file-scope thread_local) since timer and
    // tracer macros may be used independently of one another.
    if (gTracerDepth >= kHardMaxDepth5) {
        return;
    }
    const int32_t threshold = ctx.getTracerLevel();
    if (threshold == kPerfLevelOff5 || static_cast<int32_t>(gTracerDepth) > threshold) {
        return;
    }

    // Copy + truncate the label even on non-Android so any future sink
    // (e.g. an in-memory trace ring for desktop) can read it back.
    const std::size_t cp = std::min(name.size(), kMaxName - 1);
    if (cp > 0) {
        std::memcpy(mName, name.data(), cp);
    }
    mName[cp] = '\0';
    mNameLen  = static_cast<uint8_t>(cp);
    mActive   = true;
    ++gTracerDepth;

#if AU_OS_ANDROID
    writeTraceMarker('B', getpid(), mName, mNameLen);
#endif
}

XTracer5Scoped::~XTracer5Scoped() noexcept
{
    if (!mActive) {
        return;
    }

    if (mSubOpen) {
        sub();  // close the inflight sub before the outer slice
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', getpid(), nullptr, 0);
#endif

    // Symmetric decrement of the per-thread tracer depth counter.
    if (gTracerDepth > 0u) {
        --gTracerDepth;
    }
}

void XTracer5Scoped::sub(const std::string& name) noexcept
{
    if (!mActive) {
        return;
    }

#if AU_OS_ANDROID
    if (mSubOpen) {
        writeTraceMarker('E', getpid(), nullptr, 0);
    }
#endif

    mSubOpen = true;

#if AU_OS_ANDROID
    char        buf[kMaxName];
    std::size_t cp = std::min(name.size(), kMaxName - 1);
    if (cp > 0) {
        std::memcpy(buf, name.data(), cp);
    }
    buf[cp] = '\0';
    writeTraceMarker('B', getpid(), buf, cp);
#else
    (void)name;
#endif
}

void XTracer5Scoped::sub() noexcept
{
    if (!mActive || !mSubOpen) {
        return;
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', getpid(), nullptr, 0);
#endif
    mSubOpen = false;
}

}  // namespace perf
}  // namespace au