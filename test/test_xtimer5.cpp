#if ENABLE_TEST_XTIMER5

#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "perf/xperf5_macros.h"
#include "perf/xtimer5.h"

// ---------------------------------------------------------------------------
//  StdoutCapture — RAII wrapper around testing::internal::CaptureStdout.
// ---------------------------------------------------------------------------

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

class XTimer5Test : public ::testing::Test
{
protected:
    void SetUp() override
    {
        au::log::Config::get().setTag("XTimer5Test");
        au::log::Config::get().setLevel(au::log::Level::Verbose);
        au::log::Config::get().setColorEnabled(false);
#if AU_OS_ANDROID
        au::log::Config::get().setShellPrintEnabled(true);
#endif
        // Reset global config to a deterministic baseline.
        auto& cfg = au::perf::PerfConfig::get();
        cfg.setEnabled(true);
        cfg.setMode(au::perf::Mode5::Release);
        cfg.setTimerLevel(3);
        cfg.setTracerLevel(au::perf::kPerfLevelAll5);
        cfg.setAggregateMode(false);
        cfg.setRootName("perf");
    }

    void TearDown() override
    {
        // Defensive cleanup: if a test leaked unclosed subs / tree nodes,
        // silently drain the TLS tree so subsequent tests start clean.
        au::log::Config::get().setLevel(au::log::Level::Silent);
        {
            au::perf::XTimer5Scoped drain("_cleanup_");
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

// ===========================================================================
//  1. PerfConfig — basic accessors
// ===========================================================================

TEST_F(XTimer5Test, PerfConfigAccessors)
{
    auto& cfg = au::perf::PerfConfig::get();

    cfg.setEnabled(false);
    EXPECT_FALSE(cfg.isEnabled());
    cfg.setEnabled(true);
    EXPECT_TRUE(cfg.isEnabled());

    cfg.setMode(au::perf::Mode5::Debug);
    EXPECT_EQ(cfg.getMode(), au::perf::Mode5::Debug);

    cfg.setTimerLevel(7);
    EXPECT_EQ(cfg.getTimerLevel(), 7);
    cfg.setTimerLevel(au::perf::kPerfLevelOff5);
    EXPECT_EQ(cfg.getTimerLevel(), au::perf::kPerfLevelOff5);

    cfg.setTracerLevel(0);
    EXPECT_EQ(cfg.getTracerLevel(), 0);

    cfg.setRootName("AlgoRoot");
    char name[64] = {0};
    cfg.getRootName(name, sizeof(name));
    EXPECT_STREQ(name, "AlgoRoot");

    cfg.setAggregateMode(true);
    EXPECT_TRUE(cfg.isAggregateMode());
    cfg.setAggregateMode(false);
    EXPECT_FALSE(cfg.isAggregateMode());
}

// ===========================================================================
//  2. RootName — std::string overload + truncation
// ===========================================================================

TEST_F(XTimer5Test, RootNameStringAndTruncation)
{
    auto& cfg = au::perf::PerfConfig::get();

    cfg.setRootName(std::string("literal-root"));
    char buf[64] = {0};
    cfg.getRootName(buf, sizeof(buf));
    EXPECT_STREQ(buf, "literal-root");

    cfg.setRootName(std::string{});
    std::memset(buf, 0xCC, sizeof(buf));
    cfg.getRootName(buf, sizeof(buf));
    EXPECT_STREQ(buf, "");

    std::string huge(200, 'x');
    cfg.setRootName(huge);
    cfg.getRootName(buf, sizeof(buf));
    EXPECT_LE(std::strlen(buf), sizeof(buf) - 1);
    EXPECT_GT(std::strlen(buf), 0u);
}

// ===========================================================================
//  3. XTimer5 — elapsed / restart / sleepFor non-positive
// ===========================================================================

TEST_F(XTimer5Test, TimerElapsed)
{
    au::perf::XTimer5 t;
    au::perf::XTimer5::sleepFor(10);
    const float ms = t.elapsedMs();
    EXPECT_GT(ms, 5.0f);
    EXPECT_LT(ms, 500.0f) << "elapsedMs=" << ms;
}

TEST_F(XTimer5Test, TimerSleepNonPositiveNoOp)
{
    au::perf::XTimer5 t;
    au::perf::XTimer5::sleepFor(0);
    au::perf::XTimer5::sleepFor(-5);
    EXPECT_LT(t.elapsedMs(), 50.0f);
}

TEST_F(XTimer5Test, TimerGetTimeFormatted)
{
    const std::string s = au::perf::XTimer5::getTimeFormatted("%Y-%m-%d-%H-%M-%S");
    EXPECT_GE(s.size(), 21u);
    EXPECT_NE(s.find('_'), std::string::npos);
}

// ===========================================================================
//  4. Release-mode: one-liner per scope, no tree characters
// ===========================================================================

TEST_F(XTimer5Test, ReleaseModeOneLiner)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped s(std::string("rel.scope"));
        au::perf::XTimer5::sleepFor(2);
    }
    const std::string out = cap.drain();

    EXPECT_NE(out.find("[perf5]"), std::string::npos) << out;
    EXPECT_NE(out.find("rel.scope"), std::string::npos) << out;
    EXPECT_NE(out.find("ms"), std::string::npos) << out;
    EXPECT_EQ(out.find("|--"), std::string::npos);
    EXPECT_EQ(out.find("`--"), std::string::npos);
}

// ===========================================================================
//  5. Debug-mode: hierarchical tree with header + ASCII branches + colon
// ===========================================================================

TEST_F(XTimer5Test, DebugModeTreeHierarchy)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped root(std::string("root5"));
        {
            au::perf::XTimer5Scoped a(std::string("childA5"));
            {
                au::perf::XTimer5Scoped c(std::string("grand5"));
                au::perf::XTimer5::sleepFor(1);
            }
        }
        {
            au::perf::XTimer5Scoped b(std::string("childB5"));
            au::perf::XTimer5::sleepFor(1);
        }
    }
    const std::string out = cap.drain();

    EXPECT_NE(out.find("[perf5]"), std::string::npos) << out;
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

