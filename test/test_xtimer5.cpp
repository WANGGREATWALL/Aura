#if ENABLE_TEST_XTIMER5

#include <atomic>
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
//
//  v5 deliberately removed the IPerfWriter interface, so unit tests must
//  intercept output by redirecting the underlying stdout (where xlogger
//  writes via fwrite). GoogleTest exposes this through CaptureStdout /
//  GetCapturedStdout in testing::internal.
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
        // Reset the default context to a deterministic baseline.
        auto& cfg = au::perf::XPerfContext5::defaultContext();
        cfg.setEnabled(true);
        cfg.setMode(au::perf::Mode5::Release);
        cfg.setTimerLevel(3);
        cfg.setTracerLevel(au::perf::kPerfLevelAll5);
        cfg.setAggregateMode(false);
        cfg.setRootName("perf");
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
//  1. XPerfContext5 — basic atomic accessors
// ===========================================================================

TEST_F(XTimer5Test, ContextBasicAccessors)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();

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
//  2. Multiple independent contexts — config isolation
// ===========================================================================

TEST_F(XTimer5Test, MultipleContextsIndependent)
{
    au::perf::XPerfContext5 a;
    au::perf::XPerfContext5 b;

    a.setTimerLevel(1);
    b.setTimerLevel(99);

    EXPECT_EQ(a.getTimerLevel(), 1);
    EXPECT_EQ(b.getTimerLevel(), 99);

    au::perf::XPerfContext5::defaultContext().setTimerLevel(5);
    EXPECT_EQ(a.getTimerLevel(), 1);
    EXPECT_EQ(b.getTimerLevel(), 99);
    EXPECT_EQ(au::perf::XPerfContext5::defaultContext().getTimerLevel(), 5);
}

// ===========================================================================
//  3. RootName — std::string overload + truncation
// ===========================================================================

TEST_F(XTimer5Test, RootNameStringAndTruncation)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();

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
//  4. XTimer5 — elapsed / restart / sleepFor non-positive
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
//  5. Release-mode: one-liner per scope, no tree characters
// ===========================================================================

TEST_F(XTimer5Test, ReleaseModeOneLiner)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  6. Debug-mode: hierarchical tree with header + ASCII branches + colon
// ===========================================================================

TEST_F(XTimer5Test, DebugModeTreeHierarchy)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
    // v5 invariant: tree lines retain colon separator.
    EXPECT_NE(out.find(": "), std::string::npos);
    // ASCII-only invariant.
    EXPECT_EQ(out.find("\xE2"), std::string::npos);
}

// ===========================================================================
//  7. sub(name) / sub() — Debug mode aggregates into tree
// ===========================================================================

TEST_F(XTimer5Test, SubAndSubNamedDebug)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  8. sub(name) — Release mode prints the previous segment immediately
// ===========================================================================

TEST_F(XTimer5Test, SubReleaseImmediateOutput)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
    // Ordering: phase1 must precede phase2 (immediate output semantics).
    EXPECT_LT(out.find("phase1"), out.find("phase2"));
}

// ===========================================================================
//  9. Level filtering: kPerfLevelOff fully silences; threshold gates depth
// ===========================================================================

TEST_F(XTimer5Test, LevelFiltering)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
    cfg.setMode(au::perf::Mode5::Release);

    // Level == depth: setTimerLevel(0) means "show only depth-0 nodes".
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
//  10. setEnabled(false) hard-off
// ===========================================================================

TEST_F(XTimer5Test, DisabledHardOff)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  11. Multi-thread isolation: each thread one header, no interleave
// ===========================================================================

TEST_F(XTimer5Test, MultiThreadIsolation)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  12. Aggregate mode: per-context flush (instance method)
// ===========================================================================

