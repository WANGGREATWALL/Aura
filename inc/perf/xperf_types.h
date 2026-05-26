#ifndef AURA_PERF_XPERF_TYPES_H_
#define AURA_PERF_XPERF_TYPES_H_

#include <cstdint>

namespace au {
namespace perf {

constexpr int32_t  LEVEL_OFF      = -1;
constexpr int32_t  LEVEL_ALL      = INT32_MAX;
constexpr uint32_t HARD_MAX_DEPTH = 512;

struct PerfScope
{
    uint64_t opaque[8];
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XPERF_TYPES_H_
