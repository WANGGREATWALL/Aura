#if ENABLE_TEST_XLOGGER

#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "log/xlogger.h"


// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class XLoggerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        au::log::Config::get().setTag("XLoggerTest");
        au::log::Config::get().setLevel(au::log::Level::Verbose);
        au::log::Config::get().setColorEnabled(true);
#if AU_OS_ANDROID
        // Default target is logcat; enable shell so CaptureStdout can intercept.
        au::log::Config::get().setShellPrintEnabled(true);
#endif
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Capture everything written to stdout during fn().
static std::string captureStdout(const std::function<void()>& fn)
{
    testing::internal::CaptureStdout();
    fn();
    std::fflush(stdout);
    return testing::internal::GetCapturedStdout();
}

/// Count non-overlapping occurrences of sub in s.
static int countOccurrences(const std::string& s, const std::string& sub)
{
    int  cnt = 0;
    auto pos = s.find(sub);
    while (pos != std::string::npos) {
        ++cnt;
        pos = s.find(sub, pos + sub.size());
    }
    return cnt;
}

// XCHECK_WITH_* require a function return context.
static int fnCheckRet(bool cond)
{
    XCHECK_WITH_RET(cond, -1);
    return 0;
}
static int fnCheckMsg(int w)
{
    XCHECK_WITH_MSG(w > 0, -2, "bad width %d\n", w);
    return 0;
}

// ===========================================================================
// 1. Configuration
// ===========================================================================

TEST_F(XLoggerTest, Config)
{
    au::log::Config::get().setTag("XLoggerConfig");
    EXPECT_STREQ(au::log::Config::get().getTag(), "XLoggerConfig");

    // null tag falls back to "unknown"
    au::log::Config::get().setTag(nullptr);
    EXPECT_STREQ(au::log::Config::get().getTag(), "unknown");

    au::log::Config::get().setLevel(au::log::Level::Fatal);
    EXPECT_EQ(au::log::Config::get().getLevel(), au::log::Level::Fatal);

    au::log::Config::get().setColorEnabled(true);
    EXPECT_TRUE(au::log::Config::get().isColorEnabled());
    au::log::Config::get().setColorEnabled(false);
    EXPECT_FALSE(au::log::Config::get().isColorEnabled());
}

// ===========================================================================
// 2. Level filtering
// ===========================================================================

TEST_F(XLoggerTest, LevelFilter)
{
    au::log::Config::get().setLevel(au::log::Level::Warn);

    const std::string suppressed = captureStdout([] {
        XLOG_V("v\n");
        XLOG_D("d\n");
        XLOG_I("i\n");
    });
    EXPECT_TRUE(suppressed.empty()) << "V/D/I must be suppressed below Warn";

    const std::string passed = captureStdout([] {
        XLOG_W("warn\n");
        XLOG_E("error\n");
        XLOG_F("fatal\n");
    });
    EXPECT_NE(passed.find("warn"), std::string::npos);
    EXPECT_NE(passed.find("error"), std::string::npos);
    EXPECT_NE(passed.find("fatal"), std::string::npos);

    au::log::Config::get().setLevel(au::log::Level::Silent);
    const std::string silent = captureStdout([] {
        XLOG_W("w\n");
        XLOG_E("e\n");
        XLOG_F("f\n");
    });
    EXPECT_TRUE(silent.empty()) << "Silent must suppress all levels";
}

// ===========================================================================
// 3. Output format
// ===========================================================================

TEST_F(XLoggerTest, Format)
{
    au::log::Config::get().setTag("Fmt");

    au::log::Config::get().setColorEnabled(false);

    // V/D/I: "[tag][L] message", no file:line
    const std::string outI = captureStdout([] { XLOG_I("hello %d\n", 42); });
    EXPECT_NE(outI.find("[Fmt][I]"), std::string::npos);
    EXPECT_NE(outI.find("hello 42"), std::string::npos);
    EXPECT_EQ(outI.find("test_xlogger"), std::string::npos) << "I must not carry location";

    // W/E/F: "[tag][L] (file:line) message"
    const std::string outW = captureStdout([] { XLOG_W("warn msg\n"); });
    EXPECT_NE(outW.find("[Fmt][W]"), std::string::npos);
    EXPECT_NE(outW.find("warn msg"), std::string::npos);
    const auto pos = outW.find('(');
    ASSERT_NE(pos, std::string::npos) << "W must carry location '('";
    EXPECT_NE(outW.find(':', pos), std::string::npos) << "location must contain ':'";

    // Tag change must be reflected immediately.
    au::log::Config::get().setTag("New");
    const std::string outNew = captureStdout([] { XLOG_I("x\n"); });
    EXPECT_NE(outNew.find("[New]"), std::string::npos);
    EXPECT_EQ(outNew.find("[Fmt]"), std::string::npos);
}