// ===========================================================================
//  6. sub(name) / sub() — Debug mode aggregates into tree
// ===========================================================================

TEST_F(XTimer5Test, SubAndSubNamedDebug)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped root(std::string("pipeline5"));
        root.sub(std::string("step1"));
        au::perf::XTimer5::sleepFor(1);
        root.sub(std::string("step2"));
        au::perf::XTimer5::sleepFor(1);
        root.sub();  // explicit close
    }
    const std::string out = cap.drain();

    EXPECT_EQ(countOccurrences(out, "pipeline5"), 1);
    EXPECT_EQ(countOccurrences(out, "step1"), 1);
    EXPECT_EQ(countOccurrences(out, "step2"), 1);
    EXPECT_EQ(out.find("(open)"), std::string::npos);
}

// ===========================================================================
//  7. sub(name) — Release mode prints the previous segment immediately
// ===========================================================================

TEST_F(XTimer5Test, SubReleaseImmediateOutput)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped root(std::string("relPipeline"));
        root.sub(std::string("phase1"));
        au::perf::XTimer5::sleepFor(2);
        root.sub(std::string("phase2"));  // must immediately print "phase1: ..."
        au::perf::XTimer5::sleepFor(2);
        root.sub();  // immediately prints "phase2: ..."
    }
    const std::string out = cap.drain();

    EXPECT_NE(out.find("phase1"), std::string::npos) << out;
    EXPECT_NE(out.find("phase2"), std::string::npos) << out;
    EXPECT_NE(out.find("relPipeline"), std::string::npos) << out;
    EXPECT_LT(out.find("phase1"), out.find("phase2"));
}

// ===========================================================================
//  8. Level filtering: kPerfLevelOff fully silences; threshold gates depth
// ===========================================================================

TEST_F(XTimer5Test, LevelFiltering)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);

    cfg.setTimerLevel(0);
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped root(std::string("depth0"));
            {
                au::perf::XTimer5Scoped child(std::string("depth1"));
            }
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("depth0"), std::string::npos);
        EXPECT_EQ(out.find("depth1"), std::string::npos) << "depth>level must be silent";
    }

    cfg.setTimerLevel(au::perf::kPerfLevelOff5);
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped s(std::string("never5"));
        }
        EXPECT_EQ(cap.drain().find("never5"), std::string::npos);
    }
}

// ===========================================================================
//  9. setEnabled(false) hard-off
// ===========================================================================

TEST_F(XTimer5Test, DisabledHardOff)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setEnabled(false);
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped a(std::string("off5.a"));
        au::perf::XTimer5Scoped b(std::string("off5.b"));
    }
    EXPECT_TRUE(cap.drain().empty());
}

// ===========================================================================
//  10. Multi-thread isolation: each thread one header, no interleave
// ===========================================================================

