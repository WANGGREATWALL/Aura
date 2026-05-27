#ifndef AURA_PERF_XTIMER_H_
#define AURA_PERF_XTIMER_H_

#include <cstdint>
#include <string>

#include "perf/xperf_types.h"

namespace au {
namespace perf {

void setEnabled(bool on) noexcept;
bool isEnabled() noexcept;

void setDebugMode(bool on) noexcept;
bool isDebugMode() noexcept;

void    setTimerLevel(int32_t threshold) noexcept;
int32_t getTimerLevel() noexcept;

void    setTracerLevel(int32_t threshold) noexcept;
int32_t getTracerLevel() noexcept;

void        setRootName(const std::string& name) noexcept;
std::string getRootName() noexcept;

void setAggregateMode(bool on) noexcept;
bool isAggregateMode() noexcept;
void flushAggregated() noexcept;

void timerBegin(PerfScope* scope, const std::string& name) noexcept;
void timerSubBegin(PerfScope* scope, const std::string& name) noexcept;
void timerSubEnd(PerfScope* scope) noexcept;
void timerEnd(PerfScope* scope) noexcept;

void        sleepFor(int64_t ms) noexcept;
std::string getTimeFormatted(const std::string& fmt = "%Y-%m-%d-%H-%M-%S") noexcept;
uint64_t    timerNowNs() noexcept;
float       timerElapsedMs(uint64_t beginNs) noexcept;

class Config
{
public:
    static constexpr int32_t  LEVEL_OFF      = au::perf::LEVEL_OFF;
    static constexpr int32_t  LEVEL_ALL      = au::perf::LEVEL_ALL;
    static constexpr uint32_t HARD_MAX_DEPTH = au::perf::HARD_MAX_DEPTH;

    static Config& get() noexcept
    {
        static Config instance;
        return instance;
    }

    void setEnabled(bool on) noexcept { au::perf::setEnabled(on); }
    bool isEnabled() const noexcept { return au::perf::isEnabled(); }

    void setDebugMode(bool on) noexcept { au::perf::setDebugMode(on); }
    bool isDebugMode() const noexcept { return au::perf::isDebugMode(); }

    void    setTimerLevel(int32_t threshold) noexcept { au::perf::setTimerLevel(threshold); }
    int32_t getTimerLevel() const noexcept { return au::perf::getTimerLevel(); }

    void    setTracerLevel(int32_t threshold) noexcept { au::perf::setTracerLevel(threshold); }
    int32_t getTracerLevel() const noexcept { return au::perf::getTracerLevel(); }

    void        setRootName(const std::string& name) noexcept { au::perf::setRootName(name); }
    std::string getRootName() const noexcept { return au::perf::getRootName(); }

    void setAggregateMode(bool on) noexcept { au::perf::setAggregateMode(on); }
    bool isAggregateMode() const noexcept { return au::perf::isAggregateMode(); }

    void flushAggregated() noexcept { au::perf::flushAggregated(); }

private:
    Config()                         = default;
    Config(const Config&)            = delete;
    Config& operator=(const Config&) = delete;
};

class XTimer
{
public:
    static void sleepFor(int64_t ms) noexcept { au::perf::sleepFor(ms); }

    static std::string getTimeFormatted(const std::string& fmt = "%Y-%m-%d-%H-%M-%S") noexcept
    {
        return au::perf::getTimeFormatted(fmt);
    }

    XTimer() noexcept : mBeginNs(au::perf::timerNowNs()) {}

    void  restart() noexcept { mBeginNs = au::perf::timerNowNs(); }
    float elapsedMs() const noexcept { return au::perf::timerElapsedMs(mBeginNs); }

private:
    uint64_t mBeginNs;
};

class XTimerScoped
{
public:
    explicit XTimerScoped(const std::string& name) noexcept { au::perf::timerBegin(&mScope, name); }
    ~XTimerScoped() noexcept { au::perf::timerEnd(&mScope); }

    XTimerScoped(const XTimerScoped&)            = delete;
    XTimerScoped& operator=(const XTimerScoped&) = delete;
    XTimerScoped(XTimerScoped&&)                 = delete;
    XTimerScoped& operator=(XTimerScoped&&)      = delete;

    void sub(const std::string& name) noexcept { au::perf::timerSubBegin(&mScope, name); }
    void sub() noexcept { au::perf::timerSubEnd(&mScope); }

private:
    PerfScope mScope;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTIMER_H_
