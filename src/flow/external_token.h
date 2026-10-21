#ifndef AURA_FLOW_EXTERNAL_TOKEN_H_
#define AURA_FLOW_EXTERNAL_TOKEN_H_

#include "flow/completion.h"
#include "flow/node.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
struct ExternalToken
{
    Core*                     mCore = nullptr;
    NodeRef                   mParent;
    ExternalOps               mOps;
    void*                     mPayload = nullptr;
    std::uint64_t             mOrder   = 0;
    std::atomic<unsigned int> mNotification{0};  // 0 pending, 1 publishing, 2 terminal status published.
    std::atomic<bool>         mRegistrar{true};
    std::atomic<bool>         mQueued{false};
    ExternalStatus            mStatus;
    ReadyLink                 mLink;
    ExternalToken*            mPendingNext = nullptr;
};

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_EXTERNAL_TOKEN_H_
