#include "flow/graph_search.h"

#include "flow/testing_internal.h"
namespace au {
namespace flow AU_LOCAL {
namespace detail {
GraphTargets::GraphTargets(Node* target, bool ancestors, Frame* frames) noexcept
    : mTarget(target), mAncestors(ancestors), mFrames(frames)
{
    mark(true);
}
GraphTargets::~GraphTargets() noexcept { mark(false); }
void GraphTargets::mark(bool value) noexcept
{
    for (Node* node = mTarget; node; node = mAncestors ? node->mParent.get() : nullptr)
        node->mSearchTarget = value;
    for (Frame* frame = mFrames; frame; frame = frame->mPrevious)
        frame->mNode->mSearchTarget = value;
}
GraphSearch::GraphSearch(bool completion) noexcept : mCompletion(completion)
{
#ifdef AURA_FLOW_TESTING
    if (mCompletion)
        mStarted = std::chrono::steady_clock::now();
#endif
}
GraphSearch::~GraphSearch() noexcept
{
    while (mHead) {
        Node* node          = mHead;
        mHead               = node->mSearchNext;
        node->mSearchNext   = nullptr;
        node->mSearchQueued = false;
    }
#ifdef AURA_FLOW_TESTING
    if (mCompletion)
        recordGraphSearch(mNodes, mEdges, 0,
                          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                         std::chrono::steady_clock::now() - mStarted)
                                                         .count()));
#endif
}
void GraphSearch::add(Node* node) noexcept
{
#ifdef AURA_FLOW_TESTING
    if (node)
        ++mEdges;
#endif
    if (!node || node->mSearchQueued)
        return;
    node->mSearchQueued = true;
    if (mTail)
        mTail->mSearchNext = node;
    else
        mHead = node;
    mTail = node;
}
bool GraphSearch::reachesTarget() noexcept
{
    for (Node* node = mHead; node; node = node->mSearchNext) {
#ifdef AURA_FLOW_TESTING
        ++mNodes;
#endif
        if (node->mSearchTarget)
            return true;
        for (const auto& pred : node->mPreds)
            add(pred.get());
        for (const auto& member : node->mMembers)
            add(member.get());
        add(node->mEntry.get());
        add(node->mGate.get());
        if (!mCompletion)
            continue;
        add(node->mWaitTarget);
        add(node->mExecutingChild);
        for (Node* child = node->mFirstChild; child; child = child->mSiblingNext)
            add(child);
    }
    return false;
}
void linkChild(Node* node) noexcept
{
    Node* parent = node->mParent.get();
    if (!parent)
        return;
    node->mSiblingNext = parent->mFirstChild;
    if (parent->mFirstChild)
        parent->mFirstChild->mSiblingPrev = node;
    parent->mFirstChild = node;
}
void unlinkChild(Node* node) noexcept
{
    Node* parent = node->mParent.get();
    if (!parent)
        return;
    if (node->mSiblingPrev)
        node->mSiblingPrev->mSiblingNext = node->mSiblingNext;
    else
        parent->mFirstChild = node->mSiblingNext;
    if (node->mSiblingNext)
        node->mSiblingNext->mSiblingPrev = node->mSiblingPrev;
    node->mSiblingPrev = nullptr;
    node->mSiblingNext = nullptr;
}
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
