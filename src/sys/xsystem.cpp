#include "sys/xsystem.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <thread>

#include "log/xerror.h"
#include "sys/xsystem_property_detail.h"

#if AU_OS_WINDOWS
#  include <windows.h>
#elif AU_OS_APPLE
#  include <mach/mach.h>
#  include <pthread.h>
#  include <sys/sysctl.h>
#  include <unistd.h>
#elif AU_OS_LINUX || AU_OS_ANDROID
#  include <sched.h>
#  include <sys/sysinfo.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

#if AU_OS_ANDROID
#  include <sys/system_properties.h>
#endif

namespace au {
namespace sys {
namespace {

bool validName(const std::string& name) noexcept
{
    return !name.empty() && name.find('=') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

bool bytesFromKilobytes(uint64_t kilobytes, uint64_t& bytes) noexcept
{
    if (kilobytes > std::numeric_limits<uint64_t>::max() / 1024)
        return false;
    bytes = kilobytes * 1024;
    return true;
}

#if AU_OS_ANDROID
bool readProperty(const std::string& property, char (&value)[PROP_VALUE_MAX]) noexcept
{
    if (property.empty() || property.find('\0') != std::string::npos)
        return false;
    value[0] = '\0';
    return __system_property_get(property.c_str(), value) > 0;
}
#endif

int setPropertyString(const std::string& property, const std::string& value) noexcept
{
#if AU_OS_ANDROID
    if (!validName(property) || value.find('\0') != std::string::npos)
        return err::kErrorInvalidParam;
    return __system_property_set(property.c_str(), value.c_str()) == 0
               ? err::kSuccess
               : err::kErrorPlatformAPI;
#else
    (void)property;
    (void)value;
    return err::kErrorNotSupported;
#endif
}

}  // namespace

namespace detail {

int parsePropertyInt(const char* value, int fallback) noexcept
{
    if (value == nullptr || *value == '\0')
        return fallback;
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno == ERANGE ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max())
        return fallback;
    return static_cast<int>(parsed);
}

float parsePropertyFloat(const char* value, float fallback) noexcept
{
    if (value == nullptr || *value == '\0')
        return fallback;
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    if (end == value || *end != '\0' || errno == ERANGE || !std::isfinite(parsed))
        return fallback;
    return parsed;
}

}  // namespace detail

const char* platformName(Platform platform) noexcept
{
    switch (platform) {
    case Platform::Windows: return "Windows";
    case Platform::Linux: return "Linux";
    case Platform::macOS: return "macOS";
    case Platform::iOS: return "iOS";
    case Platform::Android: return "Android";
    default: return "Unknown";
    }
}

const char* archName(Arch arch) noexcept
{
    switch (arch) {
    case Arch::x86: return "x86";
    case Arch::x86_64: return "x86_64";
    case Arch::ARM: return "ARM";
    case Arch::ARM64: return "ARM64";
    default: return "Unknown";
    }
}

SocVendor getSocVendor() noexcept
{
#if AU_OS_ANDROID
    try {
        return detail::detectSocVendor([](const char* key) {
            return getSystemPropertyValue(key, std::string());
        });
    } catch (...) {
        return SocVendor::Unknown;
    }
#elif AU_OS_APPLE
    return SocVendor::Apple;
#else
    return SocVendor::Unknown;
#endif
}

int getLogicalCpuCount() noexcept
{
#if AU_OS_WINDOWS
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<int>(info.dwNumberOfProcessors);
#elif AU_OS_APPLE
    int count = 0;
    size_t size = sizeof(count);
    return ::sysctlbyname("hw.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0
               ? count : 0;
#elif AU_OS_LINUX || AU_OS_ANDROID
    const long count = ::sysconf(_SC_NPROCESSORS_CONF);
    return count > 0 && count <= std::numeric_limits<int>::max()
               ? static_cast<int>(count) : 0;
#else
    return 0;
#endif
}

int getOnlineCpuCount() noexcept
{
#if AU_OS_WINDOWS
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return static_cast<int>(info.dwNumberOfProcessors);
#elif AU_OS_APPLE
    int count = 0;
    size_t size = sizeof(count);
    return ::sysctlbyname("hw.activecpu", &count, &size, nullptr, 0) == 0 && count > 0
               ? count : 0;
#elif AU_OS_LINUX || AU_OS_ANDROID
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 && count <= std::numeric_limits<int>::max()
               ? static_cast<int>(count) : 0;
#else
    return 0;
#endif
}

std::string getCpuModelName() noexcept
{
    try {
#if AU_OS_APPLE
        char model[256]{};
        size_t size = sizeof(model);
        if (::sysctlbyname("machdep.cpu.brand_string", model, &size, nullptr, 0) == 0)
            return model;
        size = sizeof(model);
        if (::sysctlbyname("hw.model", model, &size, nullptr, 0) == 0)
            return model;
#elif AU_OS_LINUX || AU_OS_ANDROID
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("model name") == std::string::npos &&
                line.find("Hardware") == std::string::npos)
                continue;
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const size_t begin = line.find_first_not_of(" \t", colon + 1);
            if (begin != std::string::npos)
                return line.substr(begin);
        }
#endif
    } catch (...) {
    }
    return {};
}

int getThreadAffinity(uint32_t* cpus, std::size_t capacity, std::size_t& count) noexcept
{
    count = 0;
    if (!cpus && capacity)
        return err::kErrorInvalidParam;
#if AU_OS_LINUX || AU_OS_ANDROID
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set))
        return err::kErrorPlatformAPI;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set))
            ++count;
    if (!cpus && !capacity)
        return 0;
    if (capacity < count)
        return err::kErrorOutOfRange;
    size_t index = 0;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &set))
            cpus[index++] = cpu;
    return 0;
