#ifndef AURA_XLOGGER_H_
#define AURA_XLOGGER_H_

/**
 * @file xlogger.h
 * @brief Lightweight, cross-platform logging framework.
 *
 * Features:
 *  - Macro-side level check (zero overhead for filtered-out messages)
 *  - ANSI color output (optional, applies to stdout/shell on all platforms)
 *  - Android logcat + optional shell/stdout output
 *  - Thread-safe output serialisation
 *  - File/line info only for Warn/Error/Fatal (reduces V/D/I overhead)
 *
 * Newline convention:
 *  Always terminate format strings with '\n' (e.g. XLOG_I("msg\n")).
 *  - Desktop/shell: '\n' is written as-is by fwrite for proper line separation.
 *  - Android logcat: logd (API 21+) strips any trailing '\n' from every record
 *    before storage, so no double blank lines appear regardless of level.
 *
 * Android shell output:
 *  By default, logs go to logcat only. To also print to stdout (e.g. adb shell),
 *  call setShellPrintEnabled(true) at startup. xlogger intentionally does NOT read
 *  any system property internally — the property name and read logic are business-
 *  specific and must be handled by the caller. Recommended pattern:
 *
 *    // In your module's init (e.g. MyModule.cpp):
 *    #include "sys/xsystem.h"
 *    const bool shellOn =
 *        sys::getSystemPropertyValue("vendor.algo_module.enable_log_shell", 0) != 0;
 *    log::Config::get().setShellPrintEnabled(shellOn);
 *
 * Quick start:
 *   log::Config::get().setTag("MyApp");
 *   log::Config::get().setLevel(log::Level::Debug);
 *   XLOG_I("initialised, version=%d\n", 1);
 *   XLOG_E("open failed: %s\n", path);
 */

#include <cstdlib>

#include "sys/xsystem.h"

namespace au {
namespace log {

enum class Level : int
{
    Verbose = 0,
    Debug   = 1,
    Info    = 2,  ///< default threshold
    Warn    = 3,
    Error   = 4,
    Fatal   = 5,
    Silent  = 6,
};

/**
 * @brief Singleton holding logger configuration.
 *
 * Implementation details (atomics, mutex, tag buffer) are hidden behind a
 * Pimpl pointer so that this header remains free of <atomic> and other
 * heavy standard-library includes.
 */
class AU_API Config
{
public:
    static Config& get() noexcept;

    void        setTag(const char* tag) noexcept;
    const char* getTag() const noexcept;

    /// Returns true exactly once if setTag() was never called.
    bool tryConsumeTagWarning() noexcept;

    void  setLevel(Level level) noexcept;
    Level getLevel() const noexcept;

    void setColorEnabled(bool on) noexcept;
    bool isColorEnabled() const noexcept;

    void setShellPrintEnabled(bool on) noexcept;
    bool isShellPrintEnabled() const noexcept;

private:
    Config() noexcept;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // Opaque implementation — defined in xlogger.cpp.
    // Points to a static Impl instance; lifetime equals the process lifetime.
    struct Impl;
    Impl* mImpl;
};

namespace detail {

constexpr const char* basename(const char* path) noexcept
{
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void logPrintF(Level level, const char* fmt, ...) noexcept;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
void logPrintFLoc(Level level, const char* file, int line, const char* fmt, ...) noexcept;

}  // namespace detail
}  // namespace log
}  // namespace au

// ── Public logging macros ──

// Internal: level check + dispatch without location (V/D/I).
#define XLOG_IMPL(lvl, fmt, ...)                                     \
    do {                                                             \
        if ((lvl) >= ::au::log::Config::get().getLevel()) {          \
            ::au::log::detail::logPrintF((lvl), fmt, ##__VA_ARGS__); \
        }                                                            \
    } while (0)

// Internal: level check + dispatch with (file:line) (W/E/F).
#define XLOG_IMPL_LOC(lvl, fmt, ...)                                                                     \
    do {                                                                                                 \
        if ((lvl) >= ::au::log::Config::get().getLevel()) {                                              \
            ::au::log::detail::logPrintFLoc((lvl), ::au::log::detail::basename(__FILE__), __LINE__, fmt, \
                                            ##__VA_ARGS__);                                              \
        }                                                                                                \
    } while (0)

// clang-format off
#define XLOG_V(fmt, ...) XLOG_IMPL(::au::log::Level::Verbose, fmt, ##__VA_ARGS__)
#define XLOG_D(fmt, ...) XLOG_IMPL(::au::log::Level::Debug,   fmt, ##__VA_ARGS__)
#define XLOG_I(fmt, ...) XLOG_IMPL(::au::log::Level::Info,    fmt, ##__VA_ARGS__)
#define XLOG_W(fmt, ...) XLOG_IMPL_LOC(::au::log::Level::Warn,  fmt, ##__VA_ARGS__)
#define XLOG_E(fmt, ...) XLOG_IMPL_LOC(::au::log::Level::Error, fmt, ##__VA_ARGS__)
#define XLOG_F(fmt, ...) XLOG_IMPL_LOC(::au::log::Level::Fatal, fmt, ##__VA_ARGS__)
// clang-format on

// ── Assertion / check macros ──

#define XCHECK(expr)                                                                                                  \
    do {                                                                                                              \
        if (!(expr)) {                                                                                                \
            ::au::log::detail::logPrintFLoc(::au::log::Level::Fatal, ::au::log::detail::basename(__FILE__), __LINE__, \
                                            "check failed: '%s'\n", #expr);                                           \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (0)

#define XCHECK_WITH_RET(expr, ret)                                                                                    \
    do {                                                                                                              \
        if (!(expr)) {                                                                                                \
            ::au::log::detail::logPrintFLoc(::au::log::Level::Error, ::au::log::detail::basename(__FILE__), __LINE__, \
                                            "check failed: '%s'\n", #expr);                                           \
            return (ret);                                                                                             \
        }                                                                                                             \
    } while (0)

#define XCHECK_WITH_MSG(expr, ret, fmt, ...)                                                                          \
    do {                                                                                                              \
        if (!(expr)) {                                                                                                \
            ::au::log::detail::logPrintFLoc(::au::log::Level::Error, ::au::log::detail::basename(__FILE__), __LINE__, \
                                            fmt, ##__VA_ARGS__);                                                      \
            return (ret);                                                                                             \
        }                                                                                                             \
    } while (0)

#endif  // AURA_XLOGGER_H_
