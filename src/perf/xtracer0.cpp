#include "perf/xtracer0.h"

#include <cstdio>

#include "log/xlogger.h"
#include "sys/xplatform.h"

#ifdef AU_OS_ANDROID
#include <fcntl.h>
#include <unistd.h>
#endif

namespace au {
namespace perf {

// ==========================================================================
// XTracer0Context
// ==========================================================================

XTracer0Context::XTracer0Context() : mTimerCtx(&XTimer0Context::global()), mLevel(-1), mFdTrace(-1)
{
#ifdef AU_OS_ANDROID
    int fd = ::open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY);
    if (fd == -1)
        fd = ::open("/sys/kernel/tracing/trace_marker", O_WRONLY);
    mFdTrace = fd;
#endif
}

XTracer0Context& XTracer0Context::get(const char* name)
{
    static XTracer0Context sGlobal;
    if (!name || !*name)
        return sGlobal;

    // Per-named contexts stored as static locals keyed by a simple array.
    // Max 16 named tracer contexts (parallel to timer contexts).
    constexpr int kMaxTracerCtx = 16;
    struct Slot
    {
        XTracer0Context* ctx      = nullptr;
        char             name[64] = {0};
    };
    static Slot sSlots[kMaxTracerCtx];

    for (int i = 0; i < kMaxTracerCtx; ++i) {
        if (sSlots[i].ctx && std::strcmp(sSlots[i].name, name) == 0)
            return *sSlots[i].ctx;
    }
    for (int i = 0; i < kMaxTracerCtx; ++i) {
        if (!sSlots[i].ctx) {
            auto* c = new XTracer0Context();
            // Also create a corresponding named timer context
            c->mTimerCtx  = &XTimer0Context::get(name);
            sSlots[i].ctx = c;
            std::strncpy(sSlots[i].name, name, sizeof(sSlots[i].name) - 1);
            return *c;
        }
    }
    return sGlobal;
}

XTracer0Context& XTracer0Context::global()
{
    static XTracer0Context instance;
    return instance;
}

void XTracer0Context::configureFrom(const char* propertyPrefix) noexcept
{
    mTimerCtx->configureFrom(propertyPrefix);

    if (!propertyPrefix)
        return;

    std::string keyLevel = std::string(propertyPrefix) + ".trace_level";
    std::string valLevel = sys::getSystemPropertyValue(keyLevel.c_str(), "");
    if (valLevel.empty())
        valLevel = sys::getEnv(keyLevel.c_str());
    if (!valLevel.empty())
        setLevel(std::atoi(valLevel.c_str()));
}

// ==========================================================================
// XTracer0Scoped
// ==========================================================================

namespace {

enum TraceMode
{
    kBegin = 0,
    kEnd
};

void writeTraceMarker([[maybe_unused]] int fd, [[maybe_unused]] const char* name,
                      [[maybe_unused]] TraceMode mode) noexcept
{
#ifdef AU_OS_ANDROID
    if (fd < 0 || !name)
        return;
    char buf[256];
    int  pid = static_cast<int>(getpid());
    int  len = std::snprintf(buf, sizeof(buf), "%c|%d|%s", (mode == kBegin ? 'B' : 'E'), pid, name);
    if (len > 0)
        ::write(fd, buf, static_cast<size_t>(len));
#endif
}

}  // anonymous namespace

XTracer0Scoped::XTracer0Scoped(const char* name, XTracer0Context* ctx) noexcept
    : mCtx(ctx ? ctx : &XTracer0Context::global()), mNameMain(name), mNameNode(), mActive(false)
{
    if (!mCtx->isEnabled())
        return;

    mActive = true;
    if (mCtx->shouldPrint(1)) {
        mTimer.reset(new XTimer0Scoped(name, &mCtx->timerContext()));
        writeTraceMarker(mCtx->getTraceFd(), name, kBegin);
    }
}

XTracer0Scoped::~XTracer0Scoped() noexcept
{
    if (!mActive)
        return;

    if (mTimer) {
        if (!mNameNode.empty())
            writeTraceMarker(mCtx->getTraceFd(), mNameNode.c_str(), kEnd);
        writeTraceMarker(mCtx->getTraceFd(), mNameMain, kEnd);
    }
}

void XTracer0Scoped::sub(const char* name) noexcept
{
    if (!mActive)
        return;

    if (mTimer && mCtx->shouldPrint(mTimer->getLevel() + 1)) {
        mTimer->sub(name);
        if (!mNameNode.empty())
            writeTraceMarker(mCtx->getTraceFd(), mNameNode.c_str(), kEnd);
        mNameNode = name;
        writeTraceMarker(mCtx->getTraceFd(), name, kBegin);
    }
}

void XTracer0Scoped::sub() noexcept
{
    if (!mActive)
        return;

    if (mTimer) {
        mTimer->sub();
        if (!mNameNode.empty()) {
            writeTraceMarker(mCtx->getTraceFd(), mNameNode.c_str(), kEnd);
            mNameNode.clear();
        }
    }
}

float XTracer0Scoped::elapsed() const noexcept { return mTimer ? mTimer->elapsed() : 0.f; }

}  // namespace perf
}  // namespace au
