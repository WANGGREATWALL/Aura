#include "perf/xtracer.h"

#include <cstdio>

#include "perf/xtimer.h"
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

static int getTracePid() noexcept
{
    static int pid = static_cast<int>(au::sys::getCurrentProcessId());
    return pid;
}

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

void writeTraceMarker(char mode, const char* name, std::size_t nameLen) noexcept
{
    int fd = getTraceFd();
    if (fd < 0) {
        return;
    }

    const int pid = getTracePid();

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

thread_local uint32_t gTracerDepth = 0;

}  // anonymous namespace

// ===========================================================================
//  XTracerScoped
// ===========================================================================

XTracerScoped::XTracerScoped(const std::string& name) noexcept : mActive(false), mSubOpen(false)
{
    if (!Config::get().isEnabled()) {
        return;
    }

    if (gTracerDepth >= Config::HARD_MAX_DEPTH) {
        return;
    }
    const int32_t threshold = Config::get().getTracerLevel();
    if (threshold == Config::LEVEL_OFF || static_cast<int32_t>(gTracerDepth) > threshold) {
        return;
    }

    mName   = name;
    mActive = true;
    ++gTracerDepth;

#if AU_OS_ANDROID
    writeTraceMarker('B', mName.c_str(), mName.size());
#endif
}

XTracerScoped::~XTracerScoped() noexcept
{
    if (!mActive) {
        return;
    }

    if (mSubOpen) {
        sub();
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', nullptr, 0);
#endif

    if (gTracerDepth > 0u) {
        --gTracerDepth;
    }
}

void XTracerScoped::sub(const std::string& name) noexcept
{
    if (!mActive) {
        return;
    }

    if (mSubOpen) {
        sub();
    }

    ++gTracerDepth;
#if AU_OS_ANDROID
    writeTraceMarker('B', name.c_str(), name.size());
#else
    (void)name;
#endif
    mSubOpen = true;
}

void XTracerScoped::sub() noexcept
{
    if (!mActive || !mSubOpen) {
        return;
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', nullptr, 0);
#endif
    if (gTracerDepth > 0u) {
        --gTracerDepth;
    }
    mSubOpen = false;
}

}  // namespace perf
}  // namespace au
