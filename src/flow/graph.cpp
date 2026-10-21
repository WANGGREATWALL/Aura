#include <unordered_map>

#include "flow/core.h"
#include "flow/graph_search.h"
#include "log/xlogger.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
int Core::builderError(Node* node, int code) noexcept
{
    if (owns(node) && node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft && !node->mBuildError)
        node->mBuildError = code;
    return code;  // The operation reports its own error; submission preserves the first build failure.
}

int Core::precede(Node* before, Node* after) noexcept
{
    if (!owns(before) || !owns(after))
        return builderError(after, au::err::kErrorInvalidHandle);
    if (after->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
        return au::err::kErrorStateInvalid;
    std::lock_guard<std::mutex> lock(mMutex);
    try {
        GraphTargets targets(after, false);
        GraphSearch  search(false);
        search.add(before);
        if (search.reachesTarget())
            return builderError(after, au::flow::kErrorCycleDetected);
        for (const auto& pred : after->mPreds)
            if (pred.get() == before)
                return 0;
        after->mPreds.emplace_back(before);
        return 0;
    } catch (...) {
        return builderError(after, au::err::kErrorNoMemory);
    }
}

int Core::add(Node* group, Node* member) noexcept
{
    if (!owns(group) || !owns(member))
        return builderError(group, au::err::kErrorInvalidHandle);
    if (group->mKind != Kind::Group)
        return au::err::kErrorInvalidParam;
    if (group->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
        return au::err::kErrorStateInvalid;
    if (member->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || member->mGroup)
        return builderError(group, au::err::kErrorStateInvalid);
    std::lock_guard<std::mutex> lock(mMutex);
    try {
        GraphTargets targets(group, false);
        GraphSearch  search(false);
        search.add(member);
        if (search.reachesTarget())
            return builderError(group, au::flow::kErrorCycleDetected);
        group->mMembers.emplace_back(member);
        member->mGroup = group;
        return 0;
    } catch (...) {
        return builderError(group, au::err::kErrorNoMemory);
    }
}

void Core::reject(Node* top, int error, int native) noexcept
{
    // No allocation, recursion or user destruction while editing state.
    Node* todo        = top;
    todo->mFinishNext = nullptr;
    while (todo) {
        Node* node = todo;
        todo       = node->mFinishNext;
        if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
            continue;
        for (const auto& member : node->mMembers) {
            member->mGroup      = nullptr;
            member->mFinishNext = todo;
            todo                = member.get();
        }
        if (node->mEntry.get()) {
            node->mEntry->mFinishNext = todo;
            todo                      = node->mEntry.get();
        }
        node->mResult             = taskResult(node->mId, error,
                                               error == au::err::kErrorNoMemory      ? au::flow::ErrorDomain::Memory
                                               : error == au::err::kErrorPlatformAPI ? au::flow::ErrorDomain::System
                                                                                     : au::flow::ErrorDomain::Flow);
        node->mResult.mState      = static_cast<std::uint32_t>(au::flow::TaskState::Rejected);
        node->mResult.mNativeCode = native;
        node->mSubmission.store(error, std::memory_order_release);  // Publish rejection diagnostics.
        node->mClosure.clear();
        {
            std::lock_guard<std::mutex> lock(node->mWaitMutex);
            node->mState.store(au::flow::TaskState::Rejected, std::memory_order_release);
        }
        node->mChanged.notify_all();
    }
}

int Core::submit(Node* top, bool reportFailure) noexcept
{
    int ret    = 0;
    int native = 0;
    if (gTileContext)
        ret = au::err::kErrorStateInvalid;
    else if (gCleanupDepth)
        ret = au::flow::kErrorWouldDeadlock;
    else if (!owns(top))
        ret = au::err::kErrorInvalidHandle;
    else if (top->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || top->mGroup)
        ret = au::err::kErrorStateInvalid;
    if (ret) {
        if (reportFailure)
            scopedError(ret);
        return ret;
    }
    WorkerAdmission           admission(this);
    std::vector<Prepared>     batch;
    std::vector<Registration> registrations;
    try {
        batch.push_back({top, gFrame ? gFrame->mNode : nullptr, nullptr, {}});
        for (std::size_t i = 0; i < batch.size(); ++i) {
            Node* node = batch[i].mNode;
            if (node->mBuildError) {
                ret = node->mBuildError;
                break;
            }
            if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft) {
                ret = au::err::kErrorStateInvalid;
                break;
            }
            if (node->mKind == Kind::Group) {
                if (node->mMembers.size() == SIZE_MAX) {
                    ret = au::err::kErrorOverflow;
                    break;
                }
                Node*    entry = node->mEntry.get();
                Prepared gate{entry, node, batch[i].mGate, {}};
                for (const auto& pred : node->mPreds)
                    gate.mPreds.push_back(pred.get());
                batch.push_back(std::move(gate));
                for (const auto& child : node->mMembers)
                    batch.push_back({child.get(), node, entry, {}});
            } else {
                for (const auto& pred : node->mPreds)
                    batch[i].mPreds.push_back(pred.get());
            }
        }
        std::unordered_map<Node*, std::size_t> indices;
        indices.reserve(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (!indices.emplace(batch[i].mNode, i).second)
                ret = au::err::kErrorAlreadyExists;
            if (batch[i].mGate && batch[i].mNode->mKind != Kind::Group)
                batch[i].mPreds.push_back(batch[i].mGate);
        }
        std::vector<std::size_t>              degree(batch.size(), 0);
        std::vector<std::vector<std::size_t>> successors(batch.size());
        for (std::size_t i = 0; i < batch.size() && !ret; ++i) {
            for (Node* before : batch[i].mPreds) {
                auto found = indices.find(before);
                if (found == indices.end()) {
                    if (before->mSubmission.load(std::memory_order_acquire) != 0)
                        ret = au::err::kErrorStateInvalid;
                } else {
                    ++degree[i];
                    successors[found->second].push_back(i);
                }
            }
            auto parent = indices.find(batch[i].mParent);
            if (parent != indices.end()) {
                ++degree[parent->second];
                successors[i].push_back(parent->second);
            }
        }
        std::vector<std::size_t> ready;
        ready.reserve(batch.size());
        for (std::size_t i = 0; i < degree.size(); ++i)
            if (!degree[i])
                ready.push_back(i);
        std::size_t visited = 0;
        while (visited < ready.size()) {
            for (std::size_t next : successors[ready[visited++]])
                if (!--degree[next])
                    ready.push_back(next);
        }
        if (visited != batch.size() && !ret)
            ret = au::flow::kErrorCycleDetected;
        if (!ret) {
            for (const Prepared& p : batch) {
                for (Node* before : p.mPreds) {
#ifdef AURA_FLOW_TESTING
                    if (fail(au::flow::testing::Failure::Edge))
                        throw std::bad_alloc();
#endif
                    std::unique_ptr<Edge, EdgeOwner> edge(mEdges.create(), EdgeOwner{this});
                    edge->mTarget = NodeRef(p.mNode);
                    registrations.push_back({before, std::move(edge)});
                }
            }
            ret = admission.accept(&native);
        }
        if (!ret) {
            std::lock_guard<std::mutex> lock(mMutex);
            // A child cannot depend on a scope that must wait for that child.
            // Existing accepted predecessor graphs and explicit wait edges
            // are immutable or protected by this mutex while inspected.
            if (batch[0].mParent) {
                GraphTargets targets(batch[0].mParent, true);
                GraphSearch  search(true);
                for (const Prepared& prepared : batch)
                    for (Node* pred : prepared.mPreds)
                        if (indices.find(pred) == indices.end())
                            search.add(pred);
                if (search.reachesTarget())
                    ret = au::flow::kErrorCycleDetected;
            }
            if (mPhase == Phase::Closed || (mPhase == Phase::Stopping && !batch[0].mParent))
                ret = au::err::kErrorStateInvalid;
            else if (mOrder > UINT64_MAX - batch.size() || (!batch[0].mParent && mRootOrder == UINT64_MAX) ||
                     mAccepted > SIZE_MAX - batch.size())
                ret = au::err::kErrorOverflow;
#ifdef AURA_FLOW_TESTING
            else if (fail(au::flow::testing::Failure::Order))
                ret = au::err::kErrorOverflow;
#endif
            if (!ret && batch[0].mParent && !reserveOutstanding(batch[0].mParent))
                ret = au::err::kErrorOverflow;
            if (!ret) {
                const std::uint64_t rootOrder = batch[0].mParent ? 0 : ++mRootOrder;
                for (const Prepared& p : batch) {
                    Node* node       = p.mNode;
                    node->mOrder     = ++mOrder;
                    node->mRootOrder = node == top ? rootOrder : 0;
                    node->mParent    = NodeRef(p.mParent);
                    linkChild(node);
                    node->mGate = NodeRef(p.mGate);
                    node->mUnresolved.store(p.mPreds.size(), std::memory_order_relaxed);  // Before commit release.
                    node->mOutstanding.store(node->mKind == Kind::Group ? node->mMembers.size() + 1 : 1,
                                             std::memory_order_relaxed);  // Children + entry, or body reservation.
                    retain(node);  // Accepted reference; released after final propagation.
                    node->mActiveNext = mActive;
                    if (mActive)
                        mActive->mActivePrev = node;
                    mActive = node;
                    node->mState.store(
                        node->mKind == Kind::Group ? au::flow::TaskState::Waiting : au::flow::TaskState::Pending,
                        std::memory_order_release);
                    node->mSubmission.store(0, std::memory_order_release);
                }
                if (mPhase == Phase::Dormant)
                    mPhase = Phase::Running;
                mAccepted += batch.size();
            }
        }
    } catch (const std::bad_alloc&) {
        ret = au::err::kErrorNoMemory;
    } catch (...) {
        ret = au::err::kErrorPlatformAPI;
    }
    if (ret) {
        reject(top, ret, native);
        if (reportFailure) {
            scopedError(ret);
            XLOG_E("au::flow submission rejected task=%llu code=%d\n", static_cast<unsigned long long>(top->mId), ret);
        }
        return ret;
    }

    for (Registration& r : registrations) {
        Edge* edge = r.mEdge.release();
        Edge* head = r.mBefore->mSuccessors.load(std::memory_order_acquire);  // Observe final close.
        for (;;) {
            if (head == closedEdges()) {
                resolve(edge->mTarget.get(), r.mBefore->mResult, r.mBefore->mOrder);
                mEdges.destroy(edge);
                break;
            }
            edge->mNext = head;
            if (r.mBefore->mSuccessors.compare_exchange_weak(head, edge, std::memory_order_acq_rel,
                                                             std::memory_order_acquire))
                break;  // Publish initialized edge, or see close.
        }
    }
    for (const Prepared& p : batch) {
        p.mNode->mCommitted.store(true, std::memory_order_release);  // All predecessor registrations exist.
        activate(p.mNode);
    }
    grow();
    return 0;
}

int Core::waitAll() noexcept
{
    if (gFrame || gTileContext || gCleanupDepth)
        return au::flow::kErrorWouldDeadlock;
    try {
        std::uint64_t watermark;
        std::size_t   count = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            watermark = mRootOrder;
            for (Node* n = mActive; n; n = n->mActiveNext)
                if (n->mRootOrder)
                    ++count;
        }
        std::vector<NodeRef> roots;
#ifdef AURA_FLOW_TESTING
        if (au::flow::testing::SnapshotHook* hook = gSnapshotHook.load(std::memory_order_acquire))
            hook->mInvoke(hook->mPayload);  // Outside the submission mutex.
#endif
        roots.reserve(count);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (Node* n = mActive; n; n = n->mActiveNext)
                if (n->mRootOrder && n->mRootOrder <= watermark)
                    roots.emplace_back(n);
        }
        for (const auto& root : roots) {
            const int ret = wait(root.get(), UINT64_MAX, nullptr);
            if (ret)
                return ret;
        }
        return 0;
    } catch (...) {
        return au::err::kErrorNoMemory;
    }
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
