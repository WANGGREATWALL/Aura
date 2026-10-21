#include "flow/task_errors.h"
namespace au {
namespace flow AU_LOCAL {
namespace detail {
bool TaskErrors::scoped(std::uint64_t id, int code, au::flow::ErrorDomain domain, int native,
                        std::uint64_t cause) noexcept
{
    if (!code)
        return false;
    std::lock_guard<std::mutex> lock(mMutex);
    if (mScoped.mCode)
        return false;
    mScoped             = taskResult(id, code, domain);
    mScoped.mNativeCode = native;
    if (cause)
        mScoped.mCauseTaskId = cause;
    return true;
}
void TaskErrors::inherit(const au_flow_result& source, std::uint64_t order) noexcept
{
    if (!source.mCode)
        return;
    const bool                  skipped = source.mState == static_cast<std::uint32_t>(au::flow::TaskState::Skipped);
    std::lock_guard<std::mutex> lock(mMutex);
    const bool inheritedSkipped = mInherited.mState == static_cast<std::uint32_t>(au::flow::TaskState::Skipped);
    if (!mInherited.mCode || (inheritedSkipped && !skipped) ||
        (inheritedSkipped == skipped && order < mInheritedOrder)) {
        mInherited      = source;
        mInheritedOrder = order;
    }
}
void TaskErrors::body(int code) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    mBodyCode = code;
    if (code != au::flow::kErrorUnhandledException)
        mBodyDomain = au::flow::ErrorDomain::Callable;
}
void TaskErrors::exception() noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    mBodyDomain = au::flow::ErrorDomain::Flow;
}
void TaskErrors::skip() noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    mSkipped = true;
}
au_flow_result TaskErrors::complete(std::uint64_t id) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    // Body > first scoped failure > inherited failure > success. An actual
    // failure beats a skipped one; inheritance breaks ties by acceptance order.
    auto result = mBodyCode          ? taskResult(id, mBodyCode, mBodyDomain)
                  : mScoped.mCode    ? mScoped
                  : mInherited.mCode ? mInherited
                                     : taskResult(id, 0, au::flow::ErrorDomain::None);
    // Forwarding a matching reported code preserves its native provenance.
    if (mBodyCode && mBodyDomain == au::flow::ErrorDomain::Callable) {
        if (mScoped.mCode == mBodyCode)
            result = mScoped;
        else if (mInherited.mCode == mBodyCode && mInherited.mCauseTaskId == id)
            result = mInherited;
    }
    result.mState = static_cast<std::uint32_t>(mSkipped       ? au::flow::TaskState::Skipped
                                               : result.mCode ? au::flow::TaskState::Failed
                                                              : au::flow::TaskState::Succeeded);
    return result;
}
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
