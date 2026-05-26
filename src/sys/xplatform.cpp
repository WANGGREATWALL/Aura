#include "sys/xplatform.h"
#include "log/xlogger.h"

#include <cstdlib>
#include <thread>
#include <fstream>
#include <sstream>

#ifdef AU_OS_WINDOWS
#include <Windows.h>
#elif defined(AU_OS_APPLE)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <unistd.h>
#include <pthread.h>
#elif defined(AU_OS_LINUX)
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#endif

#ifdef AU_OS_ANDROID
#include <sys/system_properties.h>
#endif

namespace au { namespace sys {

// ============================================================================
// Platform / Arch name strings
// ============================================================================

Platform runtimePlatform() { return kBuildPlatform; }

const char* platformName(Platform p) {
    switch (p) {
        case Platform::Windows: return "Windows";
        case Platform::Linux:   return "Linux";
        case Platform::macOS:   return "macOS";
        case Platform::iOS:     return "iOS";
        case Platform::Android: return "Android";
        default:                return "Unknown";
    }
}

const char* archName(Arch a) {
    switch (a) {
        case Arch::x86:    return "x86";
        case Arch::x86_64: return "x86_64";
        case Arch::ARM:    return "ARM";
        case Arch::ARM64:  return "ARM64";
        default:           return "Unknown";
    }
}

// ============================================================================
// SoC Vendor (Android-specific)
// ============================================================================

SocVendor getSocVendor() {
#ifdef AU_OS_ANDROID
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.hardware", buf);
    std::string hw(buf);

    if (hw.find("qcom") != std::string::npos || hw.find("sdm") != std::string::npos ||
        hw.find("msm") != std::string::npos  || hw.find("kona") != std::string::npos) {
        return SocVendor::Qualcomm;
    }
    if (hw.find("mt") != std::string::npos) {
        return SocVendor::MediaTek;
    }
    if (hw.find("exynos") != std::string::npos) {
        return SocVendor::Samsung;
    }
    if (hw.find("kirin") != std::string::npos || hw.find("hi") != std::string::npos) {
        return SocVendor::HiSilicon;
    }
#elif defined(AU_OS_APPLE)
    return SocVendor::Apple;
#endif
    return SocVendor::Unknown;
}

// ============================================================================
// CPU Info
// ============================================================================

CpuInfo getCpuInfo() {
    CpuInfo info;
    info.arch = kBuildArch;

#ifdef AU_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info.coreCount = static_cast<int>(si.dwNumberOfProcessors);
    info.onlineCoreCount = info.coreCount;
    info.modelName = "Unknown (Windows)";

#elif defined(AU_OS_APPLE)
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    sysctlbyname("hw.physicalcpu", &ncpu, &len, nullptr, 0);
    info.coreCount = ncpu;

    sysctlbyname("hw.logicalcpu", &ncpu, &len, nullptr, 0);
    info.onlineCoreCount = ncpu;

    char model[256] = {0};
    len = sizeof(model);
    sysctlbyname("machdep.cpu.brand_string", model, &len, nullptr, 0);
    info.modelName = model;

#elif defined(AU_OS_LINUX)
    info.coreCount = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    info.onlineCoreCount = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos ||
            line.find("Hardware") != std::string::npos) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                info.modelName = line.substr(pos + 2);
            }
            break;
        }
    }
#endif

    return info;
}

// ============================================================================
// Memory Info
// ============================================================================

MemoryInfo getMemoryInfo() {
    MemoryInfo info;

#ifdef AU_OS_WINDOWS
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    info.totalBytes = ms.ullTotalPhys;
    info.availableBytes = ms.ullAvailPhys;

#elif defined(AU_OS_APPLE)
    int64_t mem = 0;
    size_t len = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    info.totalBytes = static_cast<uint64_t>(mem);

    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vmstat;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&vmstat), &count) == KERN_SUCCESS) {
        info.availableBytes = static_cast<uint64_t>(vmstat.free_count) * vm_page_size;
    }

#elif defined(AU_OS_LINUX)
    struct sysinfo si;
    sysinfo(&si);
    info.totalBytes = static_cast<uint64_t>(si.totalram) * si.mem_unit;
    info.availableBytes = static_cast<uint64_t>(si.freeram) * si.mem_unit;
#endif

    return info;
}

// ============================================================================
// GPU Info (basic — reads from system where possible)
// ============================================================================

std::vector<GpuInfo> getGpuInfo() {
    std::vector<GpuInfo> gpus;

#ifdef AU_OS_APPLE
    // macOS: use system_profiler would require subprocess; return placeholder
    GpuInfo info;
    info.name = "Apple GPU";
    info.vendor = "Apple";
    gpus.push_back(info);

#elif defined(AU_OS_LINUX) && !defined(AU_OS_ANDROID)
    // Try reading /proc/driver/nvidia/gpus/*/information
    // Or lspci parsing; for now return empty
#endif

    return gpus;
}

// ============================================================================
// Environment Variables
// ============================================================================

bool setEnv(const char* name, const char* value) {
#ifdef AU_OS_WINDOWS
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

std::string getEnv(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string();
}

bool hasEnv(const char* name) {
    return std::getenv(name) != nullptr;
}

std::string getSystemPropertyValue(const char* name, const char* defaultValue) {
#ifdef AU_OS_ANDROID
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, prop) > 0) {
        return std::string(prop);
    }
#endif
    return std::string(defaultValue ? defaultValue : "");
}

int getSystemPropertyValue(const char* name, int defaultValue) {
#ifdef AU_OS_ANDROID
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, prop) > 0) {
        if (std::strlen(prop) > 0) {
            return std::atoi(prop);
        }
    }
#endif
    return defaultValue;
}

float getSystemPropertyValue(const char* name, float defaultValue) {
#ifdef AU_OS_ANDROID
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, prop) > 0) {
        if (std::strlen(prop) > 0) {
            return static_cast<float>(std::atof(prop));
        }
    }
#endif
    return defaultValue;
}

bool setSystemPropertyValue(const char* name, const char* value) {
#ifdef AU_OS_ANDROID
    return __system_property_set(name, value ? value : "") == 0;
#else
    return false;
#endif
}

bool setSystemPropertyValue(const char* name, int value) {
#ifdef AU_OS_ANDROID
    std::string valStr = std::to_string(value);
    return __system_property_set(name, valStr.c_str()) == 0;
#else
    return false;
#endif
}

bool setSystemPropertyValue(const char* name, float value) {
#ifdef AU_OS_ANDROID
    std::string valStr = std::to_string(value);
    return __system_property_set(name, valStr.c_str()) == 0;
#else
    return false;
#endif
}

// ============================================================================
// Process / Thread
// ============================================================================

uint64_t getCurrentProcessId() {
#ifdef AU_OS_WINDOWS
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

uint64_t getCurrentThreadId() {
#ifdef AU_OS_WINDOWS
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(AU_OS_APPLE)
    uint64_t tid = 0;
    ::pthread_threadid_np(::pthread_self(), &tid);
    return tid;
#else
    return static_cast<uint64_t>(::syscall(SYS_gettid));
#endif
}

// ============================================================================
// Misc
// ============================================================================

std::string getHostName() {
    char buf[256] = {0};
#ifdef AU_OS_WINDOWS
    DWORD size = sizeof(buf);
    GetComputerNameA(buf, &size);
#else
    gethostname(buf, sizeof(buf));
#endif
    return std::string(buf);
}

int getHardwareConcurrency() {
    return static_cast<int>(std::thread::hardware_concurrency());
}

}}  // namespace au::sys