TEST_F(XTimer5Test, AggregateModeInstanceFlush)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  13. Per-ctx aggregate isolation (xperf_8_design.md §8 acceptance #1)
// ===========================================================================

TEST_F(XTimer5Test, PerCtxAggregateIsolation)
{
    au::perf::XPerfContext5 ctxA;
    au::perf::XPerfContext5 ctxB;
    ctxA.setMode(au::perf::Mode5::Debug);
    ctxA.setTimerLevel(au::perf::kPerfLevelAll5);
    ctxA.setAggregateMode(true);
    ctxA.setRootName(std::string("ctxA-root"));

    ctxB.setMode(au::perf::Mode5::Debug);
    ctxB.setTimerLevel(au::perf::kPerfLevelAll5);
    ctxB.setAggregateMode(true);
    ctxB.setRootName(std::string("ctxB-root"));

    {
        au::perf::XTimer5Scoped a(ctxA, std::string("workA"));
        au::perf::XTimer5::sleepFor(1);
    }
    {
        au::perf::XTimer5Scoped b(ctxB, std::string("workB"));
        au::perf::XTimer5::sleepFor(1);
    }

    {
        StdoutCapture cap;
        ctxA.flushAggregated();
        const std::string out = cap.drain();
        EXPECT_NE(out.find("workA"), std::string::npos);
        EXPECT_EQ(out.find("workB"), std::string::npos) << "ctxA flush leaked B";
        EXPECT_NE(out.find("ctxA-root"), std::string::npos);
    }
    {
        StdoutCapture cap;
        ctxB.flushAggregated();
        const std::string out = cap.drain();
        EXPECT_NE(out.find("workB"), std::string::npos);
        EXPECT_EQ(out.find("workA"), std::string::npos) << "ctxB flush leaked A";
        EXPECT_NE(out.find("ctxB-root"), std::string::npos);
    }
}

// ===========================================================================
//  14. Cross-ctx nested call: each ctx renders its own independent tree
//      (xperf_8_design.md §4.4.2 scenario 4)
// ===========================================================================

TEST_F(XTimer5Test, NestedCrossCtxTreesIndependent)
{
    au::perf::XPerfContext5 ctxA;
    au::perf::XPerfContext5 ctxB;
    ctxA.setMode(au::perf::Mode5::Debug);
    ctxA.setTimerLevel(au::perf::kPerfLevelAll5);
    ctxA.setRootName(std::string("ctxA-root"));
    ctxB.setMode(au::perf::Mode5::Debug);
    ctxB.setTimerLevel(au::perf::kPerfLevelAll5);
    ctxB.setRootName(std::string("ctxB-root"));

    StdoutCapture cap;
    {
        au::perf::XTimer5Scoped outerA(ctxA, std::string("outerA"));
        {
            au::perf::XTimer5Scoped innerB(ctxB, std::string("innerB"));
            au::perf::XTimer5::sleepFor(1);
        }
        au::perf::XTimer5::sleepFor(1);
    }
    const std::string out = cap.drain();

    // Each ctx must produce its own header.
    EXPECT_NE(out.find("ctxA-root"), std::string::npos) << out;
    EXPECT_NE(out.find("ctxB-root"), std::string::npos) << out;
    EXPECT_NE(out.find("outerA"), std::string::npos);
    EXPECT_NE(out.find("innerB"), std::string::npos);
    // innerB must NOT appear as a child of outerA — they live in
    // separate per-ctx trees.
    const std::size_t headerA = out.find("ctxA-root");
    const std::size_t innerB  = out.find("innerB");
    EXPECT_NE(headerA, std::string::npos);
    EXPECT_NE(innerB, std::string::npos);
    // innerB belongs to ctxB's tree which is flushed when innerB's root
    // closes; it must therefore appear BEFORE ctxA's header (because
    // outerA's root closes after innerB has long been flushed).
    EXPECT_LT(innerB, headerA);
}

// ===========================================================================
//  15. ctx destruction auto-flushes residual aggregate data
// ===========================================================================

TEST_F(XTimer5Test, DestructorAutoFlushesResidual)
{
    StdoutCapture cap;
    {
        au::perf::XPerfContext5 transient;
        transient.setMode(au::perf::Mode5::Debug);
        transient.setTimerLevel(au::perf::kPerfLevelAll5);
        transient.setAggregateMode(true);
        transient.setRootName(std::string("transient-root"));

        {
            au::perf::XTimer5Scoped s(transient, std::string("transient.work"));
            au::perf::XTimer5::sleepFor(1);
        }
        // No explicit flushAggregated() — destructor must drain the buffer.
    }
    const std::string out = cap.drain();
    EXPECT_NE(out.find("transient.work"), std::string::npos) << out;
    EXPECT_NE(out.find("transient-root"), std::string::npos);
}

// ===========================================================================
//  16. Exception safety: outermost flush still fires when stack unwinds
// ===========================================================================

TEST_F(XTimer5Test, ExceptionSafety)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  17. Long-name truncation marker
// ===========================================================================

TEST_F(XTimer5Test, LongNameTruncationMarker)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  18. Temporary std::string name does not dangle
// ===========================================================================

TEST_F(XTimer5Test, TemporaryStringNameNoDangle)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  19. Convenience macros AU_TIMER5 / AU_PERF5_SCOPE
// ===========================================================================

TEST_F(XTimer5Test, ConvenienceMacros)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
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
//  20. Stress: 10k scopes — performance smoke + leak guard
// ===========================================================================

TEST_F(XTimer5Test, StressTenThousandScopes)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
    cfg.setMode(au::perf::Mode5::Release);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    // Capture-and-discard; we only care about throughput.
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
//  21. Deep-nest hard cap: kHardMaxDepth5 (=512) does not crash
// ===========================================================================

TEST_F(XTimer5Test, HardDepthCapNoCrash)
{
    auto& cfg = au::perf::XPerfContext5::defaultContext();
    cfg.setMode(au::perf::Mode5::Debug);
    cfg.setTimerLevel(au::perf::kPerfLevelAll5);

    StdoutCapture cap;
    // Recurse via std::function to exercise the hard cap silently.
    std::function<void(int)> recurse = [&](int n) {
        if (n <= 0)
            return;
        au::perf::XTimer5Scoped s(std::string("d"));
        recurse(n - 1);
    };
    recurse(static_cast<int>(au::perf::kHardMaxDepth5) + 16);
    (void)cap.drain();  // discard, only crash-free outcome matters
    SUCCEED();
}

#endif  // ENABLE_TEST_XTIMER5