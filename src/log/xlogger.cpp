#include "log/xlogger.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#if AU_OS_ANDROID
#include <android/log.h>

// AURA_USING_AU
using namespace au;
#endif

// ============================================================================
// Config::Impl - hidden from the public header
// ============================================================================

struct au::log::Config::Impl
{
    std::atomic<Level> level{Level::Info};
    std::atomic<bool>  colorEnabled{true};
    std::atomic<bool>  shellPrint{false};
};



// ============================================================================
// Internal helpers (anonymous namespace)
// ============================================================================

namespace {

std::mutex        gOutputMutex;
char              gTag[64] = "unknown";
std::atomic<bool> gTagWarned{false};

inline int clampLen(int len, int capacity) noexcept
{
    return len < 0 ? 0 : (len >= capacity ? capacity - 1 : len);
}

inline const char* levelTag(::au::log::Level level) noexcept
{
    // clang-format off
    switch (level) {
        case ::au::log::Level::Verbose: return "V";
        case ::au::log::Level::Debug:   return "D";
        case ::au::log::Level::Info:    return "I";
        case ::au::log::Level::Warn:    return "W";
        case ::au::log::Level::Error:   return "E";
        case ::au::log::Level::Fatal:   return "F";
        default:                    return "?";
    }
    // clang-format on
}

inline const char* levelColor(::au::log::Level level) noexcept
{
    // All badges use reverse video (SGR 7) so the level letter sits on a
    // coloured block — much more visible than a single coloured glyph.
    // clang-format off
    switch (level) {
        case ::au::log::Level::Verbose: return "\033[2;7m";      // dim + reverse
        case ::au::log::Level::Debug:   return "\033[7;36m";     // reverse cyan
        case ::au::log::Level::Info:    return "\033[7;32m";     // reverse green
        case ::au::log::Level::Warn:    return "\033[7;33m";     // reverse yellow
        case ::au::log::Level::Error:   return "\033[7;31m";     // reverse red
        case ::au::log::Level::Fatal:   return "\033[1;5;41;97m"; // bold + blink + red bg
        default:                    return nullptr;
    }
    // clang-format on
}

#if AU_OS_ANDROID
inline int toAndroidPriority(::au::log::Level level) noexcept
{
    // clang-format off
    switch (level) {
        case ::au::log::Level::Verbose: return ANDROID_LOG_VERBOSE;
        case ::au::log::Level::Debug:   return ANDROID_LOG_DEBUG;
        case ::au::log::Level::Info:    return ANDROID_LOG_INFO;
        case ::au::log::Level::Warn:    return ANDROID_LOG_WARN;
        case ::au::log::Level::Error:   return ANDROID_LOG_ERROR;
        case ::au::log::Level::Fatal:   return ANDROID_LOG_FATAL;
        default:                    return ANDROID_LOG_DEFAULT;
    }
    // clang-format on
}
#endif

struct FmtResult
{
    char buf[896];
    int  hdrLen;
    int  outLen;
};

inline void formatOutBuf(
    FmtResult&        r,
    const char*       tag,
    ::au::log::Level  level,
    bool              color,
    const char*       file,
    int               line,
    const char*       fmt,
    va_list           args) noexcept
{
    const char* lc = color ? levelColor(level) : nullptr;
    if (lc) {
        r.hdrLen = clampLen(
            std::snprintf(r.buf, sizeof(r.buf), "[%s]%s[%s]\033[0m ", tag, lc, levelTag(level)),
            static_cast<int>(sizeof(r.buf)));
    } else {
        r.hdrLen = clampLen(
            std::snprintf(r.buf, sizeof(r.buf), "[%s][%s] ", tag, levelTag(level)),
            static_cast<int>(sizeof(r.buf)));
    }

    int prefixLen = r.hdrLen;
    if (file != nullptr) {
        prefixLen += clampLen(
            std::snprintf(r.buf + prefixLen, sizeof(r.buf) - prefixLen, "(%s:%d) ", file, line),
            static_cast<int>(sizeof(r.buf)) - prefixLen);
    }

    const int bodyLen = clampLen(
        std::vsnprintf(r.buf + prefixLen, sizeof(r.buf) - prefixLen, fmt, args),
        static_cast<int>(sizeof(r.buf)) - prefixLen);

    r.outLen = prefixLen + bodyLen;
}

inline void logPrint(
    ::au::log::Level level,
    const char*      file,
    int              line,
    const char*      fmt,
    va_list          args) noexcept
{
    ::au::log::Config& cfg = ::au::log::Config::get();

    if (cfg.tryConsumeTagWarning()) {
        std::fprintf(stderr,
                     "[unknown][W] log tag not set — "
                     "call au::log::Config::get().setTag(\"YourTag\") at startup\n");
    }

    const bool needFlush = (level >= ::au::log::Level::Warn);

#if AU_OS_ANDROID
    const int  prio         = toAndroidPriority(level);
    const bool shellEnabled = cfg.isShellPrintEnabled();

    if (!shellEnabled) {
        if (file == nullptr) {
            __android_log_vprint(prio, cfg.getTag(), fmt, args);
        } else {
            char bodyBuf[820];
            std::vsnprintf(bodyBuf, sizeof(bodyBuf), fmt, args);
            __android_log_print(prio, cfg.getTag(), "(%s:%d) %s", file, line, bodyBuf);
        }
        return;
    }
#endif

    FmtResult r;
    formatOutBuf(r, cfg.getTag(), level, cfg.isColorEnabled(), file, line, fmt, args);

#if AU_OS_ANDROID
    __android_log_write(prio, cfg.getTag(), r.buf + r.hdrLen);
#endif

    std::lock_guard<std::mutex> lk(gOutputMutex);
    std::fwrite(r.buf, 1, static_cast<size_t>(r.outLen), stdout);
    if (needFlush)
        std::fflush(stdout);
}

}  // namespace


