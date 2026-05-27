#include "perf/xtracer.h"

#include <cstdio>
#include <new>

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

constexpr uint32_t kScopeMagic = 0x41555046u;  // AUPF
constexpr uint16_t kScopeAbi   = 1;
constexpr uint8_t  kKindTrace  = 2;

constexpr uint8_t kFlagActive = 1u << 0;
constexpr uint8_t kFlagSub    = 1u << 1;

struct ScopeState
{
    uint32_t magic{kScopeMagic};
    uint16_t abi{kScopeAbi};
    uint8_t  kind{kKindTrace};
    uint8_t  flags{0};
    uint32_t depth{0};
    uint32_t reserved0{0};
    uint64_t reserved1[6]{};
};

static_assert(sizeof(ScopeState) <= sizeof(PerfScope), "PerfScope is too small for tracer state");
static_assert(alignof(ScopeState) <= alignof(PerfScope), "PerfScope alignment is too small");

ScopeState& scopeState(PerfScope* scope) noexcept { return *reinterpret_cast<ScopeState*>(scope->opaque); }

ScopeState& initScopeState(PerfScope* scope) noexcept { return *new (scope->opaque) ScopeState(); }

bool isTraceScope(const PerfScope* scope) noexcept
{
    if (scope == nullptr) {
        return false;
    }
    const ScopeState& st = *reinterpret_cast<const ScopeState*>(scope->opaque);
    return st.magic == kScopeMagic && st.abi == kScopeAbi && st.kind == kKindTrace;
}

void resetScope(PerfScope* scope) noexcept
{
    if (scope != nullptr) {
        scopeState(scope) = ScopeState{};
    }
}

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
//  Tracer scope free functions
// ===========================================================================

void traceBegin(PerfScope* scope, const std::string& name) noexcept
{
    if (scope == nullptr) {
        return;
    }

    ScopeState& st = initScopeState(scope);

    if (!isEnabled()) {
        return;
    }

    if (gTracerDepth >= HARD_MAX_DEPTH) {
        return;
    }
    const int32_t threshold = getTracerLevel();
    if (threshold == LEVEL_OFF || static_cast<int32_t>(gTracerDepth) > threshold) {
        return;
    }

    st.flags = kFlagActive;
    st.depth = gTracerDepth;
    ++gTracerDepth;

#if AU_OS_ANDROID
    writeTraceMarker('B', name.c_str(), name.size());
#else
    (void)name;
#endif
}

void traceSubBegin(PerfScope* scope, const std::string& name) noexcept
{
    if (!isTraceScope(scope)) {
        return;
    }

    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u) {
        return;
    }

    if ((st.flags & kFlagSub) != 0u) {
        traceSubEnd(scope);
    }

    const uint32_t depth = st.depth + 1u;
    if (depth >= HARD_MAX_DEPTH) {
        return;
    }
    const int32_t threshold = getTracerLevel();
    if (threshold == LEVEL_OFF || static_cast<int32_t>(depth) > threshold) {
        return;
    }

    ++gTracerDepth;
#if AU_OS_ANDROID
    writeTraceMarker('B', name.c_str(), name.size());
#else
    (void)name;
#endif
    st.flags |= kFlagSub;
}

void traceSubEnd(PerfScope* scope) noexcept
{
    if (!isTraceScope(scope)) {
        return;
    }

    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u || (st.flags & kFlagSub) == 0u) {
        return;
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', nullptr, 0);
#endif
    if (gTracerDepth > 0u) {
        --gTracerDepth;
    }
    st.flags &= static_cast<uint8_t>(~kFlagSub);
}

void traceEnd(PerfScope* scope) noexcept
{
    if (!isTraceScope(scope)) {
        return;
    }

    ScopeState& st = scopeState(scope);
    if ((st.flags & kFlagActive) == 0u) {
        resetScope(scope);
        return;
    }

    if ((st.flags & kFlagSub) != 0u) {
        traceSubEnd(scope);
    }

#if AU_OS_ANDROID
    writeTraceMarker('E', nullptr, 0);
#endif

    if (gTracerDepth > 0u) {
        --gTracerDepth;
    }
    resetScope(scope);
}

}  // namespace perf
}  // namespace au
