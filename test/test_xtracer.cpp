#if ENABLE_TEST_XTRACER

#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "perf/xperf_macros.h"
#include "perf/xtimer.h"
#include "perf/xtracer.h"

// ---------------------------------------------------------------------------
//  Fixture
// ---------------------------------------------------------------------------

class XTracerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        au::log::Config::get().setTag("XTracerTest");
        au::log::Config::get().setLevel(au::log::Level::Verbose);
        au::log::Config::get().setColorEnabled(false);
#if AU_OS_ANDROID
        au::log::Config::get().setShellPrintEnabled(true);
#endif
        auto& cfg = au::perf::PerfConfig::get();
        cfg.setEnabled(true);
        cfg.setMode(au::perf::Mode::Release);
        cfg.setTimerLevel(au::perf::kPerfLevelOff);  // silence timer noise
        cfg.setTracerLevel(au::perf::kPerfLevelAll);
        cfg.setAggregateMode(false);
        cfg.setRootName("perf");
    }
};

TEST_F(XTracerTest, BasicScopeNoCrash)
{
    {
        au::perf::XTracerScoped s(std::string("trace.basic"));
        au::perf::XTimer::sleepFor(1);
    }
    SUCCEED();
}

TEST_F(XTracerTest, DisabledHardOff)
{
    au::perf::PerfConfig::get().setEnabled(false);

    {
        au::perf::XTracerScoped a(std::string("off.tracer.a"));
        au::perf::XTracerScoped b(std::string("off.tracer.b"));
    }
    SUCCEED();
}

TEST_F(XTracerTest, LevelGating)
{
    auto& cfg = au::perf::PerfConfig::get();

    cfg.setTracerLevel(au::perf::kPerfLevelOff);
    {
        au::perf::XTracerScoped s(std::string("never.traced"));
    }

    cfg.setTracerLevel(0);
    {
        au::perf::XTracerScoped s(std::string("traced"));
    }

    cfg.setTracerLevel(au::perf::kPerfLevelAll);
    SUCCEED();
}

TEST_F(XTracerTest, SubPhaseTransitions)
{
    {
        au::perf::XTracerScoped root(std::string("root.tracer"));
        root.sub(std::string("phase1"));
        au::perf::XTimer::sleepFor(1);
        root.sub(std::string("phase2"));
        au::perf::XTimer::sleepFor(1);
        root.sub();
    }
    SUCCEED();
}

TEST_F(XTracerTest, CompositeMacroSafe)
{
    au::perf::PerfConfig::get().setTimerLevel(au::perf::kPerfLevelAll);

    {
        AU_PERF_SCOPE(std::string("composite.tracer.scope"));
        au::perf::XTimer::sleepFor(1);
    }
    SUCCEED();
}

TEST_F(XTracerTest, MultiThreadStress)
{
    constexpr int            kThreads = 4;
    std::vector<std::thread> ths;
    ths.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ths.emplace_back([] {
            for (int j = 0; j < 8; ++j) {
                au::perf::XTracerScoped s(std::string("mt.tracer"));
                au::perf::XTimer::sleepFor(1);
            }
        });
    }
    for (auto& t : ths) {
        t.join();
    }
    SUCCEED();
}

TEST_F(XTracerTest, LongNameTruncationSafe)
{
    std::string huge(2000, 'X');
    {
        au::perf::XTracerScoped s(huge);
    }
    SUCCEED();
}

TEST_F(XTracerTest, TemporaryStringNameNoDangle)
{
    {
        au::perf::XTracerScoped s(std::string("temp.tracer.") + std::to_string(7));
    }
    SUCCEED();
}

TEST_F(XTracerTest, DepthCounterDecrementsSymmetrically)
{
    for (int i = 0; i < 1024; ++i) {
        au::perf::XTracerScoped s(std::string("seq"));
    }
    SUCCEED();
}

TEST_F(XTracerTest, NestedScopeDepthSymmetry)
{
    for (int cycle = 0; cycle < 32; ++cycle) {
        au::perf::XTracerScoped outer(std::string("outer.nest"));
        outer.sub(std::string("before.inner"));
        au::perf::XTimer::sleepFor(0);
        {
            au::perf::XTracerScoped inner(std::string("inner.nest"));
            inner.sub(std::string("inner.phase"));
            au::perf::XTimer::sleepFor(0);
        }
        outer.sub(std::string("after.inner"));
        au::perf::XTimer::sleepFor(0);
        outer.sub();
    }
    SUCCEED();
}

TEST_F(XTracerTest, BareSubBeforeFirstNamedSub)
{
    {
        au::perf::XTracerScoped s(std::string("bare.first"));
        s.sub();
        s.sub();
        s.sub(std::string("first.real.phase"));
        au::perf::XTimer::sleepFor(1);
        s.sub();
    }
    SUCCEED();
}

TEST_F(XTracerTest, AlternatingSubMultiCycle)
{
    au::perf::PerfConfig::get().setTracerLevel(au::perf::kPerfLevelAll);

    {
        au::perf::XTracerScoped s(std::string("alt.root"));
        for (int i = 0; i < 8; ++i) {
            s.sub(std::string("phase.") + std::to_string(i));
            au::perf::XTimer::sleepFor(0);
            s.sub();
        }
    }
    SUCCEED();
}

TEST_F(XTracerTest, EmptyNameAndSubName)
{
    {
        au::perf::XTracerScoped s(std::string(""));
    }
    {
        au::perf::XTracerScoped s(std::string("root.with.empty.sub"));
        s.sub(std::string(""));
        au::perf::XTimer::sleepFor(0);
        s.sub();
    }
    SUCCEED();
}

TEST_F(XTracerTest, SameThreadMultipleInstances)
{
    {
        au::perf::XTracerScoped a(std::string("tracer.a"));
        au::perf::XTracerScoped b(std::string("tracer.b"));
        a.sub(std::string("a.phase"));
        au::perf::XTimer::sleepFor(0);
        a.sub();
        b.sub(std::string("b.phase"));
        au::perf::XTimer::sleepFor(0);
        b.sub();
    }
    SUCCEED();
}

#endif  // ENABLE_TEST_XTRACER
