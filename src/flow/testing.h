#ifndef AURA_FLOW_TESTING_H_
#define AURA_FLOW_TESTING_H_

#include <cstddef>
#include <cstdint>
#include <array>

// Private test seams, compiled only when the flow test configuration is enabled.
namespace au {
namespace flow {
namespace detail { struct QueueHooks; }
namespace testing {
enum class Failure { Node, Edge, Worker, Payload, Order };
struct SnapshotHook { void (*mInvoke)(void*); void* mPayload; };
enum class RuntimePoint { BeforeSleep, AfterRetire, QueryPinned, BeforePendingQuery, QueueProbeMiss, AfterAdmission };
struct RuntimeHook {
    void (*mInvoke)(void*, au::flow::testing::RuntimePoint, std::size_t) noexcept;
    void* mPayload;
};
struct RuntimeStats {
    std::size_t mLive = 0, mRetireBlocked = 0;
    int mPollOwner = -1;
    std::uint64_t mWaitTimeouts = 0, mControlWakes = 0, mWorkWakes = 0, mPollPasses = 0, mRetirements = 0;
    std::array<std::uint64_t, 8> mGenerations{};
};
int queueHooks(const au::flow::detail::QueueHooks& hooks) noexcept;
void runtimeHook(au::flow::testing::RuntimeHook* hook) noexcept;
au::flow::testing::RuntimeStats runtimeStats() noexcept;
void failAfter(au::flow::testing::Failure failure, int successfulOperations) noexcept;
void snapshotHook(au::flow::testing::SnapshotHook* hook) noexcept;
std::size_t budget() noexcept;
int setBudget(std::size_t budget) noexcept;
// Limits only structured reservations for counter-boundary models; SIZE_MAX restores production behavior.
void setOutstandingLimit(std::size_t limit) noexcept;
} // namespace testing
} // namespace flow
} // namespace au
#endif
