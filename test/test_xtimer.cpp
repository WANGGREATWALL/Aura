#if ENABLE_TEST_XTIMER

#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "perf/xperf_macros.h"
#include "perf/xtimer.h"

class StdoutCapture
{
public:
    StdoutCapture() noexcept { ::testing::internal::CaptureStdout(); }
    ~StdoutCapture() noexcept
    {
        if (!mDrained) {
            (void)::testing::internal::GetCapturedStdout();
        }
    }

    std::string drain()
    {
        mDrained = true;
        return ::testing::internal::GetCapturedStdout();
    }

private:
    bool mDrained{false};
};

// ---------------------------------------------------------------------------
//  Fixture
// ---------------------------------------------------------------------------

class XTimerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        au::log::Config::get().setTag("XTimerTest");
        au::log::Config::get().setLevel(au::log::Level::Verbose);
        au::log::Config::get().setColorEnabled(false);
#if AU_OS_ANDROID
        au::log::Config::get().setShellPrintEnabled(true);
#endif
        auto& cfg = au::perf::Config::get();
        cfg.setEnabled(true);
        cfg.setDebugMode(false);
        cfg.setTimerLevel(3);
        cfg.setTracerLevel(au::perf::Config::LEVEL_ALL);
        cfg.setAggregateMode(false);
        cfg.setRootName("perf");
    }

    void TearDown() override
    {
        // Defensive cleanup: drain TLS tree to isolate subsequent tests.
        au::log::Config::get().setLevel(au::log::Level::Silent);
        {
            au::perf::XTimerScoped drain("_cleanup_");
        }
        au::log::Config::get().setLevel(au::log::Level::Verbose);
    }

    static int countOccurrences(const std::string& s, const std::string& sub)
    {
        int         cnt = 0;
        std::size_t pos = s.find(sub);
        while (pos != std::string::npos) {
            ++cnt;
            pos = s.find(sub, pos + sub.size());
        }
        return cnt;
    }
};

TEST_F(XTimerTest, ConfigAccessors)
{
    auto& cfg = au::perf::Config::get();

    cfg.setEnabled(false);
    EXPECT_FALSE(cfg.isEnabled());
    cfg.setEnabled(true);
    EXPECT_TRUE(cfg.isEnabled());

    cfg.setDebugMode(true);
    EXPECT_TRUE(cfg.isDebugMode());

    cfg.setTimerLevel(7);
    EXPECT_EQ(cfg.getTimerLevel(), 7);
    cfg.setTimerLevel(au::perf::Config::LEVEL_OFF);
    EXPECT_EQ(cfg.getTimerLevel(), au::perf::Config::LEVEL_OFF);

    cfg.setTracerLevel(0);
    EXPECT_EQ(cfg.getTracerLevel(), 0);

    cfg.setRootName("AlgoRoot");
    EXPECT_EQ(cfg.getRootName(), "AlgoRoot");

    cfg.setAggregateMode(true);
    EXPECT_TRUE(cfg.isAggregateMode());
    cfg.setAggregateMode(false);
    EXPECT_FALSE(cfg.isAggregateMode());
}

// ---------------------------------------------------------------------------
//  XTimer — bare stopwatch
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, TimerElapsed)
{
    au::perf::XTimer t;
    au::perf::XTimer::sleepFor(10);
    const float ms = t.elapsedMs();
    EXPECT_GT(ms, 5.0f);
    EXPECT_LT(ms, 500.0f) << "elapsedMs=" << ms;
}

TEST_F(XTimerTest, TimerSleepNonPositiveNoOp)
{
    au::perf::XTimer t;
    au::perf::XTimer::sleepFor(0);
    au::perf::XTimer::sleepFor(-5);
    EXPECT_LT(t.elapsedMs(), 50.0f);
}

TEST_F(XTimerTest, TimerGetTimeFormatted)
{
    const std::string s = au::perf::XTimer::getTimeFormatted("%Y-%m-%d-%H-%M-%S");
    EXPECT_GE(s.size(), 21u);
    EXPECT_NE(s.find('_'), std::string::npos);
}

// ---------------------------------------------------------------------------
//  Release mode
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, SubReleaseImmediateOutput)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped root(std::string("relPipeline"));
        root.sub(std::string("phase1"));
        au::perf::XTimer::sleepFor(2);
        root.sub(std::string("phase2"));
        au::perf::XTimer::sleepFor(2);
        root.sub();
    }
    const std::string out = cap.drain();

    EXPECT_NE(out.find("phase1"), std::string::npos) << out;
    EXPECT_NE(out.find("phase2"), std::string::npos) << out;
    EXPECT_NE(out.find("relPipeline"), std::string::npos) << out;
    EXPECT_LT(out.find("phase1"), out.find("phase2"));
}

