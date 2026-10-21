#ifndef AURA_FLOW_XFLOW_TYPES_H_
#define AURA_FLOW_XFLOW_TYPES_H_

#include <cstddef>
#include <cstdint>

#include "sys/xvisibility.h"

namespace au {
namespace flow AU_LOCAL {

// Flow owns -5001..-5004; common errors remain in log/xerror.h. Task bodies may
// return any nonzero int (including positive codes). Inspect TaskResult domain
// and native code to distinguish scheduler/driver failures from business codes.
constexpr int kErrorDependencyFailed   = -5001;
constexpr int kErrorCycleDetected      = -5002;
constexpr int kErrorWouldDeadlock      = -5003;
constexpr int kErrorUnhandledException = -5004;

enum class TaskState : std::uint32_t
{
    Invalid,
    Draft,
    Pending,
    Ready,
    Running,
    Waiting,
    Succeeded,
    Failed,
    Skipped,
    Rejected
};
enum class ErrorDomain : std::uint32_t
{
    None,
    Flow,
    Callable,
    Memory,
    OpenCL,
    System
};
enum class AffinityMode : std::uint32_t
{
    SystemDefault,
    ExplicitCpuSet
};

struct TaskResult
{
    std::uint32_t         mStructSize;
    au::flow::TaskState   mState;
    au::flow::ErrorDomain mDomain;
    int                   mCode;
    int                   mNativeCode;
    std::uint64_t         mCauseTaskId;
};
struct AffinityPolicy
{
    au::flow::AffinityMode mMode;
    const std::uint32_t*   mCpuIds;
    std::size_t            mCount;
};
struct WorkerInfo
{
    std::uint32_t mStructSize;
    std::uint32_t mWorkerId;
    std::uint64_t mThreadId;
    std::uint64_t mRequestedVersion;
    std::uint64_t mAppliedVersion;
    int           mAffinityCode;
};

// Recognizes flow codes and delegates common/unknown codes to au::err. Numeric
// lookup alone does not determine provenance; consult TaskResult::mDomain.
const char* getErrorStr(int code) noexcept;

}  // namespace flow AU_LOCAL
}  // namespace au
#endif