// ===========================================================================
// 4. Color output
// ===========================================================================
// Verified on all platforms including Android (shellEnabled=true in SetUp).
// logcat output is not colorized — ANSI escapes are for the shell/stdout path only.

TEST_F(XLoggerTest, ColorOutput)
{
    au::log::Config::get().setTag("Color");
    au::log::Config::get().setColorEnabled(true);

    // V/D/I have no badge color; use W (yellow) to verify ANSI is present.
    const std::string outColor = captureStdout([] { XLOG_W("colored\n"); });
    EXPECT_NE(outColor.find("\033["), std::string::npos) << "Expected ANSI escape in [W] badge";
    EXPECT_NE(outColor.find("\033[0m"), std::string::npos) << "Expected ANSI reset after [W] badge";
    EXPECT_NE(outColor.find("colored"), std::string::npos);

    au::log::Config::get().setColorEnabled(false);
    const std::string outPlain = captureStdout([] { XLOG_W("plain\n"); });
    EXPECT_EQ(outPlain.find("\033["), std::string::npos) << "No ANSI escape when color=false";
    EXPECT_NE(outPlain.find("plain"), std::string::npos);
}

// ===========================================================================
// 5. XCHECK macros
// ===========================================================================

TEST_F(XLoggerTest, CheckMacros)
{
    // Passing condition: no output, correct return value.
    EXPECT_EQ(fnCheckRet(true), 0);
    EXPECT_EQ(fnCheckMsg(5), 0);

    // XCHECK_WITH_RET fail: logs "check failed", returns -1, carries location.
    int               retRet = 0;
    const std::string outRet = captureStdout([&] { retRet = fnCheckRet(false); });
    EXPECT_EQ(retRet, -1);
    EXPECT_NE(outRet.find("check failed"), std::string::npos);
    EXPECT_NE(outRet.find('('), std::string::npos) << "must carry location";

    // XCHECK_WITH_MSG fail: logs custom message with arg, returns -2.
    int               retMsg = 0;
    const std::string outMsg = captureStdout([&] { retMsg = fnCheckMsg(-7); });
    EXPECT_EQ(retMsg, -2);
    EXPECT_NE(outMsg.find("bad width"), std::string::npos);
    EXPECT_NE(outMsg.find("-7"), std::string::npos);
    EXPECT_NE(outMsg.find('('), std::string::npos) << "must carry location";
}

// ===========================================================================
// 6. Newline correctness on stdout
// ===========================================================================
//
// Paths verified here (via CaptureStdout):
//   Desktop                        : fwrite writes exactly what was formatted.
//   Android shellEnabled=true      : '\n' stripped before __android_log_write,
//                                    restored for fwrite — one '\n' per line.
//   Android shellEnabled=false     : stdout empty (all output goes to logcat).
//
// logcat newline behaviour (see AndroidLogcatNewlineProbe):
//   Modern logd (API 21+) strips trailing '\n' from every log record before
//   storage, so no double blank lines appear regardless of level or path.
//   The '\n'-strip in the W/E/F fast path is therefore redundant but harmless.

