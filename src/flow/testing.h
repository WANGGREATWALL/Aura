#ifndef AURA_FLOW_TESTING_H_
#define AURA_FLOW_TESTING_H_

#include <cstddef>

// Private test seams, compiled only when the flow test configuration is enabled.
namespace au {
namespace flow {
namespace testing {
enum class Failure { Node, Edge, Worker };
struct SnapshotHook { void (*mInvoke)(void*); void* mPayload; };
void failAfter(au::flow::testing::Failure failure, int successfulOperations) noexcept;
void snapshotHook(au::flow::testing::SnapshotHook* hook) noexcept;
std::size_t budget() noexcept;
int setBudget(std::size_t budget) noexcept;
} // namespace testing
} // namespace flow
} // namespace au
#endif