TEST_F(XTimer5Test, MultiThreadIsolation)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);
    cfg.setAggregateMode(false);

    constexpr int kThreads = 4;
    StdoutCapture cap;
    {
        std::vector<std::thread> ths;
        ths.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            ths.emplace_back([] {
                au::perf::XTimer5Scoped root(std::string("rootMT5"));
                for (int j = 0; j < 3; ++j) {
                    au::perf::XTimer5Scoped child(std::string("childMT5"));
                    au::perf::XTimer5::sleepFor(1);
                }
            });
        }
        for (auto& t : ths) {
            t.join();
        }
    }
    const std::string out = cap.drain();

    const int headerCount = countOccurrences(out, "[perf5][tid=");
    EXPECT_EQ(headerCount, kThreads);
    EXPECT_EQ(countOccurrences(out, "rootMT5"), kThreads);
}

// ===========================================================================
//  11. Aggregate mode: global flush
// ===========================================================================

TEST_F(XTimer5Test, AggregateModeFlush)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);
    cfg.setAggregateMode(true);

    {
        StdoutCapture capDuring;
        {
            std::thread worker([] {
                au::perf::XTimer5Scoped r(std::string("agg5.worker"));
                au::perf::XTimer5::sleepFor(1);
            });
            worker.join();
            au::perf::XTimer5Scoped r(std::string("agg5.main"));
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

    {
        // Second flush must be a no-op (idempotent).
        StdoutCapture capSecond;
        cfg.flushAggregated();
        EXPECT_TRUE(capSecond.drain().empty());
    }

    cfg.setAggregateMode(false);
}

// ===========================================================================
//  12. Exception safety: outermost flush still fires when stack unwinds
// ===========================================================================

TEST_F(XTimer5Test, ExceptionSafety)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    bool          caught = false;
    try {
        au::perf::XTimer5Scoped root(std::string("will.throw5"));
        au::perf::XTimer5Scoped inner(std::string("inner5"));
        throw std::runtime_error("boom");
    } catch (const std::runtime_error& e) {
        caught = true;
        EXPECT_STREQ(e.what(), "boom");
    }
    EXPECT_TRUE(caught);

    const std::string out = cap.drain();
    EXPECT_NE(out.find("will.throw5"), std::string::npos);
    EXPECT_NE(out.find("inner5"), std::string::npos);
}

// ===========================================================================
//  13. Long-name truncation marker
// ===========================================================================

TEST_F(XTimer5Test, LongNameTruncationMarker)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    std::string   longName(1500, 'A');
    {
        au::perf::XTimer5Scoped s(longName);
    }
    const std::string out = cap.drain();
    EXPECT_NE(out.find("(truncated)"), std::string::npos) << out;
    EXPECT_NE(out.find("AAAAAAAA"), std::string::npos);
}

// ===========================================================================
//  14. Temporary std::string name does not dangle
// ===========================================================================

TEST_F(XTimer5Test, TemporaryStringNameNoDangle)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped s(std::string("temp.name.") + std::to_string(42));
        au::perf::XTimer5::sleepFor(1);
    }
    const std::string out = cap.drain();
    EXPECT_NE(out.find("temp.name.42"), std::string::npos) << out;
}

// ===========================================================================
//  15. Convenience macros AU_TIMER5 / AU_PERF5_SCOPE
// ===========================================================================

TEST_F(XTimer5Test, ConvenienceMacros)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    {
        StdoutCapture cap;
        {
            AU_TIMER5(std::string("macro5.basic"));
            au::perf::XTimer5::sleepFor(1);
        }
        EXPECT_NE(cap.drain().find("macro5.basic"), std::string::npos);
    }

    // __COUNTER__ disambiguation: two macros on the same source line.
    {
        StdoutCapture cap;
        {
            AU_TIMER5(std::string("same.A"));
            AU_TIMER5(std::string("same.B"));
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("same.A"), std::string::npos);
        EXPECT_NE(out.find("same.B"), std::string::npos);
    }

    // Composite macro: timer + tracer with the same label.
    {
        StdoutCapture cap;
        {
            AU_PERF5_SCOPE(std::string("composite.scope"));
            au::perf::XTimer5::sleepFor(1);
        }
        EXPECT_NE(cap.drain().find("composite.scope"), std::string::npos);
    }
}

// ===========================================================================
//  16. Stress: 10k scopes — performance smoke + leak guard
// ===========================================================================

