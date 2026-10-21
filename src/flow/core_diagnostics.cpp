#include <unordered_set>

#include "flow/core.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
namespace {
GraphNodeSnapshot nodeSnapshot(Node* node)
{
    GraphNodeSnapshot item;
    item.mId    = node->mId;
    item.mName  = node->mName;
    item.mGroup = node->mKind == Kind::Group;
    item.mState = node->mState.load(std::memory_order_acquire);
    if (finalState(item.mState)) {
        item.mCode  = node->mResult.mCode;
        item.mCause = node->mResult.mCauseTaskId;
    }
    for (const auto& member : node->mMembers)
        item.mMembers.push_back(member->mId);
    for (const auto& pred : node->mPreds)
        item.mPreds.push_back(pred->mId);
    return item;
}
void mergeNode(GraphSnapshot& graph, GraphNodeSnapshot node, bool live = false)
{
    const auto inserted = graph.mIndex.emplace(node.mId, graph.mNodes.size());
    if (inserted.second)
        graph.mNodes.push_back(std::move(node));
    else {
        auto& existing = graph.mNodes[inserted.first->second];
        auto  children = std::move(existing.mChildren);
        for (auto id : node.mChildren)
            if (std::find(children.begin(), children.end(), id) == children.end())
                children.push_back(id);
        if (live ||
            (node.mState >= au::flow::TaskState::Succeeded && existing.mState < au::flow::TaskState::Succeeded)) {
            node.mChildren = std::move(children);
            existing       = std::move(node);
        } else
            existing.mChildren = std::move(children);
    }
}
}  // namespace
Core::CapturePreparation Core::prepareCapture(const std::vector<Prepared>& batch, Node* parent) noexcept
{
    try {
        CapturePreparation capture;
        capture.mHistory =
            parent && parent->mDiagnostic ? parent->mDiagnostic->mHistory : std::make_shared<GraphHistory>();
        capture.mBatch.reset(new GraphHistory::Batch());
        capture.mBatch->mParent = parent ? parent->mId : 0;
        capture.mBatch->mTop    = batch[0].mNode->mId;
        std::vector<Node*>                     todo;
        std::unordered_map<Node*, std::size_t> indices;
        for (const auto& p : batch)
            if (p.mNode->mKind != Kind::Entry)
                todo.push_back(p.mNode);
        for (std::size_t i = 0; i < todo.size(); ++i) {
            Node* node = todo[i];
            // Do not copy a captured predecessor chain into every dynamic
            // batch. New Drafts still require their own stable record.
            if ((node->mDiagnostic && node->mDiagnostic->mHistory == capture.mHistory) ||
                (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft &&
                 capture.mHistory->contains(node->mId)))
                continue;
            if (!indices.emplace(node, capture.mBatch->mNodes.size()).second)
                continue;
            capture.mBatch->mNodes.push_back(nodeSnapshot(node));
            for (const auto& member : node->mMembers)
                todo.push_back(member.get());
            for (const auto& pred : node->mPreds)
                todo.push_back(pred.get());
        }
        capture.mNodes.resize(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            Node* node = batch[i].mNode;
            if (node->mKind == Kind::Entry)
                continue;
            auto* record   = &capture.mBatch->mNodes[indices.at(node)];
            record->mState = node->mKind == Kind::Group ? au::flow::TaskState::Waiting : au::flow::TaskState::Pending;
            capture.mNodes[i].reset(new CapturedNode{capture.mHistory, record});
        }
        return capture;
    } catch (...) {
        return {};
    }  // Diagnostics must never reject a valid task.
}
int Core::dot(Node* top, const char* path) noexcept
{
    if (!owns(top))
        return au::err::kErrorInvalidHandle;
    if (!path)
        return au::err::kErrorNullPointer;
    try {
        GraphSnapshot graph;
        graph.mCaptured = top->mGraphCaptured;
        std::vector<NodeRef>                    todo{NodeRef(top)};
        std::unordered_set<std::uint64_t>       visited;
        std::unordered_set<const GraphHistory*> histories;
        while (!todo.empty()) {
            NodeRef pin = std::move(todo.back());
            todo.pop_back();
            auto* node = pin.get();
            if (!visited.insert(node->mId).second)
                continue;
            if (node->mDiagnostic && histories.insert(node->mDiagnostic->mHistory.get()).second) {
                auto history = node->mDiagnostic->mHistory->snapshot();
                graph.mDropped += history.mDropped;
                for (auto& item : history.mNodes)
                    mergeNode(graph, std::move(item));
            }
            mergeNode(graph, nodeSnapshot(node), true);
            graph.mDropped += node->mGraphDropped.load(std::memory_order_relaxed);
            for (const auto& member : node->mMembers)
                todo.emplace_back(member);
            for (const auto& pred : node->mPreds)
                todo.emplace_back(pred);
        }
        // A shared scope history can include ancestors/siblings. Export only
        // nodes reachable from the requested task, as before; no scheduler pins
        // are required to traverse historical children.
        GraphSnapshot result;
        result.mCaptured = graph.mCaptured;
        result.mDropped  = graph.mDropped;
        std::vector<std::uint64_t> ids{top->mId};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto found = graph.mIndex.find(ids[i]);
            if (found == graph.mIndex.end() || !result.mIndex.emplace(ids[i], result.mNodes.size()).second)
                continue;
            const auto& item = graph.mNodes[found->second];
            result.mNodes.push_back(item);
            for (auto id : item.mMembers)
                ids.push_back(id);
            for (auto id : item.mPreds)
                ids.push_back(id);
            for (auto id : item.mChildren)
                ids.push_back(id);
        }
        return writeDot(result, path);
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
