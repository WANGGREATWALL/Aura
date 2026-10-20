#ifndef AURA_FLOW_COMPLETION_H_
#define AURA_FLOW_COMPLETION_H_

#include "flow/xflow.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

struct ExternalToken;
struct ExternalProfile {
    std::uint64_t mTask = 0;
    std::uint64_t mHostSubmitUs = 0, mHostObservedUs = 0;
    std::uint64_t mQueuedNs = 0, mSubmitNs = 0, mStartNs = 0, mEndNs = 0;
    int mNativeCode = 0;
    bool mAvailable = false;
};
bool tracingEnabled() noexcept;
void recordExternalProfile(const au::flow::detail::ExternalProfile& profile) noexcept;
bool cleanupActive() noexcept;
// Token remains live until the provider publishes done. This call pins its
// parent independently, so publication inside fn may safely release the token.
void cleanupExternal(au::flow::detail::ExternalToken* token, void (*fn)(void*) noexcept, void* payload) noexcept;
// Managed workers may execute ready work while a provider waits on a resource
// event. External callers and cleanup extents never acquire scheduler work.
bool assistExternalWait() noexcept;
void* pauseCpuSlice() noexcept;
void resumeCpuSlice(void* frame) noexcept;
class CpuSlicePause final {
public:
    explicit CpuSlicePause(bool enabled = true) noexcept : mFrame(enabled ? au::flow::detail::pauseCpuSlice() : nullptr) {}
    ~CpuSlicePause() noexcept { au::flow::detail::resumeCpuSlice(mFrame); }
    CpuSlicePause(const CpuSlicePause&) = delete;
    CpuSlicePause& operator=(const CpuSlicePause&) = delete;
    CpuSlicePause(CpuSlicePause&&) = delete;
    CpuSlicePause& operator=(CpuSlicePause&&) = delete;
private:
    void* mFrame;
};
struct ExternalStatus {
    int mCode = 0;
    int mNativeCode = 0;
    au::flow::ErrorDomain mDomain = au::flow::ErrorDomain::None;
    std::uint64_t mCauseTaskId = 0; // Optional predecessor root, preserved through asynchronous native chains.
};
struct ExternalOps {
    // A query error is not proof that the device stopped. Set done only after
    // establishing terminal completion. Called without scheduler locks.
    int (*mPoll)(void*, bool* done, au::flow::detail::ExternalStatus*) noexcept = nullptr;
    au::flow::detail::ExternalStatus (*mFinalize)(void*) noexcept = nullptr;
    void (*mRelease)(void*) noexcept = nullptr;
};

// Prepare BEFORE native enqueue. A null token means there is no managed task;
// the provider must use its standalone pending registry instead. On prepare
// failure payload ownership stays with the caller. On non-null success it moves
// to the token until the provider's normal-thread release callback.
int prepareExternal(const au::flow::detail::ExternalOps& ops, void* payload,
                    au::flow::detail::ExternalToken** out) noexcept;
std::uint64_t currentTaskId() noexcept;
// Synchronous wrapper preparation/enqueue failures can enrich an int return
// with the actual native status without requiring an accepted native event.
void recordExternalError(const au::flow::detail::ExternalStatus& status) noexcept;
// Pins protect token storage, not a provider callback return stack during dlclose.
// OpenCL v1 uses polling and never registers native callbacks.
// Arm before registering a generic provider callback, including immediate callbacks.
void armExternalCallback(au::flow::detail::ExternalToken* token) noexcept;
void leaveExternalCallback(au::flow::detail::ExternalToken* token) noexcept;
// Notification only: atomics, intrusive publication and a wake. Does not run
// cleanup/user code. A live registrar or callback pin must cover this call.
void notifyExternal(au::flow::detail::ExternalToken* token,
                    const au::flow::detail::ExternalStatus& status) noexcept;
// On callback registration failure, disarm its unused pin and enable fallback.
void pollExternal(au::flow::detail::ExternalToken* token) noexcept;
// Last registrar access. No use of token after this call without a callback pin.
void leaveExternalRegistrar(au::flow::detail::ExternalToken* token) noexcept;

} // namespace detail
} // namespace flow
} // namespace au
#endif
