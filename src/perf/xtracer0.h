#ifndef AURA_PERF_XTRACER0_H_
#define AURA_PERF_XTRACER0_H_

/**
 * @file xtracer0.h
 * @brief Scoped performance tracer with Android perfetto/systrace integration.
 *
 * On Android, writes to the kernel trace_marker file for systrace/perfetto.
 * On other platforms, only the timer component is active (no trace output).
 *
 * XTracer0Context extends XTimer0Context with a cached trace fd.
 * XTracer0Scoped combines an XTimer0Scoped with trace_marker writes.
 *
 * Level independence:
 *   XTracer0Context and XTimer0Context have separate level settings.
 *   Use ctx.setLevel(N) for trace verbosity (perfetto handles deep nesting well).
 *   Use ctx.timerContext().setLevel(M) for timer output verbosity separately.
 *
 * @example
 *   auto& tctx = au::perf::XTracer0Context::get("myLib");
 *   tctx.setEnabled(true);
 *   tctx.setLevel(2);        // trace level
 *   tctx.timerContext().setLevel(1);  // timer log level
 *   {
 *       au::perf::XTracer0Scoped t("processFrame", &tctx);
 *       t.sub("preprocess");
 *       // ... work ...
 *       t.sub("inference");
 *       // ... work ...
 *   }
 */

#include <memory>
#include <string>

#include "perf/xtimer0.h"

namespace au {
namespace perf {

// ============================================================================
// XTracer0Context
// ============================================================================

class XTracer0Context
{
public:
    static XTracer0Context& get(const char* name);
    static XTracer0Context& global();

    XTracer0Context();

    // ── delegation to timer context ──

    XTimer0Context& timerContext() noexcept { return *mTimerCtx; }

    void setEnabled(bool on) noexcept { mTimerCtx->setEnabled(on); }
    bool isEnabled() const noexcept { return mTimerCtx->isEnabled(); }

    // Tracer's own level (independent of timer level)
    void setLevel(int level) noexcept { mLevel.store(level, std::memory_order_relaxed); }
    int  getLevel() const noexcept { return mLevel.load(std::memory_order_relaxed); }

    void                 setMode(XTimer0Context::Mode mode) noexcept { mTimerCtx->setMode(mode); }
    XTimer0Context::Mode getMode() const noexcept { return mTimerCtx->getMode(); }

    void configureFrom(const char* propertyPrefix) noexcept;

    bool shouldPrint(int level) const noexcept
    {
        if (!isEnabled())
            return false;
        int lim = getLevel();
        return lim < 0 || level <= lim;
    }

    // ── trace fd (internal) ──

    int getTraceFd() const noexcept { return mFdTrace; }

private:
    XTimer0Context*  mTimerCtx;
    std::atomic<int> mLevel{-1};
    int              mFdTrace;
};

// ============================================================================
// XTracer0Scoped
// ============================================================================

class XTracer0Scoped
{
public:
    explicit XTracer0Scoped(const char* name, XTracer0Context* ctx = nullptr) noexcept;
    ~XTracer0Scoped() noexcept;

    XTracer0Scoped(const XTracer0Scoped&)            = delete;
    XTracer0Scoped& operator=(const XTracer0Scoped&) = delete;

    void  sub(const char* name) noexcept;
    void  sub() noexcept;
    float elapsed() const noexcept;

private:
    void writeTraceBegin(const char* name) noexcept;
    void writeTraceEnd() noexcept;

    XTracer0Context*               mCtx;
    std::unique_ptr<XTimer0Scoped> mTimer;
    const char*                    mNameMain;
    std::string                    mNameNode;
    bool                           mActive;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTRACER0_H_