#else
    (void)cpus;
    return err::kErrorNotSupported;
#endif
}

int setThreadAffinity(const uint32_t* cpus, std::size_t count) noexcept
{
    if (!cpus || !count)
        return err::kErrorInvalidParam;
#if AU_OS_LINUX || AU_OS_ANDROID
    cpu_set_t set;
    CPU_ZERO(&set);
    for (size_t i = 0; i < count; ++i) {
        if (cpus[i] >= CPU_SETSIZE)
            return err::kErrorInvalidParam;
        CPU_SET(cpus[i], &set);
    }
    return sched_setaffinity(0, sizeof(set), &set) ? err::kErrorPlatformAPI : 0;
#else
    return err::kErrorNotSupported;
#endif
}

int selectCpuMask(CpuClass cpuClass, uint64_t& mask) noexcept
{
    mask = 0;
    if (cpuClass != CpuClass::kPerformance && cpuClass != CpuClass::kPowerSaving)
        return err::kErrorInvalidParam;
#if AU_OS_LINUX || AU_OS_ANDROID
    try {
        // Only static capacities are cached; affinity and online CPUs are queried below.
        static const std::array<unsigned, 64> capacities = [] {
            std::array<unsigned, 64> values{};
            for (unsigned cpu = 0; cpu < values.size(); ++cpu) {
                std::ifstream input("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
                unsigned      capacity = 0;
                if (input >> capacity && capacity)
                    values[cpu] = capacity;
            }
            return values;
        }();
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed))
            return err::kErrorPlatformAPI;
        bool classified = true;
        for (unsigned cpu = 0; cpu < capacities.size(); ++cpu)
            if (CPU_ISSET(cpu, &allowed) && !capacities[cpu])
                classified = false;
        unsigned best = cpuClass == CpuClass::kPerformance ? 0 : UINT32_MAX;
        for (unsigned cpu = 0; cpu < capacities.size(); ++cpu) {
            if (!CPU_ISSET(cpu, &allowed))
                continue;
            const unsigned capacity = classified ? capacities[cpu] : 1024;
            if ((cpuClass == CpuClass::kPerformance && capacity > best) ||
                (cpuClass == CpuClass::kPowerSaving && capacity < best)) {
                best = capacity;
                mask = 0;
            }
            if (capacity == best)
                mask |= UINT64_C(1) << cpu;
        }
        return mask ? 0 : err::kErrorPlatformAPI;
    } catch (const std::bad_alloc&) {
        return err::kErrorNoMemory;
    } catch (...) {
        return err::kErrorPlatformAPI;
    }
#else
    return err::kErrorNotSupported;
#endif
}

uint64_t getMemoryTotalBytes() noexcept
{
#if AU_OS_WINDOWS
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif AU_OS_APPLE
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    return ::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#elif AU_OS_LINUX || AU_OS_ANDROID
    struct sysinfo info{};
    if (::sysinfo(&info) != 0 ||
        static_cast<uint64_t>(info.totalram) >
            std::numeric_limits<uint64_t>::max() / info.mem_unit)
        return 0;
    return static_cast<uint64_t>(info.totalram) * info.mem_unit;
#else
    return 0;
#endif
}

uint64_t getMemoryAvailableBytes() noexcept
{
#if AU_OS_WINDOWS
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#elif AU_OS_APPLE
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS)
        return 0;
    const uint64_t pages = static_cast<uint64_t>(stats.free_count) + stats.inactive_count;
    return pages <= std::numeric_limits<uint64_t>::max() / vm_page_size
               ? pages * vm_page_size : 0;
