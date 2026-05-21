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
        auto& cfg = au::perf::PerfConfig::get();
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
//  2. Disabled — every scope is a no-op
// ===========================================================================

TEST_F(XTracer5Test, DisabledHardOff)
{
    au::perf::PerfConfig::get().setEnabled(false);

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
    auto& cfg = au::perf::PerfConfig::get();

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
//  5. Composite macro AU_PERF5_SCOPE wires both timer and tracer
// ===========================================================================

TEST_F(XTracer5Test, CompositeMacroSafe)
{
    au::perf::PerfConfig::get().setTimerLevel(au::perf::kPerfLevelAll5);

    {
        AU_PERF5_SCOPE(std::string("composite.tracer.scope"));
        au::perf::XTimer5::sleepFor(1);
    }
    SUCCEED();
}

// ===========================================================================
//  6. Multi-thread tracer scopes do not crash
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
//  7. Long-name truncation does not crash (truncated to kMaxName=128)
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
//  8. Temporary std::string label does not dangle
// ===========================================================================

TEST_F(XTracer5Test, TemporaryStringNameNoDangle)
{
    {
        au::perf::XTracer5Scoped s(std::string("temp.tracer.") + std::to_string(7));
    }
    SUCCEED();
}

// ===========================================================================
//  9. Depth counter symmetric: many sequential scopes do not exhaust cap
// ===========================================================================

TEST_F(XTracer5Test, DepthCounterDecrementsSymmetrically)
{
    for (int i = 0; i < 1024; ++i) {
        au::perf::XTracer5Scoped s(std::string("seq"));
    }
    SUCCEED();
}

// ===========================================================================
//  10. Nested-scope depth symmetry: outer sub works after inner destruction
// ===========================================================================

TEST_F(XTracer5Test, NestedScopeDepthSymmetry)
{
    for (int cycle = 0; cycle < 32; ++cycle) {
        au::perf::XTracer5Scoped outer(std::string("outer.nest"));
        outer.sub(std::string("before.inner"));
        au::perf::XTimer5::sleepFor(0);
        {
            au::perf::XTracer5Scoped inner(std::string("inner.nest"));
            inner.sub(std::string("inner.phase"));
            au::perf::XTimer5::sleepFor(0);
        }
        outer.sub(std::string("after.inner"));
        au::perf::XTimer5::sleepFor(0);
        outer.sub();
    }
    SUCCEED();
}

// ===========================================================================
//  11. Bare sub() before first named sub() — must be a safe no-op
// ===========================================================================

TEST_F(XTracer5Test, BareSubBeforeFirstNamedSub)
{
    {
        au::perf::XTracer5Scoped s(std::string("bare.first"));
        s.sub();
        s.sub();
        s.sub(std::string("first.real.phase"));
        au::perf::XTimer5::sleepFor(1);
        s.sub();
    }
    SUCCEED();
}

// ===========================================================================
//  12. Alternating named / bare sub — multiple open/close cycles
// ===========================================================================

TEST_F(XTracer5Test, AlternatingSubMultiCycle)
{
    au::perf::PerfConfig::get().setTracerLevel(au::perf::kPerfLevelAll5);

    {
        au::perf::XTracer5Scoped s(std::string("alt.root"));
        for (int i = 0; i < 8; ++i) {
            s.sub(std::string("phase.") + std::to_string(i));
            au::perf::XTimer5::sleepFor(0);
            s.sub();
        }
    }
    SUCCEED();
}

// ===========================================================================
//  13. Empty name and empty sub-name — boundary on fixed inline buffer
// ===========================================================================

TEST_F(XTracer5Test, EmptyNameAndSubName)
{
    {
        au::perf::XTracer5Scoped s(std::string(""));
    }
    {
        au::perf::XTracer5Scoped s(std::string("root.with.empty.sub"));
        s.sub(std::string(""));
        au::perf::XTimer5::sleepFor(0);
        s.sub();
    }
    SUCCEED();
}

// ===========================================================================
//  14. Same-thread multiple independent XTracer5Scoped instances
// ===========================================================================

TEST_F(XTracer5Test, SameThreadMultipleInstances)
{
    {
        au::perf::XTracer5Scoped a(std::string("tracer.a"));
        au::perf::XTracer5Scoped b(std::string("tracer.b"));
        a.sub(std::string("a.phase"));
        au::perf::XTimer5::sleepFor(0);
        a.sub();
        b.sub(std::string("b.phase"));
        au::perf::XTimer5::sleepFor(0);
        b.sub();
    }
    SUCCEED();
}

#endif  // ENABLE_TEST_XTRACER5