// ============================================================================
// Config - Pimpl method implementations
// ============================================================================

au::log::Config::Config() noexcept
{
    static Impl sImpl;
    mImpl = &sImpl;
}

au::log::Config& au::log::Config::get() noexcept
{
    static Config sInstance;
    return sInstance;
}

void au::log::Config::setTag(const char* tag) noexcept
{
    std::lock_guard<std::mutex> lock(gOutputMutex);
    std::strncpy(gTag, tag ? tag : "unknown", sizeof(gTag) - 1);
    gTag[sizeof(gTag) - 1] = '\0';
    gTagWarned.store(true, std::memory_order_relaxed);
}

const char* au::log::Config::getTag() const noexcept
{
    return gTag;
}

bool au::log::Config::tryConsumeTagWarning() noexcept
{
    if (gTagWarned.load(std::memory_order_relaxed))
        return false;
    bool expected = false;
    return gTagWarned.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

void au::log::Config::setLevel(Level level) noexcept
{
    mImpl->level.store(level, std::memory_order_relaxed);
}

au::log::Level au::log::Config::getLevel() const noexcept
{
    return mImpl->level.load(std::memory_order_relaxed);
}

void au::log::Config::setColorEnabled(bool on) noexcept
{
    mImpl->colorEnabled.store(on, std::memory_order_relaxed);
}

bool au::log::Config::isColorEnabled() const noexcept
{
    return mImpl->colorEnabled.load(std::memory_order_relaxed);
}

void au::log::Config::setShellPrintEnabled(bool on) noexcept
{
    mImpl->shellPrint.store(on, std::memory_order_relaxed);
}

bool au::log::Config::isShellPrintEnabled() const noexcept
{
    return mImpl->shellPrint.load(std::memory_order_relaxed);
}


// ============================================================================
// detail::logPrintF / logPrintFLoc
// ============================================================================

void au::log::detail::logPrintF(au::log::Level level, const char* fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);
    logPrint(level, nullptr, 0, fmt, args);
    va_end(args);
}

void au::log::detail::logPrintFLoc(
    au::log::Level level,
    const char*    file,
    int            line,
    const char*    fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);
    logPrint(level, file, line, fmt, args);
    va_end(args);
}

// AURA_NS_WRAPPED