#elif AU_OS_LINUX || AU_OS_ANDROID
    try {
        std::ifstream meminfo("/proc/meminfo");
        std::string name;
        uint64_t kilobytes = 0;
        std::string unit;
        while (meminfo >> name >> kilobytes >> unit) {
            if (name == "MemAvailable:") {
                uint64_t bytes = 0;
                return unit == "kB" && bytesFromKilobytes(kilobytes, bytes) ? bytes : 0;
            }
        }
    } catch (...) {
    }
    return 0;
#else
    return 0;
#endif
}

std::string getHostName() noexcept
{
    char name[256]{};
#if AU_OS_WINDOWS
    DWORD size = sizeof(name);
    if (!::GetComputerNameA(name, &size))
        return {};
#elif AU_OS_APPLE || AU_OS_LINUX || AU_OS_ANDROID
    if (::gethostname(name, sizeof(name)) != 0)
        return {};
    name[sizeof(name) - 1] = '\0';
#else
    return {};
#endif
    try {
        return name;
    } catch (...) {
        return {};
    }
}

int getHardwareConcurrency() noexcept
{
    const unsigned count = std::thread::hardware_concurrency();
    return count <= static_cast<unsigned>(std::numeric_limits<int>::max())
               ? static_cast<int>(count) : 0;
}

uint64_t getCurrentProcessId() noexcept
{
#if AU_OS_WINDOWS
    return static_cast<uint64_t>(::GetCurrentProcessId());
#elif AU_OS_APPLE || AU_OS_LINUX || AU_OS_ANDROID
    return static_cast<uint64_t>(::getpid());
#else
    return 0;
#endif
}

uint64_t getCurrentThreadId() noexcept
{
#if AU_OS_WINDOWS
    return static_cast<uint64_t>(::GetCurrentThreadId());
#elif AU_OS_APPLE
    return static_cast<uint64_t>(::pthread_mach_thread_np(::pthread_self()));
#elif AU_OS_LINUX || AU_OS_ANDROID
    const long id = ::syscall(SYS_gettid);
    return id > 0 ? static_cast<uint64_t>(id) : 0;
#else
    return 0;
#endif
}

std::string getEnvVar(const std::string& name, const std::string& defaultVal) noexcept
{
    try {
        const char* value = name.empty() ? nullptr : std::getenv(name.c_str());
        return value ? std::string(value) : defaultVal;
    } catch (...) {
        return {};
    }
}

bool hasEnvVar(const std::string& name) noexcept
{
    return !name.empty() && std::getenv(name.c_str()) != nullptr;
}

int setEnvVar(const std::string& name, const std::string& value) noexcept
{
    if (!validName(name) || value.find('\0') != std::string::npos)
        return err::kErrorInvalidParam;
#if AU_OS_WINDOWS
    return ::_putenv_s(name.c_str(), value.c_str()) == 0
               ? err::kSuccess : err::kErrorPlatformAPI;
#else
    return ::setenv(name.c_str(), value.c_str(), 1) == 0
               ? err::kSuccess : err::kErrorPlatformAPI;
#endif
}

int getSystemPropertyValue(const std::string& property, int defaultVal) noexcept
{
#if AU_OS_ANDROID
    char value[PROP_VALUE_MAX]{};
    return readProperty(property, value)
               ? detail::parsePropertyInt(value, defaultVal) : defaultVal;
#else
    (void)property;
    return defaultVal;
#endif
}

float getSystemPropertyValue(const std::string& property, float defaultVal) noexcept
{
#if AU_OS_ANDROID
    char value[PROP_VALUE_MAX]{};
    return readProperty(property, value)
               ? detail::parsePropertyFloat(value, defaultVal) : defaultVal;
#else
    (void)property;
    return defaultVal;
#endif
}

std::string getSystemPropertyValue(const std::string& property,
                                   std::string defaultVal) noexcept
{
    try {
#if AU_OS_ANDROID
        char value[PROP_VALUE_MAX]{};
        return readProperty(property, value) ? std::string(value) : defaultVal;
#else
        (void)property;
        return defaultVal;
#endif
    } catch (...) {
        return {};
    }
}

int setSystemPropertyValue(const std::string& property, int value) noexcept
{
#if AU_OS_ANDROID
    try {
        return setPropertyString(property, std::to_string(value));
    } catch (...) {
        return err::kErrorNoMemory;
    }
#else
    (void)property;
    (void)value;
    return err::kErrorNotSupported;
#endif
}

int setSystemPropertyValue(const std::string& property, float value) noexcept
{
#if AU_OS_ANDROID
    try {
        return setPropertyString(property, std::to_string(value));
    } catch (...) {
        return err::kErrorNoMemory;
    }
#else
    (void)property;
    (void)value;
    return err::kErrorNotSupported;
#endif
}

int setSystemPropertyValue(const std::string& property, std::string value) noexcept
{
    return setPropertyString(property, value);
}

}  // namespace sys
}  // namespace au
