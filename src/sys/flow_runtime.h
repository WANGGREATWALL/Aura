#ifndef AURA_SYS_FLOW_RUNTIME_H_
#define AURA_SYS_FLOW_RUNTIME_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace au {
namespace sys {
namespace flow {

// The count and sleep predicate share the same mutex: a notification cannot be
// lost between observing zero and entering the OS wait. No per-post allocation.
class WorkSemaphore {
public:
    void post(std::size_t count = 1) noexcept;
    bool tryAcquire() noexcept;
    bool acquireUntil(std::chrono::steady_clock::time_point deadline) noexcept;
private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCount = 0;
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