// ---------------------------------------------------------------------------
//  Debug mode
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, DebugModeTreeHierarchy)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped root(std::string("root5"));
        {
            au::perf::XTimerScoped a(std::string("childA5"));
            {
                au::perf::XTimerScoped c(std::string("grand5"));
                au::perf::XTimer::sleepFor(1);
            }
        }
        {
            au::perf::XTimerScoped b(std::string("childB5"));
            au::perf::XTimer::sleepFor(1);
        }
    }
    const std::string out = cap.drain();

    EXPECT_NE(out.find("[perf]"), std::string::npos) << out;
    EXPECT_NE(out.find("tid="), std::string::npos);
    EXPECT_EQ(countOccurrences(out, "root5"), 1);
    EXPECT_EQ(countOccurrences(out, "childA5"), 1);
    EXPECT_EQ(countOccurrences(out, "childB5"), 1);
    EXPECT_EQ(countOccurrences(out, "grand5"), 1);
    EXPECT_NE(out.find("|--"), std::string::npos);
    EXPECT_NE(out.find("`--"), std::string::npos);
    EXPECT_NE(out.find(": "), std::string::npos);
    EXPECT_EQ(out.find("\xE2"), std::string::npos);
}

TEST_F(XTimerTest, SubAndSubNamedDebug)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped root(std::string("pipeline5"));
        root.sub(std::string("step1"));
        au::perf::XTimer::sleepFor(1);
        root.sub(std::string("step2"));
        au::perf::XTimer::sleepFor(1);
        root.sub();
    }
    const std::string out = cap.drain();

    EXPECT_EQ(countOccurrences(out, "pipeline5"), 1);
    EXPECT_EQ(countOccurrences(out, "step1"), 1);
    EXPECT_EQ(countOccurrences(out, "step2"), 1);
    EXPECT_EQ(out.find("(open)"), std::string::npos);
}

TEST_F(XTimerTest, WideTreeSiblingOrdering)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped root(std::string("wide"));
        for (int i = 0; i < 20; ++i) {
            root.sub(std::string("s") + std::to_string(i));
            au::perf::XTimer::sleepFor(0);
        }
        root.sub();
    }
    const std::string out = cap.drain();

    for (int i = 0; i < 20; ++i) {
        const std::string marker = std::string("s") + std::to_string(i) + ":";
        EXPECT_NE(out.find(marker), std::string::npos) << "missing " << marker;
    }
    for (int i = 0; i < 19; ++i) {
        const std::string a = std::string("s") + std::to_string(i) + ":";
        const std::string b = std::string("s") + std::to_string(i + 1) + ":";
        EXPECT_LT(out.find(a), out.find(b)) << a << " must precede " << b;
    }
}

// ---------------------------------------------------------------------------
//  Config gates
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, LevelFiltering)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);

    cfg.setTimerLevel(0);
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped root(std::string("depth0"));
            {
                au::perf::XTimerScoped child(std::string("depth1"));
            }
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("depth0"), std::string::npos);
        EXPECT_EQ(out.find("depth1"), std::string::npos) << "depth>level must be silent";
    }

    cfg.setTimerLevel(au::perf::Config::LEVEL_OFF);
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped s(std::string("never5"));
        }
        EXPECT_EQ(cap.drain().find("never5"), std::string::npos);
    }
}

TEST_F(XTimerTest, DisabledHardOff)
{
    auto& cfg = au::perf::Config::get();
    cfg.setEnabled(false);
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped a(std::string("off5.a"));
        au::perf::XTimerScoped b(std::string("off5.b"));
    }
    EXPECT_TRUE(cap.drain().empty());
}

// ---------------------------------------------------------------------------
//  Multi-thread & aggregate
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, MultiThreadIsolation)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);
    cfg.setAggregateMode(false);

    constexpr int kThreads = 4;
    StdoutCapture cap;
    {
        std::vector<std::thread> ths;
        ths.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            ths.emplace_back([] {
                au::perf::XTimerScoped root(std::string("rootMT5"));
                for (int j = 0; j < 3; ++j) {
                    au::perf::XTimerScoped child(std::string("childMT5"));
                    au::perf::XTimer::sleepFor(1);
                }
            });
        }
        for (auto& t : ths) {
            t.join();
        }
    }
    const std::string out = cap.drain();

    const int headerCount = countOccurrences(out, "[perf][tid=");
    EXPECT_EQ(headerCount, kThreads);
    EXPECT_EQ(countOccurrences(out, "rootMT5"), kThreads);
}

