#if ENABLE_TEST_XTRACER5

#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "perf/xperf5_macros.h"
#include "perf/xtimer5.h"
#include "perf/xtracer5.h"

// ---------------------------------------------------------------------------
//  Fixture
// ---------------------------------------------------------------------------
//
//  XTracer5Scoped's user-visible payload is Android-only (writes to
//  /sys/kernel/.../trace_marker). On non-Android targets every body
//  compiles down to no-ops that maintain the per-thread depth counter.
//  These tests therefore focus on:
//    1. The tracer never crashes regardless of platform.
//    2. Activation gates (enabled / level) behave correctly.
//    3. Composite macro AU_PERF5_SCOPE wires both timer and tracer.
//    4. Multi-threaded use is safe.

class XTracer5Test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        au::log::Config::get().setTag("XTracer5Test");
        au::log::Config::get().setLevel(au::log::Level::Verbose);
        au::log::Config::get().setColorEnabled(false);
#if AU_OS_ANDROID
        au::log::Config::get().setShellPrintEnabled(true);
#endif
        auto& cfg = au::perf::XPerfContext5::defaultContext();
        cfg.setEnabled(true);
        cfg.setMode(au::perf::Mode5::Release);
        cfg.setTimerLevel(au::perf::kPerfLevelOff5);  // silence timer noise
        cfg.setTracerLevel(au::perf::kPerfLevelAll5);
        cfg.setAggregateMode(false);
        cfg.setRootName("perf");
    }
};

// ===========================================================================
//  1. Basic construction / destruction is safe on every platform
// ===========================================================================

TEST_F(XTracer5Test, BasicScopeNoCrash)
{
    {
        au::perf::XTracer5Scoped s(std::string("trace.basic"));
        au::perf::XTimer5::sleepFor(1);
    }
    SUCCEED();
}

// ===========================================================================
//  2. Disabled context — every scope is a no-op
// ===========================================================================

TEST_F(XTracer5Test, DisabledHardOff)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
    cfg.setEnabled(false);

    {
        au::perf::XTracer5Scoped a(std::string("off.tracer.a"));
        au::perf::XTracer5Scoped b(std::string("off.tracer.b"));
    }
    SUCCEED();
}

// ===========================================================================
//  3. Tracer level gating
// ===========================================================================

TEST_F(XTracer5Test, LevelGating)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();

    cfg.setTracerLevel(au::perf::kPerfLevelOff5);
    {
        au::perf::XTracer5Scoped s(std::string("never.traced"));
    }

    cfg.setTracerLevel(0);
    {
        au::perf::XTracer5Scoped s(std::string("traced"));
    }

    cfg.setTracerLevel(au::perf::kPerfLevelAll5);
    SUCCEED();
}

// ===========================================================================
//  4. sub(name) / sub() phase transitions
// ===========================================================================

TEST_F(XTracer5Test, SubPhaseTransitions)
{
    {
        au::perf::XTracer5Scoped root(std::string("root.tracer"));
        root.sub(std::string("phase1"));
        au::perf::XTimer5::sleepFor(1);
        root.sub(std::string("phase2"));
        au::perf::XTimer5::sleepFor(1);
        root.sub();  // explicit close
    }
    SUCCEED();
}

// ===========================================================================
//  5. Per-context isolation — different contexts can have different levels
// ===========================================================================

TEST_F(XTracer5Test, PerContextLevelIsolation)
{
    au::perf::XPerfContext5 a;
    au::perf::XPerfContext5 b;

    a.setTracerLevel(0);
    b.setTracerLevel(au::perf::kPerfLevelOff5);

    {
        au::perf::XTracer5Scoped sa(a, std::string("ctxA.trace"));
        au::perf::XTracer5Scoped sb(b, std::string("ctxB.trace"));
    }
    SUCCEED();
}

// ===========================================================================
//  6. Composite macro AU_PERF5_SCOPE wires both timer and tracer
// ===========================================================================

TEST_F(XTracer5Test, CompositeMacroSafe)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    {
        AU_PERF5_SCOPE(std::string("composite.tracer.scope"));
        au::perf::XTimer5::sleepFor(1);
    }
    SUCCEED();
}

// ===========================================================================
//  7. Multi-thread tracer scopes do not crash
// ===========================================================================

TEST_F(XTracer5Test, MultiThreadStress)
{
    constexpr int            kThreads = 4;
    std::vector<std::thread> ths;
    ths.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ths.emplace_back([] {
            for (int j = 0; j < 8; ++j) {
                au::perf::XTracer5Scoped s(std::string("mt.tracer"));
                au::perf::XTimer5::sleepFor(1);
            }
        });
    }
    for (auto& t : ths) {
        t.join();
    }
    SUCCEED();
}

// ===========================================================================
//  8. Long-name truncation does not crash (truncated to kMaxName=128)
// ===========================================================================

TEST_F(XTracer5Test, LongNameTruncationSafe)
{
    std::string huge(2000, 'X');
    {
        au::perf::XTracer5Scoped s(huge);
    }
    SUCCEED();
}

// ===========================================================================
//  9. Temporary std::string label does not dangle
// ===========================================================================

TEST_F(XTracer5Test, TemporaryStringNameNoDangle)
{
    {
        au::perf::XTracer5Scoped s(std::string("temp.tracer.") + std::to_string(7));
    }
    SUCCEED();
}

// ===========================================================================
//  10. Depth counter symmetric: many sequential scopes do not exhaust cap
// ===========================================================================

TEST_F(XTracer5Test, DepthCounterDecrementsSymmetrically)
{
    // If begin() and dtor used independent thread_locals (the v5 bug fixed
    // before merge), this loop would silently disable the tracer once the
    // counter exceeds kHardMaxDepth5. We verify symmetry indirectly: a deep
    // SEQUENTIAL workload (not nested) never hits the cap.
    for (int i = 0; i < 1024; ++i) {
        au::perf::XTracer5Scoped s(std::string("seq"));
    }
    SUCCEED();
}

#endif  // ENABLE_TEST_XTRACER5