TEST_F(XTimer5Test, StressTenThousandScopes)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    auto          begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        au::perf::XTimer5Scoped s(std::string("stress"));
    }
    auto end = std::chrono::steady_clock::now();
    (void)cap.drain();

    const double nsPerOp = std::chrono::duration<double, std::nano>(end - begin).count() / 10000.0;
    EXPECT_LT(nsPerOp, 50000.0) << "per scope = " << nsPerOp << " ns";
}

// ===========================================================================
//  17. Deep-nest hard cap: kHardMaxDepth5 (=512) does not crash
// ===========================================================================

TEST_F(XTimer5Test, HardDepthCapNoCrash)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    std::function<void(int)> recurse = [&](int n) {
        if (n <= 0)
            return;
        au::perf::XTimer5Scoped s(std::string("d"));
        recurse(n - 1);
    };
    recurse(static_cast<int>(au::perf::kHardMaxDepth5) + 16);
    (void)cap.drain();
    SUCCEED();
}

// ===========================================================================
//  18. XTimer5::restart() — resets elapsed to near-zero
// ===========================================================================

TEST_F(XTimer5Test, TimerRestartElapsed)
{
    au::perf::XTimer5 t;
    au::perf::XTimer5::sleepFor(20);
    EXPECT_GT(t.elapsedMs(), 10.0f);

    t.restart();
    EXPECT_LT(t.elapsedMs(), 10.0f) << "restart() must reset elapsed to near-zero";

    au::perf::XTimer5::sleepFor(10);
    const float after = t.elapsedMs();
    EXPECT_GT(after, 5.0f);
    EXPECT_LT(after, 100.0f);
}

// ===========================================================================
//  19. Debug mode: precise indentation for nested scopes and siblings
// ===========================================================================

TEST_F(XTimer5Test, DebugIndentationPrecision)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    // Two siblings under root — verify branch markers and ordering.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped root(std::string("r"));
            root.sub(std::string("A"));
            au::perf::XTimer5::sleepFor(1);
            root.sub(std::string("B"));
            au::perf::XTimer5::sleepFor(1);
            root.sub();  // close trailing sub so tree flushes
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("r: "), std::string::npos) << out;
        EXPECT_NE(out.find("|-- A:"), std::string::npos) << out;
        EXPECT_NE(out.find("`-- B:"), std::string::npos) << out;
        EXPECT_LT(out.find("A:"), out.find("B:"));
    }

    // Nested scopes: root -> inner — verify indent inheritance.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped outer(std::string("outer"));
            {
                au::perf::XTimer5Scoped inner(std::string("inner"));
                au::perf::XTimer5::sleepFor(1);
            }
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("outer: "), std::string::npos) << out;
        EXPECT_NE(out.find("`-- inner:"), std::string::npos) << out;
        EXPECT_LT(out.find("outer:"), out.find("inner:"));
    }

    // 5-level deep nest — deepest node present with accumulated indent.
    {
        StdoutCapture cap;
        {
            std::function<void(int)> nest = [&](int d) {
                if (d >= 5) return;
                au::perf::XTimer5Scoped s(std::string("L") + std::to_string(d));
                au::perf::XTimer5::sleepFor(0);
                nest(d + 1);
            };
            au::perf::XTimer5Scoped root(std::string("deep"));
            nest(0);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("`-- L4:"), std::string::npos) << out;
        for (int i = 0; i < 4; ++i) {
            EXPECT_LT(out.find(std::string("L") + std::to_string(i) + ":"),
                      out.find(std::string("L") + std::to_string(i + 1) + ":"));
        }
    }
}

// ===========================================================================
//  20. Stress: 100k scopes in Debug mode — tree-node allocator leak guard
// ===========================================================================

TEST_F(XTimer5Test, StressHundredThousandScopesDebug)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    {
        for (int i = 0; i < 100000; ++i) {
            au::perf::XTimer5Scoped s(std::string("memtest"));
        }
    }
    au::log::Config::get().setLevel(au::log::Level::Verbose);
    SUCCEED();
}

// ===========================================================================
//  21. Name corruption defense — special characters and format specifiers
// ===========================================================================

TEST_F(XTimer5Test, NameCorruptionDefense)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    // Embedded newline — must not break line-oriented output.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped s(std::string("line1\nline2"));
            au::perf::XTimer5::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("[perf5]"), std::string::npos);
    }

    // printf format specifiers — must be rendered literally.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped s(std::string("%s%d%p_test"));
            au::perf::XTimer5::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("%s%d%p_test"), std::string::npos);
    }

    // Tab and backslash characters.
    {
        au::perf::XTimer5Scoped s(std::string("tab\there\\path"));
        SUCCEED();
    }

    // UTF-8 multi-byte sequence.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped s(std::string("utf8_\xc3\xa4\xc3\xb6"));  // äö
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("utf8_"), std::string::npos);
    }

    // ANSI escape codes embedded in name.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped s(std::string("\033[31mRED\033[0m"));
            au::perf::XTimer5::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("[perf5]"), std::string::npos);
    }
}

