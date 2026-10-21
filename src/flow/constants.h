#ifndef AURA_FLOW_CONSTANTS_H_
#define AURA_FLOW_CONSTANTS_H_
#include <chrono>
#include <cstddef>
namespace au {
namespace flow {
namespace detail {
// Current worker/storage cap, independent of the tiled participant contract.
constexpr std::size_t kMaxWorkers        = 8;
constexpr std::size_t kExternalTraceLane = kMaxWorkers;
// Bounds cooperative native stack growth. A helper needs one additional frame.
constexpr unsigned int kMaxDispatchDepth = 32;
// Separate fairness budgets: CPU handoffs, external completion dispatch, polling
// round batches and queue publication-gap probes happen at different layers.
constexpr unsigned int kContinuationBurst = 8;
constexpr unsigned int kCompletionBurst   = 8;
constexpr unsigned int kPollBatch         = 8;
constexpr unsigned int kQueueProbeLimit   = 8;
// Caller participates; user-selected maximum is four, including the caller.
constexpr std::size_t kTileParticipants = 4;
// B1/B2 compared 1 versus 8. Keep that measured candidate; not a time quantum.
constexpr unsigned int kHelperTileBatch = 8;
// B3 decision and fixed-1ms comparison are recorded in its validation report.
constexpr auto kIdleTimeout = std::chrono::seconds(2);
constexpr auto kPollMinimum = std::chrono::microseconds(200);
constexpr auto kPollMaximum = std::chrono::microseconds(2000);
}  // namespace detail
}  // namespace flow
}  // namespace au
#endif
