#ifndef AURA_FLOW_TASK_ERRORS_H_
#define AURA_FLOW_TASK_ERRORS_H_
#include <cstdint>
#include <mutex>

#include "flow/xflow_api.h"
#include "flow/xflow_types.h"
namespace au {
namespace flow AU_LOCAL {
namespace detail {
inline au_flow_result taskResult(std::uint64_t id, int code, au::flow::ErrorDomain domain) noexcept
{
    return {sizeof(au_flow_result),
            static_cast<std::uint32_t>(code ? au::flow::TaskState::Failed : au::flow::TaskState::Succeeded),
            static_cast<std::uint32_t>(code ? domain : au::flow::ErrorDomain::None),
            code,
            0,
            code ? id : 0};
}
// Owns result candidates and their mutex; callers never nest two TaskErrors
// locks. User/driver calls and error logging happen after these methods return.
class TaskErrors
{
public:
    bool scoped(std::uint64_t id, int code, au::flow::ErrorDomain domain, int native, std::uint64_t cause) noexcept;
    void inherit(const au_flow_result& source, std::uint64_t order) noexcept;
    void body(int code) noexcept;
    void exception() noexcept;
    void skip() noexcept;
    au_flow_result complete(std::uint64_t id) noexcept;
    // Activation reads after the final acq_rel unresolved decrement. Final
    // propagation reads skipped only after all body/child reservations drain.
    bool dependencyFailed() const noexcept { return mInherited.mCode != 0; }
    bool skipped() const noexcept { return mSkipped; }

private:
    std::mutex            mMutex;
    int                   mBodyCode   = 0;
    au::flow::ErrorDomain mBodyDomain = au::flow::ErrorDomain::Callable;
    au_flow_result        mInherited{};
    std::uint64_t         mInheritedOrder = UINT64_MAX;
    au_flow_result        mScoped{};
    bool                  mSkipped = false;
};
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
#endif