TEST_F(XTimerTest, AggregateModeFlush)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);
    cfg.setAggregateMode(true);

    {
        StdoutCapture capDuring;
        {
            std::thread worker([] {
                au::perf::XTimerScoped r(std::string("agg5.worker"));
                au::perf::XTimer::sleepFor(1);
            });
            worker.join();
            au::perf::XTimerScoped r(std::string("agg5.main"));
        }
        const std::string outDuring = capDuring.drain();
        EXPECT_EQ(outDuring.find("agg5.worker"), std::string::npos);
        EXPECT_EQ(outDuring.find("agg5.main"), std::string::npos);
    }

    {
        StdoutCapture capFlush;
        cfg.flushAggregated();
        const std::string outFlush = capFlush.drain();
        EXPECT_NE(outFlush.find("agg5.worker"), std::string::npos);
        EXPECT_NE(outFlush.find("agg5.main"), std::string::npos);
        EXPECT_NE(outFlush.find("aggregate flush"), std::string::npos);
    }

    // Second flush must be idempotent.
    {
        StdoutCapture capSecond;
        cfg.flushAggregated();
        EXPECT_TRUE(capSecond.drain().empty());
    }

    cfg.setAggregateMode(false);
}

// ---------------------------------------------------------------------------
//  Exception & corruption safety
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, NameCorruptionDefense)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    // Embedded newline.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped s(std::string("line1\nline2"));
            au::perf::XTimer::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("[perf]"), std::string::npos);
    }

    // printf format specifiers — must render literally.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped s(std::string("%s%d%p_test"));
            au::perf::XTimer::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("%s%d%p_test"), std::string::npos);
    }

    // Tab and backslash.
    {
        au::perf::XTimerScoped s(std::string("tab\there\\path"));
        SUCCEED();
    }

    // UTF-8 multi-byte sequence.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped s(std::string("utf8_\xc3\xa4\xc3\xb6"));
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("utf8_"), std::string::npos);
    }

    // ANSI escape codes embedded in name.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped s(std::string("\033[31mRED\033[0m"));
            au::perf::XTimer::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("[perf]"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
//  Temporal & formatting
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, TemporaryStringNameNoDangle)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    {
        au::perf::XTimerScoped s(std::string("temp.name.") + std::to_string(42));
        au::perf::XTimer::sleepFor(1);
    }
    const std::string out = cap.drain();
    EXPECT_NE(out.find("temp.name.42"), std::string::npos) << out;
}

TEST_F(XTimerTest, ColorOutput)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    // Color enabled: ANSI escape codes present, reset count ≥ line count.
    au::log::Config::get().setColorEnabled(true);
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped root(std::string("color_test"));
            au::perf::XTimer::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("\033["), std::string::npos) << out;
        const int lines  = countOccurrences(out, "\n");
        const int resets = countOccurrences(out, "\033[0m");
        EXPECT_GE(resets, lines) << "dangling color state — fewer resets than lines";
        EXPECT_NE(out.find("color_test"), std::string::npos);
    }

    // Color disabled: no ANSI escape sequences.
    au::log::Config::get().setColorEnabled(false);
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped root(std::string("nocolor"));
            au::perf::XTimer::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_EQ(out.find("\033["), std::string::npos) << out;
        EXPECT_NE(out.find("nocolor"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
//  Convenience macros
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, ConvenienceMacros)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    {
        StdoutCapture cap;
        {
            AU_TIMER(std::string("macro5.basic"));
            au::perf::XTimer::sleepFor(1);
        }
        EXPECT_NE(cap.drain().find("macro5.basic"), std::string::npos);
    }

    // __COUNTER__ disambiguation: two macros on the same source line.
    {
        StdoutCapture cap;
        {
            AU_TIMER(std::string("same.A"));
            AU_TIMER(std::string("same.B"));
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("same.A"), std::string::npos);
        EXPECT_NE(out.find("same.B"), std::string::npos);
    }

    // Composite macro.
    {
        StdoutCapture cap;
        {
            AU_PERF_SCOPE(std::string("composite.scope"));
            au::perf::XTimer::sleepFor(1);
        }
        EXPECT_NE(cap.drain().find("composite.scope"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
//  Stress & edge cases
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, TimerRestartElapsed)
{
    au::perf::XTimer t;
    au::perf::XTimer::sleepFor(20);
    EXPECT_GT(t.elapsedMs(), 10.0f);

    t.restart();
    EXPECT_LT(t.elapsedMs(), 10.0f) << "restart() must reset elapsed to near-zero";

    au::perf::XTimer::sleepFor(10);
    const float after = t.elapsedMs();
    EXPECT_GT(after, 5.0f);
    EXPECT_LT(after, 100.0f);
}

TEST_F(XTimerTest, HardDepthCapNoCrash)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture            cap;
    std::function<void(int)> recurse = [&](int n) {
        if (n <= 0)
            return;
        au::perf::XTimerScoped s(std::string("d"));
        recurse(n - 1);
    };
    recurse(static_cast<int>(au::perf::Config::HARD_MAX_DEPTH) + 16);
    (void)cap.drain();
    SUCCEED();
}

TEST_F(XTimerTest, StressTenThousandScopes)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(false);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    StdoutCapture cap;
    auto          begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        au::perf::XTimerScoped s(std::string("stress"));
    }
    auto end = std::chrono::steady_clock::now();
    (void)cap.drain();

    const double nsPerOp = std::chrono::duration<double, std::nano>(end - begin).count() / 10000.0;
    EXPECT_LT(nsPerOp, 50000.0) << "per scope = " << nsPerOp << " ns";
}

