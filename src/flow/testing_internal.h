#ifndef AURA_FLOW_TESTING_INTERNAL_H_
#define AURA_FLOW_TESTING_INTERNAL_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "flow/testing.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
#ifdef AURA_FLOW_TESTING
extern std::array<std::atomic<int>, 5>               gFailure;
extern std::atomic<std::size_t>                      gOutstandingLimit;
extern std::atomic<au::flow::testing::SnapshotHook*> gSnapshotHook;
extern std::mutex                                    gRuntimeHookMutex;
extern std::condition_variable                       gRuntimeHookChanged;
extern au::flow::testing::RuntimeHook*               gRuntimeHook;
extern std::size_t                                   gRuntimeReaders;
void runtimePoint(au::flow::testing::RuntimePoint point, std::size_t id) noexcept;
void recordGraphSearch(std::uint64_t nodes, std::uint64_t edges, std::uint64_t activeScans,
                       std::uint64_t nanoseconds) noexcept;
void recordGraphWaitLock(std::uint64_t nanoseconds) noexcept;
// Construct immediately after acquiring the graph mutex; destruction precedes
// its release. This measures the checked region, not mutex acquisition latency.
class GraphWaitLock
{
public:
    GraphWaitLock() noexcept : mStarted(std::chrono::steady_clock::now()) {}
    ~GraphWaitLock() noexcept
    {
        recordGraphWaitLock(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mStarted).count()));
    }

private:
    std::chrono::steady_clock::time_point mStarted;
};
bool fail(au::flow::testing::Failure failure) noexcept;
#endif

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_TESTING_INTERNAL_H_
