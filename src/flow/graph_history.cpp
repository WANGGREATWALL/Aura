#include "flow/diagnostics.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
GraphHistory::~GraphHistory() noexcept
{
    while (mBatches) {
        auto next = std::move(mBatches->mNext);
        mBatches  = std::move(next);
    }
}
void GraphHistory::publish(std::unique_ptr<Batch> batch) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    batch->mNext = std::move(mBatches);
    mBatches     = std::move(batch);
}
void GraphHistory::state(GraphNodeSnapshot* node, au::flow::TaskState state) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    node->mState = state;
}
void GraphHistory::complete(GraphNodeSnapshot* node, const au_flow_result& result) noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    node->mState = static_cast<au::flow::TaskState>(result.mState);
    node->mCode  = result.mCode;
    node->mCause = result.mCauseTaskId;
}
bool GraphHistory::contains(std::uint64_t id) const noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    for (const Batch* batch = mBatches.get(); batch; batch = batch->mNext.get())
        for (const auto& node : batch->mNodes)
            if (node.mId == id)
                return true;
    return false;
}
GraphSnapshot GraphHistory::snapshot() const
{
    GraphSnapshot graph;
    graph.mCaptured = true;
    graph.mDropped  = mDropped.load(std::memory_order_relaxed);
    const Batch* head;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        head = mBatches.get();
    }
    // Published batch links and structural metadata are immutable. The caller
    // pins this history. Copy/allocate strings and containers outside the lock;
    // only the three mutable completion fields need a short per-node snapshot.
    for (const Batch* batch = head; batch; batch = batch->mNext.get()) {
        for (const auto& node : batch->mNodes) {
            GraphNodeSnapshot item;
            item.mId       = node.mId;
            item.mName     = node.mName;
            item.mGroup    = node.mGroup;
            item.mPreds    = node.mPreds;
            item.mMembers  = node.mMembers;
            item.mChildren = node.mChildren;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                item.mState = node.mState;
                item.mCode  = node.mCode;
                item.mCause = node.mCause;
            }
            const auto inserted = graph.mIndex.emplace(item.mId, graph.mNodes.size());
            if (inserted.second)
                graph.mNodes.push_back(std::move(item));
            else if (item.mState >= au::flow::TaskState::Succeeded)
                graph.mNodes[inserted.first->second] = std::move(item);
        }
    }
    for (const Batch* batch = head; batch; batch = batch->mNext.get()) {
        if (!batch->mParent)
            continue;
        const auto parent = graph.mIndex.find(batch->mParent), child = graph.mIndex.find(batch->mTop);
        if (parent != graph.mIndex.end() && child != graph.mIndex.end())
            graph.mNodes[parent->second].mChildren.push_back(batch->mTop);
    }
    return graph;
}
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
