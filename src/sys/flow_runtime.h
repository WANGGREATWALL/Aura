#ifndef AURA_SYS_FLOW_RUNTIME_H_
#define AURA_SYS_FLOW_RUNTIME_H_

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace au {
namespace sys {
namespace flow {

// Work permits represent queue items only. Control changes have a separate
// epoch under the same sleep mutex. A parked surplus worker uses the control
// CV so it cannot swallow notify_one intended for a work-eligible waiter.
class WorkSemaphore {
public:
    enum class Wake { Work, Control, Timeout };
    void post(std::size_t count = 1) noexcept;
    void control() noexcept;
    std::uint64_t epoch() noexcept;
    bool tryAcquire() noexcept;
    au::sys::flow::WorkSemaphore::Wake acquireUntil(std::uint64_t observed,
        std::chrono::steady_clock::time_point deadline, bool allowWork = true) noexcept;
private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::condition_variable mParked;
    std::size_t mCount = 0;
    std::atomic<std::uint64_t> mEpoch{0}; // Writes under mMutex; lock-free snapshots are rechecked before sleep.
};

int allowedCpus(std::vector<std::uint32_t>& cpus) noexcept;
int applyAffinity(const std::vector<std::uint32_t>& cpus) noexcept;
int currentAffinity(std::vector<std::uint32_t>& cpus) noexcept;
int threadAffinity(std::uint64_t thread, std::vector<std::uint32_t>& cpus) noexcept;
int affinitySupported() noexcept;
std::uint64_t threadId() noexcept;

} // namespace flow
} // namespace sys
} // namespace au
#endif
