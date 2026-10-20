#include "sys/flow_runtime.h"

#include <functional>
#include <limits>
#include <thread>
#include "log/xerror.h"

#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

void au::sys::flow::WorkSemaphore::post(std::size_t count) noexcept
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCount += count;
    }
    if (count == 1) mChanged.notify_one();
    else mChanged.notify_all();
}
bool au::sys::flow::WorkSemaphore::tryAcquire() noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mCount) return false;
    --mCount;
    return true;
}
bool au::sys::flow::WorkSemaphore::acquireUntil(std::chrono::steady_clock::time_point deadline) noexcept
{
    std::unique_lock<std::mutex> lock(mMutex);
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        mChanged.wait(lock, [this] { return mCount != 0; });
    } else if (!mChanged.wait_until(lock, deadline, [this] { return mCount != 0; })) return false;
    --mCount;
    return true;
}

int au::sys::flow::allowedCpus(std::vector<std::uint32_t>& cpus) noexcept
{
    return au::sys::flow::currentAffinity(cpus);
}
int au::sys::flow::currentAffinity(std::vector<std::uint32_t>& cpus) noexcept
{
    return au::sys::flow::threadAffinity(0, cpus);
}
int au::sys::flow::affinitySupported() noexcept
{
#if defined(__linux__) || defined(_WIN32)
    return 0;
#else
    return au::err::kErrorNotSupported;
#endif
}
int au::sys::flow::threadAffinity(std::uint64_t thread, std::vector<std::uint32_t>& cpus) noexcept
{
    try {
        cpus.clear();
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (thread > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return au::err::kErrorOutOfRange;
        if (sched_getaffinity(static_cast<pid_t>(thread), sizeof(set), &set) != 0) return au::err::kErrorPlatformAPI;
        cpus.reserve(CPU_COUNT(&set));
        for (unsigned int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) cpus.push_back(i);
#elif defined(_WIN32)
        HANDLE handle = thread ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread)) : GetCurrentThread();
        if (!handle) return au::err::kErrorPlatformAPI;
        GROUP_AFFINITY affinity{};
        const BOOL ok = GetThreadGroupAffinity(handle, &affinity);
        if (thread) CloseHandle(handle);
        if (!ok) return au::err::kErrorPlatformAPI;
        if (affinity.Group != 0) return au::err::kErrorNotSupported;
        for (unsigned int i = 0; i < sizeof(affinity.Mask) * 8; ++i)
            if ((affinity.Mask >> i) & 1) cpus.push_back(i);
#else
        (void)thread;
        const unsigned int count = std::thread::hardware_concurrency();
        for (unsigned int i = 0; i < (count ? count : 1); ++i) cpus.push_back(i);
#endif
        return cpus.empty() ? au::err::kErrorNotSupported : 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
int au::sys::flow::applyAffinity(const std::vector<std::uint32_t>& cpus) noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (std::uint32_t cpu : cpus) {
        if (cpu >= CPU_SETSIZE) return au::err::kErrorOutOfRange;
        CPU_SET(cpu, &set);
    }
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? 0 : au::err::kErrorPlatformAPI;
#elif defined(_WIN32)
    DWORD_PTR mask = 0;
    for (std::uint32_t cpu : cpus) {
        if (cpu >= sizeof(mask) * 8) return au::err::kErrorNotSupported;
        mask |= static_cast<DWORD_PTR>(1) << cpu;
    }
    return SetThreadAffinityMask(GetCurrentThread(), mask) ? 0 : au::err::kErrorPlatformAPI;
#else
    (void)cpus;
    return au::err::kErrorNotSupported;
#endif
}
std::uint64_t au::sys::flow::threadId() noexcept
{
#if defined(__linux__)
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#elif defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>()(std::this_thread::get_id()));
#endif
}