// ===========================================================================
//  22. Wide tree — 20 sibling subs, presence and ordering
// ===========================================================================

TEST_F(XTimer5Test, WideTreeSiblingOrdering)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped root(std::string("wide"));
        for (int i = 0; i < 20; ++i) {
            root.sub(std::string("s") + std::to_string(i));
            au::perf::XTimer5::sleepFor(0);
        }
        root.sub();  // close trailing sub so tree flushes
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

// ===========================================================================
//  23. Color output — ANSI escape presence when enabled, absent when disabled
// ===========================================================================

TEST_F(XTimer5Test, ColorOutput)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    // Color enabled: ANSI escape codes present, reset count ≥ line count.
    au::log::Config::get().setColorEnabled(true);
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped root(std::string("color_test"));
            au::perf::XTimer5::sleepFor(1);
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
            au::perf::XTimer5Scoped root(std::string("nocolor"));
            au::perf::XTimer5::sleepFor(1);
        }
        const std::string out = cap.drain();
        EXPECT_EQ(out.find("\033["), std::string::npos) << out;
        EXPECT_NE(out.find("nocolor"), std::string::npos);
    }
}

// ===========================================================================
//  24. Sub state-machine exception recovery
// ===========================================================================

TEST_F(XTimer5Test, SubStateExceptionRecovery)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    // Unbalanced subs: extra bare sub() must be safe no-ops.
    {
        StdoutCapture cap;
        {
            au::perf::XTimer5Scoped root(std::string("unbalanced"));
            root.sub(std::string("A"));
            au::perf::XTimer5::sleepFor(1);
            root.sub();    // close A
            root.sub();    // extra bare sub → no-op
            root.sub();    // extra bare sub → no-op
            root.sub(std::string("B"));
            au::perf::XTimer5::sleepFor(1);
            root.sub();    // close B
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
            au::perf::XTimer5Scoped root(std::string("bare_first"));
            root.sub();  // no open sub → no-op
            root.sub();  // repeated → no-op
            root.sub(std::string("after"));
            au::perf::XTimer5::sleepFor(1);
            root.sub();  // close after
        }
        const std::string out = cap.drain();
        EXPECT_NE(out.find("after:"), std::string::npos) << out;
    }

    // Exception between sub() calls — the open sub is orphaned and tree
    // cannot flush, but the process must not crash (stack unwinding + dtor).
    {
        bool caught = false;
        try {
            au::perf::XTimer5Scoped root(std::string("mid_sub"));
            root.sub(std::string("A"));
            throw std::runtime_error("mid");
        } catch (const std::exception&) {
            caught = true;
        }
        EXPECT_TRUE(caught);
        SUCCEED();
    }
}

// ===========================================================================
//  25. Stress: 1000 named + 1000 bare subs — vector growth safety
// ===========================================================================

TEST_F(XTimer5Test, StressWideSubs)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    {
        au::perf::XTimer5Scoped root(std::string("many_subs"));
        for (int i = 0; i < 1000; ++i) {
            root.sub(std::string("s") + std::to_string(i));
        }
        for (int i = 0; i < 1000; ++i) {
            root.sub();
        }
    }
    au::log::Config::get().setLevel(au::log::Level::Verbose);
    SUCCEED();
}

// ===========================================================================
//  26. Stress: mixed creation patterns — flat / sub / nested interleaved
// ===========================================================================

TEST_F(XTimer5Test, StressMixedPatterns)
{
    auto& cfg = au::perf::PerfConfig::get();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    {
        for (int i = 0; i < 1000; ++i) {
            { au::perf::XTimer5Scoped s(std::string("flat")); }
            {
                au::perf::XTimer5Scoped s(std::string("subs"));
                s.sub(std::string("a"));
                s.sub();
            }
            {
                au::perf::XTimer5Scoped o(std::string("outer"));
                { au::perf::XTimer5Scoped in(std::string("inner")); }
            }
        }
    }
    au::log::Config::get().setLevel(au::log::Level::Verbose);
    SUCCEED();
}

#endif  // ENABLE_TEST_XTIMER5
