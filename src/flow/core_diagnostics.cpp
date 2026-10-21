#include "flow/core.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
int Core::dot(Node* top, const char* path) noexcept
{
    if (!owns(top))
        return au::err::kErrorInvalidHandle;
    if (!path)
        return au::err::kErrorNullPointer;
    try {
        GraphSnapshot graph;
        graph.mCaptured = top->mGraphCaptured;
        std::vector<NodeRef> todo{NodeRef(top)};
        while (!todo.empty()) {
            NodeRef pin = std::move(todo.back());
            todo.pop_back();
            auto* node = pin.get();
            if (!graph.mIndex.emplace(node->mId, graph.mNodes.size()).second)
                continue;
            GraphNodeSnapshot item;
            item.mId    = node->mId;
            item.mName  = node->mName;
            item.mGroup = node->mKind == Kind::Group;
            item.mState = node->mState.load(std::memory_order_acquire);
            if (finalState(item.mState)) {
                item.mCode  = node->mResult.mCode;
                item.mCause = node->mResult.mCauseTaskId;
            }
            Edge* children;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                children = node->mDynamicChildren;
            }
            for (auto* edge = children; edge; edge = edge->mNext) {
                item.mChildren.push_back(edge->mTarget->mId);
                todo.emplace_back(edge->mTarget);
            }
            for (const auto& member : node->mMembers) {
                item.mMembers.push_back(member->mId);
                todo.emplace_back(member);
            }
            for (const auto& pred : node->mPreds) {
                item.mPreds.push_back(pred->mId);
                todo.emplace_back(pred);
            }
            graph.mDropped += node->mGraphDropped.load(std::memory_order_relaxed);
            graph.mNodes.push_back(std::move(item));
        }
        return writeDot(graph, path);
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}

void Core::trace(Node* node, const char* name, char phase, int code) noexcept
{
    if (!mDiagnostics.enabled())
        return;  // Tracing is observational, default off.
    const int           worker = gWorker;
    const std::size_t   index  = worker < 0 ? kExternalTraceLane : static_cast<std::size_t>(worker);
    const std::uint64_t thread = worker < 0 ? au::sys::flow::threadId() : mWorkers[index].mThreadId;
    mDiagnostics.record(index, node->mId, thread, name, phase, code, node->mParent.get() ? node->mParent->mId : 0,
                        node->mName.data(), node->mName.size());
}

void Core::externalProfile(const ExternalProfile& profile) noexcept
{
    if (!mDiagnostics.enabled())
        return;  // Observation only; never affects task completion.
    const int         worker = gWorker;
    const std::size_t index  = worker < 0 ? kExternalTraceLane : static_cast<std::size_t>(worker);
    const auto        thread = worker < 0 ? au::sys::flow::threadId() : mWorkers[index].mThreadId;
    mDiagnostics.record(index, profile.mTask, thread, "GPU profile", 'i', 0, 0, nullptr, 0, &profile);
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