TEST_F(XLoggerTest, NewlineOnStdout)
{
    au::log::Config::get().setTag("XLoggerTest-NewlineOnStdout");

    // Each message has one '\n'; stdout must show exactly that — no doubling.
    const std::string outVDI = captureStdout([] {
        XLOG_V("verbose\n");
        XLOG_D("debug\n");
        XLOG_I("info\n");
    });
    EXPECT_EQ(countOccurrences(outVDI, "\n"), 3) << "V/D/I: expected 3 newlines";
    EXPECT_EQ(countOccurrences(outVDI, "\n\n"), 0) << "V/D/I: double newline on stdout";

    const std::string outWEF = captureStdout([] {
        XLOG_W("warn\n");
        XLOG_E("error\n");
        XLOG_F("fatal\n");
    });
    EXPECT_EQ(countOccurrences(outWEF, "\n"), 3) << "W/E/F: expected 3 newlines";
    EXPECT_EQ(countOccurrences(outWEF, "\n\n"), 0) << "W/E/F: double newline on stdout";

    // Message without '\n' must not gain one.
    const std::string outNone = captureStdout([] { XLOG_I("no-newline"); });
    EXPECT_EQ(countOccurrences(outNone, "\n"), 0) << "trailing newline must not be added";

#if AU_OS_ANDROID
    // shellEnabled=false: output goes to logcat only — stdout must be empty.
    au::log::Config::get().setShellPrintEnabled(false);
    const std::string outLogcatOnly = captureStdout([] {
        XLOG_V("v\n");
        XLOG_D("d\n");
        XLOG_I("i\n");
        XLOG_W("w\n");
        XLOG_E("e\n");
        XLOG_F("f\n");
    });
    EXPECT_TRUE(outLogcatOnly.empty()) << "shellEnabled=false: stdout must be empty";
    au::log::Config::get().setShellPrintEnabled(true);
#endif
}

// ===========================================================================
// 7. Android logcat newline probe  (manual verification)
// ===========================================================================
//
// Emits messages with trailing '\n' to logcat (shellEnabled=false).
// Modern logd (API 21+) strips trailing '\n' automatically — no double blank
// lines are expected for any level.
//
// Verify with:  adb logcat -s XLOG_NL_CHK
//   All entries should appear as single lines with no spurious blank lines.

#if AU_OS_ANDROID
TEST_F(XLoggerTest, AndroidLogcatNewlineProbe)
{
    au::log::Config::get().setTag("XLOG_NL_CHK");
    au::log::Config::get().setShellPrintEnabled(false);

    XLOG_V("V with trailing newline — logd strips it, no blank line\n");
    XLOG_D("D with trailing newline — logd strips it, no blank line\n");
    XLOG_I("I with trailing newline — logd strips it, no blank line\n");
    XLOG_W("W with trailing newline — stripped by both code and logd\n");
    XLOG_E("E with trailing newline — stripped by both code and logd\n");
    XLOG_F("F with trailing newline — stripped by both code and logd\n");

    au::log::Config::get().setShellPrintEnabled(true);
    SUCCEED() << "Inspect logcat manually: adb logcat -s XLOG_NL_CHK";
}
#endif

// ===========================================================================
// 8. Thread safety
// ===========================================================================

TEST_F(XLoggerTest, ThreadSafety)
{
    constexpr int kThreads = 8;
    constexpr int kIter    = 200;

    // Disable color so every line starts with '[' — ANSI escapes would
    // prepend '\033[' and make the prefix check meaningless.
    au::log::Config::get().setColorEnabled(false);

    testing::internal::CaptureStdout();
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([i] {
                for (int j = 0; j < kIter; ++j) {
                    XLOG_I("t=%d i=%d\n", i, j);
                    XLOG_W("t=%d w=%d\n", i, j);
                }
            });
        }
        for (auto& t : threads)
            t.join();
    }
    std::fflush(stdout);
    const std::string out = testing::internal::GetCapturedStdout();

    // Garbled interleaving produces lines not starting with '['.
    std::istringstream ss(out);
    std::string        line;
    int                badLines = 0;
    while (std::getline(ss, line)) {
        if (!line.empty() && line[0] != '[')
            ++badLines;
    }
    EXPECT_EQ(badLines, 0) << badLines << " line(s) did not start with '['";
}

// ============================================================================
// tryConsumeTagWarning
// ============================================================================

TEST(XLogger, TryConsumeTagWarning)
{
    // First call on a fresh Config should return true (warn about missing tag)
    // Since the test fixture sets tag in SetUp, we test the atomic flag directly.
    // The flag is internal, but we can verify the function is callable.
    bool first  = au::log::Config::get().tryConsumeTagWarning();
    bool second = au::log::Config::get().tryConsumeTagWarning();
    // After at least one call, subsequent calls return false
    // (Note: fixture may have already consumed the warning)
    EXPECT_FALSE(second) << "Second call should return false (already warned)";
    (void)first;
}

#endif  // ENABLE_TEST_XLOGGER