TEST_F(XTimerTest, StressHundredThousandScopesDebug)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    {
        for (int i = 0; i < 100000; ++i) {
            au::perf::XTimerScoped s(std::string("memtest"));
        }
    }
    au::log::Config::get().setLevel(au::log::Level::Verbose);
    SUCCEED();
}

TEST_F(XTimerTest, StressMixedPatterns)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    {
        for (int i = 0; i < 1000; ++i) {
            {
                au::perf::XTimerScoped s(std::string("flat"));
            }
            {
                au::perf::XTimerScoped s(std::string("subs"));
                s.sub(std::string("a"));
                s.sub();
            }
            {
                au::perf::XTimerScoped o(std::string("outer"));
                {
                    au::perf::XTimerScoped in(std::string("inner"));
                }
            }
        }
    }
    au::log::Config::get().setLevel(au::log::Level::Verbose);
    SUCCEED();
}

// ---------------------------------------------------------------------------
//  Exception safety — must run last to avoid TLS tree contamination
// ---------------------------------------------------------------------------

TEST_F(XTimerTest, SubStateExceptionRecovery)
{
    auto& cfg = au::perf::Config::get();
    cfg.setDebugMode(true);
    cfg.setTimerLevel(au::perf::Config::LEVEL_ALL);

    // Unbalanced subs: extra bare sub() must be safe no-ops.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped root(std::string("unbalanced"));
            root.sub(std::string("A"));
            au::perf::XTimer::sleepFor(1);
            root.sub();
            root.sub();  // extra bare sub → no-op
            root.sub();  // extra bare sub → no-op
            root.sub(std::string("B"));
            au::perf::XTimer::sleepFor(1);
            root.sub();
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("A:"), std::string::npos) << out;
        EXPECT_NE(out.find("B:"), std::string::npos) << out;
        EXPECT_LT(out.find("A:"), out.find("B:"));
    }

    // Bare sub before first named sub: safe no-op.
    {
        StdoutCapture cap;
        {
            au::perf::XTimerScoped root(std::string("bare_first"));
            root.sub();
            root.sub();
            root.sub(std::string("after"));
            au::perf::XTimer::sleepFor(1);
            root.sub();
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("after:"), std::string::npos) << out;
    }

    // Exception between sub() calls: destructor must close the active sub
    // before closing the root so the tree can still flush.
    {
        StdoutCapture cap;
        bool caught = false;
        try {
            au::perf::XTimerScoped root(std::string("mid_sub"));
            root.sub(std::string("A"));
            throw std::runtime_error("mid");
        } catch (const std::exception&) {
            caught = true;
        }
        EXPECT_TRUE(caught);
        const std::string out = cap.drain();
        EXPECT_NE(out.find("mid_sub"), std::string::npos) << out;
        EXPECT_NE(out.find("A:"), std::string::npos) << out;
        EXPECT_EQ(out.find("(open)"), std::string::npos) << out;
    }
}

#endif  // ENABLE_TEST_XTIMER
