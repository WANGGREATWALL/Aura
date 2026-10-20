#ifndef AURA_FLOW_COMPLETION_H_
#define AURA_FLOW_COMPLETION_H_

#include "flow/xflow.h"

namespace au {
namespace flow {
namespace detail {

struct ExternalToken;
struct ExternalStatus {
    int mCode = 0;
    int mNativeCode = 0;
    au::flow::ErrorDomain mDomain = au::flow::ErrorDomain::None;
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
// Arm before registering the native callback, including immediate callbacks.
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
