#include "flow/core.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
Core& core() noexcept
{
    static Core instance;
    return instance;
}
Edge* closedEdges() noexcept
{
    static Edge closed;
    return &closed;
}
bool reserveOutstanding(Node* node) noexcept
{
    std::size_t limit = SIZE_MAX;
#ifdef AURA_FLOW_TESTING
    limit = gOutstandingLimit.load(std::memory_order_relaxed);  // Fault-model ceiling only.
#endif
    std::size_t current = node->mOutstanding.load(std::memory_order_relaxed);
    do {
        if (current >= limit)
            return false;
    } while (!node->mOutstanding.compare_exchange_weak(current, current + 1, std::memory_order_relaxed));
    // Reservation only: the accepting body keeps the parent alive. Last release
    // is acq_rel and gathers each child's/event's independently published writes.
    return true;
}
bool finalState(au::flow::TaskState state) noexcept { return state >= au::flow::TaskState::Succeeded; }

Core::Core() noexcept
{
    // Construct mm first so structured cleanup during static shutdown never
    // discovers an already destroyed owner registry. No workers are created.
    initializeMemoryContext();
    if (au::sys::flow::allowedCpus(mAllowed) != 0) {
        // A query failure must not turn a usable CPU scheduler into an error.
        mBudget.store(1, std::memory_order_relaxed);  // Not shared during construction.
    } else {
        mBudget.store(std::min<std::size_t>(kMaxWorkers, mAllowed.size()), std::memory_order_relaxed);
    }
}

Core::~Core() noexcept { (void)shutdown(); }

NodeRef Core::make(Kind kind)
{
#ifdef AURA_FLOW_TESTING
    if (fail(au::flow::testing::Failure::Node))
        throw std::bad_alloc();
#endif
    std::uint64_t id = mNextId.load(std::memory_order_relaxed);  // Identity only.
    do {
        if (id == UINT64_MAX)
            throw CreateFailure{au::err::kErrorOverflow};
    } while (
        !mNextId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));  // Saturate; never wrap to a stale ID.
    auto node   = NodeRef::adopt(mNodes.create(this, id, kind));
    node->mName = std::to_string(id);  // Cold label creation; the owner also handles construction rollback.
    return node;
}

void Core::recycle(Node* node) noexcept
{
    while (node->mDynamicChildren) {
        auto* edge             = node->mDynamicChildren;
        node->mDynamicChildren = edge->mNext;
        mEdges.destroy(edge);
    }
    mNodes.destroy(node);  // Iterative NodeRef reclamation also handles retained diagnostic descendants.
}
void retain(Node* node) noexcept
{
    if (node)
        node->mRefs.fetch_add(1, std::memory_order_relaxed);  // Caller already owns a live reference.
}
void release(Node* node) noexcept
{
    if (!node || node->mRefs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;  // See all prior owners before destroying.
    node->mReclaimNext = gReclaim;
    gReclaim           = node;
    if (gReclaiming)
        return;
    gReclaiming = true;
    while (gReclaim) {
        node     = gReclaim;
        gReclaim = node->mReclaimNext;
        for (const auto& member : node->mMembers)
            if (member->mGroup == node)
                member->mGroup = nullptr;
        node->mOwner->recycle(node);
    }
    gReclaiming = false;
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
