#include "flow/xflow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "file/xfile.h"
#include "flow/ready_queue.h"
#include "flow/slab.h"
#include "flow/trace_buffer.h"
#include "flow/testing.h"
#include "flow/completion.h"
#include "log/xlogger.h"
#include "perf/xperf.h"
#include "sys/flow_runtime.h"

namespace au {
namespace flow {
namespace detail {

class Core;
struct Node;
void retain(au::flow::detail::Node*) noexcept;
void release(au::flow::detail::Node*) noexcept;

// Shared across accepted work, handles, dependency registration and parent
// scopes. Intrusive sharing avoids an allocation for each copied handle.
class NodeRef {
public:
    NodeRef() noexcept = default;
    explicit NodeRef(au::flow::detail::Node* value) noexcept : mValue(value) { au::flow::detail::retain(mValue); }
    NodeRef(const au::flow::detail::NodeRef& other) noexcept : NodeRef(other.mValue) {}
    NodeRef(au::flow::detail::NodeRef&& other) noexcept : mValue(other.mValue) { other.mValue = nullptr; }
    ~NodeRef() noexcept { au::flow::detail::release(mValue); }
    au::flow::detail::NodeRef& operator=(au::flow::detail::NodeRef other) noexcept
    { std::swap(mValue, other.mValue); return *this; }
    au::flow::detail::Node* get() const noexcept { return mValue; }
    au::flow::detail::Node* operator->() const noexcept { return mValue; }
private:
    au::flow::detail::Node* mValue = nullptr;
};

struct Closure {
    au_flow_invoke mInvoke = nullptr;
    au_flow_destroy mDestroy = nullptr;
    void* mPayload = nullptr;
    ~Closure() noexcept { clear(); }
    void clear() noexcept
    {
        void* payload = mPayload; mPayload = nullptr;
        if (payload && mDestroy) mDestroy(payload);
    }
};
struct Edge {
    au::flow::detail::NodeRef mTarget;
    au::flow::detail::Edge* mNext = nullptr;
};
au::flow::detail::Edge* closedEdges() noexcept
{
    static au::flow::detail::Edge closed;
    return &closed;
}
enum class Kind { Work, Group, Entry };
struct CreateFailure { int mCode; };

struct Node {
    Node(au::flow::detail::Core* owner, std::uint64_t id, au::flow::detail::Kind kind) noexcept
        : mOwner(owner), mId(id), mKind(kind)
    { mReady.mValue = this; }
    std::atomic<std::size_t> mRefs{1};
    au::flow::detail::Core* const mOwner;
    const std::uint64_t mId;
    const au::flow::detail::Kind mKind;
    std::atomic<au::flow::TaskState> mState{au::flow::TaskState::Draft};
    std::atomic<int> mSubmission{au::err::kErrorNotReady};
    au::flow::detail::Closure mClosure;
    bool (*mContinue)(void*) noexcept = nullptr; // Private tiled helper continuation, never an ordinary Task.
    std::string mName;
    std::vector<au::flow::detail::NodeRef> mPreds;
    std::vector<au::flow::detail::NodeRef> mMembers;
    au::flow::detail::NodeRef mEntry;
    au::flow::detail::NodeRef mGate;
    au::flow::detail::NodeRef mParent;
    au::flow::detail::Node* mGroup = nullptr; // Borrowed; group owns its draft members.
    int mBuildError = 0;
    std::atomic<au::flow::detail::Edge*> mSuccessors{nullptr};
    std::atomic<std::size_t> mUnresolved{0};
    std::atomic<bool> mCommitted{false};
    std::atomic<std::size_t> mOutstanding{1}; // Body reservation, plus accepted children/events.
    std::mutex mErrorMutex;
    int mBodyCode = 0;
    au::flow::ErrorDomain mBodyDomain = au::flow::ErrorDomain::Callable;
    au_flow_result mInherited{};
    std::uint64_t mInheritedOrder = UINT64_MAX;
    bool mInheritedSkipped = false;
    au_flow_result mScoped{};
    bool mSkipped = false;
    au_flow_result mResult{};
    std::mutex mWaitMutex;
    std::condition_variable mChanged;
    au::flow::detail::Node* mWaitTarget = nullptr; // Protected by Core::mMutex.
    au::flow::detail::Node* mExecutingChild = nullptr; // Cooperative stack edge, protected by Core::mMutex.
    au::flow::detail::ReadyLink mReady;
    au::flow::detail::Node* mActivePrev = nullptr;
    au::flow::detail::Node* mActiveNext = nullptr;
    au::flow::detail::Node* mFinishNext = nullptr;
    au::flow::detail::Node* mReclaimNext = nullptr;
    std::uint64_t mOrder = 0;
    std::uint64_t mRootOrder = 0;
};

struct Frame {
    au::flow::detail::Node* mNode;
    au::flow::detail::Frame* mPrevious;
};
struct ExternalToken {
    au::flow::detail::Core* mCore = nullptr;
    au::flow::detail::NodeRef mParent;
    au::flow::detail::ExternalOps mOps;
    void* mPayload = nullptr;
    std::uint64_t mOrder = 0;
    std::atomic<unsigned int> mNotification{0}; // 0 pending, 1 publishing, 2 terminal status published.
    std::atomic<unsigned int> mCallbacks{0};
    std::atomic<bool> mRegistrar{true};
    std::atomic<bool> mPolling{false};
    std::atomic<bool> mQueued{false};
    au::flow::detail::ExternalStatus mStatus;
    au::flow::detail::ReadyLink mLink;
    au::flow::detail::ExternalToken* mPendingNext = nullptr;
};
// Definitions live in this translation unit, never GNU-unique public inline TLS.
thread_local au::flow::detail::Frame* gFrame = nullptr;
thread_local int gWorker = -1;
thread_local unsigned int gDepth = 0;
thread_local unsigned int gTileDepth = 0;
thread_local au::flow::detail::Node* gFinish = nullptr;
thread_local bool gFinishing = false;
thread_local au::flow::detail::Node* gReclaim = nullptr;
thread_local bool gReclaiming = false;
thread_local unsigned int gCompletionBurst = 0;

#ifdef AURA_FLOW_TESTING
std::array<std::atomic<int>, 3> gFailure{{-1, -1, -1}};
std::atomic<au::flow::testing::SnapshotHook*> gSnapshotHook{nullptr};
bool fail(au::flow::testing::Failure failure) noexcept
{
    std::atomic<int>& counter = au::flow::detail::gFailure[static_cast<unsigned int>(failure)];
    int value = counter.load(std::memory_order_relaxed); // Fault schedule only.
    while (value >= 0) {
        if (counter.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) return value == 0;
    }
    return false;
}
#endif

bool finalState(au::flow::TaskState state) noexcept
{ return state >= au::flow::TaskState::Succeeded; }

class Core {
public:
    Core() noexcept
    {
        if (au::sys::flow::allowedCpus(mAllowed) != 0) {
            // A query failure must not turn a usable CPU scheduler into an error.
            mBudget.store(1, std::memory_order_relaxed); // Not shared during construction.
        } else {
            mBudget.store(std::min<std::size_t>(8, mAllowed.size()), std::memory_order_relaxed);
        }
    }
    ~Core() noexcept { (void)shutdown(); }
    au::flow::detail::Node* make(au::flow::detail::Kind kind)
    {
#ifdef AURA_FLOW_TESTING
        if (au::flow::detail::fail(au::flow::testing::Failure::Node)) throw std::bad_alloc();
#endif
        std::uint64_t id = mNextId.load(std::memory_order_relaxed); // Identity only.
        do {
            if (id == UINT64_MAX) throw au::flow::detail::CreateFailure{au::err::kErrorOverflow};
        } while (!mNextId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)); // Saturate; never wrap to a stale ID.
        au::flow::detail::Node* node = mNodes.create(this, id, kind);
        try { node->mName = std::to_string(id); } // Cold label creation, no formatting or copies during dispatch.
        catch (...) { mNodes.destroy(node); throw; }
        return node;
    }
    void recycle(au::flow::detail::Node* node) noexcept { mNodes.destroy(node); }
    bool owns(const au::flow::detail::Node* node) const noexcept { return node && node->mOwner == this; }
    void scopedError(int code, au::flow::ErrorDomain domain = au::flow::ErrorDomain::Flow) noexcept
    {
        if (!code || !au::flow::detail::gFrame) return;
        au::flow::detail::Node* node = au::flow::detail::gFrame->mNode;
        std::lock_guard<std::mutex> lock(node->mErrorMutex);
        if (!node->mScoped.mCode) node->mScoped = result(node, code, domain);
    }
    int prepareExternal(const au::flow::detail::ExternalOps& ops, void* payload,
                        au::flow::detail::ExternalToken** out) noexcept
    {
        if (!out || !ops.mRelease) return au::err::kErrorInvalidParam;
        *out = nullptr;
        if (!au::flow::detail::gFrame) return 0;
        au::flow::detail::ExternalToken* token = nullptr;
        try {
            token = mExternal.create();
            token->mCore = this;
            token->mParent = au::flow::detail::NodeRef(au::flow::detail::gFrame->mNode);
            token->mOps = ops; token->mPayload = payload; token->mLink.mValue = token;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mOrder == UINT64_MAX) { mExternal.destroy(token); return au::err::kErrorOverflow; }
                token->mOrder = ++mOrder;
            }
            token->mParent->mOutstanding.fetch_add(1, std::memory_order_relaxed); // Body reservation pins this scope.
            *out = token;
            return 0;
        } catch (...) {
            if (token) mExternal.destroy(token);
            scopedError(au::err::kErrorNoMemory, au::flow::ErrorDomain::Memory);
            return au::err::kErrorNoMemory;
        }
    }
    void queueExternal(au::flow::detail::ExternalToken* token) noexcept
    {
        if (token->mQueued.exchange(true, std::memory_order_acq_rel)) return; // Queue/pending list owns this record once.
        mCompletions.push(&token->mLink);
        mWork.post();
    }
    void notifyExternal(au::flow::detail::ExternalToken* token,
                         const au::flow::detail::ExternalStatus& status) noexcept
    {
        unsigned int pending = 0;
        if (!token->mNotification.compare_exchange_strong(pending, 1, std::memory_order_acq_rel)) return; // First notifier owns status.
        token->mStatus = status;
        token->mNotification.store(2, std::memory_order_release); // Worker can now read immutable terminal status.
        queueExternal(token);
    }
    bool consumeExternal(au::flow::detail::ExternalToken* token) noexcept
    {
        if (token->mRegistrar.load(std::memory_order_acquire)) return false; // Registration may still touch payload/event.
        if (token->mNotification.load(std::memory_order_acquire) != 2 &&
            token->mPolling.load(std::memory_order_acquire) && token->mOps.mPoll) {
            bool done = false;
            au::flow::detail::ExternalStatus status;
            const int query = token->mOps.mPoll(token->mPayload, &done, &status);
            if (query) {
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(token->mParent->mErrorMutex);
                    if (!token->mParent->mScoped.mCode) {
                        token->mParent->mScoped = result(token->mParent.get(), query, au::flow::ErrorDomain::OpenCL);
                        token->mParent->mScoped.mNativeCode = status.mNativeCode;
                        first = true;
                    }
                }
                if (first) XLOG_E("au::flow external query failed task=%llu code=%d native=%d\n",
                    static_cast<unsigned long long>(token->mParent->mId), query, status.mNativeCode);
            }
            if (done) notifyExternal(token, status);
        }
        if (token->mNotification.load(std::memory_order_acquire) != 2 ||
            token->mCallbacks.load(std::memory_order_acquire) != 0) return false;
        au::flow::detail::ExternalStatus status = token->mStatus;
        const au::flow::detail::ExternalStatus cleanup = token->mOps.mFinalize
            ? token->mOps.mFinalize(token->mPayload) : au::flow::detail::ExternalStatus{};
        if (status.mCode) XLOG_E("au::flow external completion failed task=%llu code=%d native=%d\n",
            static_cast<unsigned long long>(token->mParent->mId), status.mCode, status.mNativeCode);
        if (cleanup.mCode) XLOG_E("au::flow external cleanup failed task=%llu code=%d native=%d\n",
            static_cast<unsigned long long>(token->mParent->mId), cleanup.mCode, cleanup.mNativeCode);
        if (!status.mCode && cleanup.mCode) status = cleanup;
        token->mOps.mRelease(token->mPayload); // Never run release or driver calls under the pending-list lock.
        au::flow::detail::NodeRef parent = token->mParent;
        if (status.mCode) {
            au_flow_result failure = result(parent.get(), status.mCode, status.mDomain);
            failure.mNativeCode = status.mNativeCode;
            inherit(parent.get(), failure, token->mOrder);
        }
        mExternal.destroy(token);
        if (parent->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(parent.get()); // Release event reservation last.
        return true;
    }
    void deferExternal(au::flow::detail::ExternalToken* token) noexcept
    {
        std::lock_guard<std::mutex> lock(mExternalMutex);
        token->mPendingNext = nullptr;
        if (mPendingExternalTail) mPendingExternalTail->mPendingNext = token;
        else mPendingExternal = token;
        mPendingExternalTail = token;
        mPendingExternalCount.fetch_add(1, std::memory_order_release); // Enable timed retry only while deferred work exists.
    }
    bool pollPendingExternal() noexcept
    {
        au::flow::detail::ExternalToken* token;
        {
            std::lock_guard<std::mutex> lock(mExternalMutex);
            token = mPendingExternal;
            if (!token) return false;
            mPendingExternal = token->mPendingNext;
            if (!mPendingExternal) mPendingExternalTail = nullptr;
            mPendingExternalCount.fetch_sub(1, std::memory_order_release);
        }
        if (!consumeExternal(token)) { deferExternal(token); return false; }
        return true;
    }
    static au_flow_result result(au::flow::detail::Node* node, int code,
                                  au::flow::ErrorDomain domain) noexcept
    {
        return {sizeof(au_flow_result), static_cast<std::uint32_t>(code ? au::flow::TaskState::Failed
            : au::flow::TaskState::Succeeded), static_cast<std::uint32_t>(code ? domain
            : au::flow::ErrorDomain::None), code, 0, code ? node->mId : 0};
    }

    // Follow strong draft references before inserting one. This rejects reference
    // cycles even for builders that are discarded without ever being submitted.
    bool reaches(au::flow::detail::Node* start, au::flow::detail::Node* target,
                 bool completion) const
    {
        std::vector<au::flow::detail::Node*> todo{start};
        std::unordered_set<au::flow::detail::Node*> seen;
        while (!todo.empty()) {
            au::flow::detail::Node* node = todo.back(); todo.pop_back();
            if (node == target) return true;
            if (!seen.insert(node).second) continue;
            for (const auto& pred : node->mPreds) todo.push_back(pred.get());
            for (const auto& member : node->mMembers) todo.push_back(member.get());
            if (node->mEntry.get()) todo.push_back(node->mEntry.get());
            if (node->mGate.get()) todo.push_back(node->mGate.get());
            if (completion) { // Callers hold mMutex while inspecting runtime completion edges.
                if (node->mWaitTarget) todo.push_back(node->mWaitTarget);
                if (node->mExecutingChild) todo.push_back(node->mExecutingChild);
                for (au::flow::detail::Node* child = mActive; child; child = child->mActiveNext)
                    if (child->mParent.get() == node) todo.push_back(child);
            }
        }
        return false;
    }
    int precede(au::flow::detail::Node* before, au::flow::detail::Node* after) noexcept
    {
        if (!owns(before) || !owns(after)) {
            if (owns(after) && after->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft)
                after->mBuildError = au::err::kErrorInvalidHandle;
            return au::err::kErrorInvalidHandle;
        }
        if (after->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
            return au::err::kErrorStateInvalid;
        try {
            if (reaches(before, after, false)) return after->mBuildError = au::flow::kErrorCycleDetected;
            for (const auto& pred : after->mPreds) if (pred.get() == before) return 0;
            after->mPreds.emplace_back(before);
            return 0;
        } catch (...) { return after->mBuildError = au::err::kErrorNoMemory; }
    }
    int add(au::flow::detail::Node* group, au::flow::detail::Node* member) noexcept
    {
        if (!owns(group) || !owns(member)) {
            if (owns(group)) group->mBuildError = au::err::kErrorInvalidHandle;
            return au::err::kErrorInvalidHandle;
        }
        if (group->mKind != au::flow::detail::Kind::Group) return au::err::kErrorInvalidParam;
        if (group->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
            return au::err::kErrorStateInvalid;
        if (member->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || member->mGroup)
            return group->mBuildError = au::err::kErrorStateInvalid;
        try {
            if (reaches(member, group, false)) return group->mBuildError = au::flow::kErrorCycleDetected;
            group->mMembers.emplace_back(member); member->mGroup = group;
            return 0;
        } catch (...) { return group->mBuildError = au::err::kErrorNoMemory; }
    }
    void reject(au::flow::detail::Node* top, int error, int native = 0) noexcept
    {
        // No allocation, recursion or user destruction while editing state.
        au::flow::detail::Node* todo = top;
        todo->mFinishNext = nullptr;
        while (todo) {
            au::flow::detail::Node* node = todo; todo = node->mFinishNext;
            if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft) continue;
            for (const auto& member : node->mMembers) {
                member->mGroup = nullptr; member->mFinishNext = todo; todo = member.get();
            }
            if (node->mEntry.get()) { node->mEntry->mFinishNext = todo; todo = node->mEntry.get(); }
            node->mResult = result(node, error, error == au::err::kErrorNoMemory ? au::flow::ErrorDomain::Memory
                : error == au::err::kErrorPlatformAPI ? au::flow::ErrorDomain::System : au::flow::ErrorDomain::Flow);
            node->mResult.mState = static_cast<std::uint32_t>(au::flow::TaskState::Rejected);
            node->mResult.mNativeCode = native;
            node->mSubmission.store(error, std::memory_order_release); // Publish rejection diagnostics.
            node->mClosure.clear();
            {
                std::lock_guard<std::mutex> lock(node->mWaitMutex);
                node->mState.store(au::flow::TaskState::Rejected, std::memory_order_release);
            }
            node->mChanged.notify_all();
        }
    }

    struct Prepared {
        au::flow::detail::Node* mNode;
        au::flow::detail::Node* mParent;
        au::flow::detail::Node* mGate;
        std::vector<au::flow::detail::Node*> mPreds;
    };
    struct EdgeOwner {
        au::flow::detail::Core* mCore;
        void operator()(au::flow::detail::Edge* edge) const noexcept { if (edge) mCore->mEdges.destroy(edge); }
    };
    struct Registration {
        au::flow::detail::Node* mBefore;
        std::unique_ptr<au::flow::detail::Edge, au::flow::detail::Core::EdgeOwner> mEdge;
    };

    int submit(au::flow::detail::Node* top) noexcept
    {
        int ret = 0;
        int native = 0;
        if (!owns(top)) ret = au::err::kErrorInvalidHandle;
        else if (top->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || top->mGroup)
            ret = au::err::kErrorStateInvalid;
        if (ret) { scopedError(ret); return ret; }
        std::vector<Prepared> batch;
        std::vector<Registration> registrations;
        try {
            batch.push_back({top, au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode : nullptr, nullptr, {}});
            for (std::size_t i = 0; i < batch.size(); ++i) {
                au::flow::detail::Node* node = batch[i].mNode;
                if (node->mBuildError) { ret = node->mBuildError; break; }
                if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft) {
                    ret = au::err::kErrorStateInvalid; break;
                }
                if (node->mKind == au::flow::detail::Kind::Group) {
                    au::flow::detail::Node* entry = node->mEntry.get();
                    Prepared gate{entry, node, batch[i].mGate, {}};
                    for (const auto& pred : node->mPreds) gate.mPreds.push_back(pred.get());
                    batch.push_back(std::move(gate));
                    for (const auto& child : node->mMembers) batch.push_back({child.get(), node, entry, {}});
                } else {
                    for (const auto& pred : node->mPreds) batch[i].mPreds.push_back(pred.get());
                }
            }
            std::unordered_map<au::flow::detail::Node*, std::size_t> indices;
            indices.reserve(batch.size());
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (!indices.emplace(batch[i].mNode, i).second) ret = au::err::kErrorAlreadyExists;
                if (batch[i].mGate && batch[i].mNode->mKind != au::flow::detail::Kind::Group)
                    batch[i].mPreds.push_back(batch[i].mGate);
            }
            std::vector<std::size_t> degree(batch.size(), 0);
            std::vector<std::vector<std::size_t>> successors(batch.size());
            for (std::size_t i = 0; i < batch.size() && !ret; ++i) {
                for (au::flow::detail::Node* before : batch[i].mPreds) {
                    auto found = indices.find(before);
                    if (found == indices.end()) {
                        if (before->mSubmission.load(std::memory_order_acquire) != 0)
                            ret = au::err::kErrorStateInvalid;
                    } else { ++degree[i]; successors[found->second].push_back(i); }
                }
                auto parent = indices.find(batch[i].mParent);
                if (parent != indices.end()) { ++degree[parent->second]; successors[i].push_back(parent->second); }
            }
            std::vector<std::size_t> ready;
            ready.reserve(batch.size());
            for (std::size_t i = 0; i < degree.size(); ++i) if (!degree[i]) ready.push_back(i);
            std::size_t visited = 0;
            while (visited < ready.size()) {
                for (std::size_t next : successors[ready[visited++]]) if (!--degree[next]) ready.push_back(next);
            }
            if (visited != batch.size() && !ret) ret = au::flow::kErrorCycleDetected;
            if (!ret) {
                for (const Prepared& p : batch) {
                    for (au::flow::detail::Node* before : p.mPreds) {
#ifdef AURA_FLOW_TESTING
                        if (au::flow::detail::fail(au::flow::testing::Failure::Edge)) throw std::bad_alloc();
#endif
                        std::unique_ptr<au::flow::detail::Edge, EdgeOwner> edge(mEdges.create(), EdgeOwner{this});
                        edge->mTarget = au::flow::detail::NodeRef(p.mNode);
                        registrations.push_back({before, std::move(edge)});
                    }
                }
                ret = ensureWorker(&native);
            }
            if (!ret) {
                std::lock_guard<std::mutex> lock(mMutex);
                // A child cannot depend on a scope that must wait for that child.
                // Existing accepted predecessor graphs and explicit wait edges
                // are immutable or protected by this mutex while inspected.
                for (const Prepared& p : batch) {
                    for (au::flow::detail::Node* ancestor = batch[0].mParent; ancestor; ancestor = ancestor->mParent.get()) {
                        for (au::flow::detail::Node* pred : p.mPreds) {
                            if (indices.find(pred) == indices.end() && reaches(pred, ancestor, true))
                                ret = au::flow::kErrorCycleDetected;
                        }
                    }
                }
                if (mPhase == Phase::Closed || (mPhase == Phase::Stopping && !batch[0].mParent))
                    ret = au::err::kErrorStateInvalid;
                else if (mOrder > UINT64_MAX - batch.size() || (!batch[0].mParent && mRootOrder == UINT64_MAX))
                    ret = au::err::kErrorOverflow;
                else if (!ret) {
                    const std::uint64_t rootOrder = batch[0].mParent ? 0 : ++mRootOrder;
                    for (const Prepared& p : batch) {
                        au::flow::detail::Node* node = p.mNode;
                        node->mOrder = ++mOrder;
                        node->mRootOrder = node == top ? rootOrder : 0;
                        node->mParent = au::flow::detail::NodeRef(p.mParent);
                        node->mGate = au::flow::detail::NodeRef(p.mGate);
                        node->mUnresolved.store(p.mPreds.size(), std::memory_order_relaxed); // Before commit release.
                        node->mOutstanding.store(node->mKind == Kind::Group ? node->mMembers.size() + 1 : 1,
                                                 std::memory_order_relaxed); // Children + entry, or body reservation.
                        if (node == top && p.mParent)
                            p.mParent->mOutstanding.fetch_add(1, std::memory_order_relaxed); // Parent body pins the scope.
                        au::flow::detail::retain(node); // Accepted reference; released after final propagation.
                        node->mActiveNext = mActive;
                        if (mActive) mActive->mActivePrev = node;
                        mActive = node;
                        node->mState.store(node->mKind == Kind::Group ? au::flow::TaskState::Waiting
                            : au::flow::TaskState::Pending, std::memory_order_release);
                        node->mSubmission.store(0, std::memory_order_release);
                    }
                    if (mPhase == Phase::Dormant) mPhase = Phase::Running;
                    mAccepted += batch.size();
                }
            }
        } catch (const std::bad_alloc&) { ret = au::err::kErrorNoMemory; }
        catch (...) { ret = au::err::kErrorPlatformAPI; }
        if (ret) {
            reject(top, ret, native); scopedError(ret);
            XLOG_E("au::flow submission rejected task=%llu code=%d\n", static_cast<unsigned long long>(top->mId), ret);
            return ret;
        }

        for (const Prepared& p : batch) trace(p.mNode, "Task", 'b');
        for (Registration& r : registrations) {
            au::flow::detail::Edge* edge = r.mEdge.release();
            au::flow::detail::Edge* head = r.mBefore->mSuccessors.load(std::memory_order_acquire); // Observe final close.
            for (;;) {
                if (head == au::flow::detail::closedEdges()) {
                    resolve(edge->mTarget.get(), r.mBefore->mResult, r.mBefore->mOrder);
                    mEdges.destroy(edge); break;
                }
                edge->mNext = head;
                if (r.mBefore->mSuccessors.compare_exchange_weak(head, edge,
                        std::memory_order_acq_rel, std::memory_order_acquire)) break; // Publish initialized edge, or see close.
            }
        }
        for (const Prepared& p : batch) {
            p.mNode->mCommitted.store(true, std::memory_order_release); // All predecessor registrations exist.
            activate(p.mNode);
        }
        grow();
        return 0;
    }

    void inherit(au::flow::detail::Node* target, const au_flow_result& source, std::uint64_t order) noexcept
    {
        if (!source.mCode) return;
        const bool skipped = source.mState == static_cast<std::uint32_t>(au::flow::TaskState::Skipped);
        std::lock_guard<std::mutex> lock(target->mErrorMutex);
        if (!target->mInherited.mCode || (target->mInheritedSkipped && !skipped) ||
            (target->mInheritedSkipped == skipped && order < target->mInheritedOrder)) {
            target->mInherited = source; target->mInheritedOrder = order; target->mInheritedSkipped = skipped;
        }
    }
    void resolve(au::flow::detail::Node* target, const au_flow_result& source, std::uint64_t order) noexcept
    {
        inherit(target, source, order);
        if (target->mUnresolved.fetch_sub(1, std::memory_order_acq_rel) == 1) activate(target); // Gather predecessor writes.
    }
    void activate(au::flow::detail::Node* node) noexcept
    {
        if (node->mKind == Kind::Group || !node->mCommitted.load(std::memory_order_acquire) ||
            node->mUnresolved.load(std::memory_order_acquire) != 0) return;
        au::flow::TaskState expected = au::flow::TaskState::Pending;
        if (!node->mState.compare_exchange_strong(expected, au::flow::TaskState::Ready,
                std::memory_order_acq_rel)) return; // Exactly one ready publisher.
        if (node->mInherited.mCode) {
            node->mSkipped = true;
            bodyEnd(node, 0);
            return;
        }
        mReadyCount.fetch_add(1, std::memory_order_relaxed); // Scaling hint only.
        trace(node, "ready", 'i');
        mReady.push(&node->mReady);
        mWork.post();
    }
    void bodyEnd(au::flow::detail::Node* node, int code) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(node->mErrorMutex);
            node->mBodyCode = code;
            if (code != au::flow::kErrorUnhandledException) node->mBodyDomain = au::flow::ErrorDomain::Callable;
        }
        node->mState.store(au::flow::TaskState::Waiting, std::memory_order_release); // Body cannot run again.
        if (node->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(node); // Gather child/event writes.
    }
    void finish(au::flow::detail::Node* node) noexcept
    {
        node->mFinishNext = au::flow::detail::gFinish; au::flow::detail::gFinish = node;
        if (au::flow::detail::gFinishing) return;
        au::flow::detail::gFinishing = true;
        while (au::flow::detail::gFinish) {
            node = au::flow::detail::gFinish; au::flow::detail::gFinish = node->mFinishNext;
            node->mClosure.clear(); // User destructors run without scheduler/error locks.
            {
                std::lock_guard<std::mutex> lock(node->mErrorMutex);
                node->mResult = node->mBodyCode ? result(node, node->mBodyCode, node->mBodyDomain)
                    : node->mScoped.mCode ? node->mScoped : node->mInherited.mCode ? node->mInherited
                    : result(node, 0, au::flow::ErrorDomain::None);
                node->mResult.mState = static_cast<std::uint32_t>(node->mSkipped ? au::flow::TaskState::Skipped
                    : node->mResult.mCode ? au::flow::TaskState::Failed : au::flow::TaskState::Succeeded);
            }
            trace(node, "Task", 'e', node->mResult.mCode);
            {
                std::lock_guard<std::mutex> lock(node->mWaitMutex);
                node->mState.store(static_cast<au::flow::TaskState>(node->mResult.mState), std::memory_order_release);
            }
            node->mChanged.notify_all();
            au::flow::detail::Edge* edge = node->mSuccessors.exchange(au::flow::detail::closedEdges(),
                std::memory_order_acq_rel); // Release final result; acquire complete edge list.
            while (edge) {
                au::flow::detail::Edge* next = edge->mNext;
                resolve(edge->mTarget.get(), node->mResult, node->mOrder); mEdges.destroy(edge); edge = next;
            }
            au::flow::detail::NodeRef parent;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                parent = std::move(node->mParent);
                if (node->mActivePrev) node->mActivePrev->mActiveNext = node->mActiveNext;
                else mActive = node->mActiveNext;
                if (node->mActiveNext) node->mActiveNext->mActivePrev = node->mActivePrev;
                --mAccepted;
            }
            if (parent.get()) {
                inherit(parent.get(), node->mResult, node->mOrder);
                if (node->mKind == Kind::Entry && node->mSkipped) {
                    std::lock_guard<std::mutex> lock(parent->mErrorMutex);
                    parent->mSkipped = true;
                }
                if (parent->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(parent.get());
            }
            mDrained.notify_all();
            au::flow::detail::release(node);
        }
        au::flow::detail::gFinishing = false;
    }

    bool helpOne(bool blocking, std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (au::flow::detail::gDepth >= 32) return false;
        if (mPendingExternalCount.load(std::memory_order_acquire) != 0) {
            if ((au::flow::detail::gCompletionBurst < 8 || mReadyCount.load(std::memory_order_relaxed) == 0) && pollPendingExternal()) {
                ++au::flow::detail::gCompletionBurst; return true;
            }
            deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
        }
        if (!(blocking ? mWork.acquireUntil(deadline) : mWork.tryAcquire())) return false;
        if (au::flow::detail::gWorker >= 0) applyPolicy(static_cast<std::size_t>(au::flow::detail::gWorker));
        if (au::flow::detail::gWorker >= 0 &&
            static_cast<std::size_t>(au::flow::detail::gWorker) >= mBudget.load(std::memory_order_acquire)) {
            mWork.post(); // Return a real permit when this retained worker is outside the new budget.
            return false;
        }
        au::flow::detail::ReadyLink* link = nullptr;
        bool overflowFirst = false;
        bool completion = false;
        for (;;) {
            if (au::flow::detail::gCompletionBurst < 8 && mCompletions.pop(link, overflowFirst)) { completion = true; break; }
            if (mReady.pop(link, overflowFirst)) break;
            if (mCompletions.pop(link, overflowFirst)) { completion = true; break; }
            if (mStop.load(std::memory_order_acquire)) return false; // Shutdown control permits have no queue item.
            overflowFirst = !overflowFirst;
            std::this_thread::yield(); // Keep the acquired permit across transient publication gaps.
        }
        if (completion) {
            auto* token = static_cast<au::flow::detail::ExternalToken*>(link->mValue);
            if (!consumeExternal(token)) deferExternal(token);
            ++au::flow::detail::gCompletionBurst;
            return true;
        }
        au::flow::detail::gCompletionBurst = 0;
        mReadyCount.fetch_sub(1, std::memory_order_relaxed); // Scaling hint only.
        run(static_cast<au::flow::detail::Node*>(link->mValue));
        grow(); // Gates can expose new parallelism without a further external enqueue.
        return true;
    }
    void run(au::flow::detail::Node* node) noexcept
    {
        au::flow::detail::NodeRef pin(node);
        node->mState.store(au::flow::TaskState::Running, std::memory_order_release);
        Frame frame{node, au::flow::detail::gFrame};
        if (frame.mPrevious) {
            std::lock_guard<std::mutex> lock(mMutex);
            frame.mPrevious->mNode->mExecutingChild = node;
        }
        au::flow::detail::gFrame = &frame;
        const unsigned int tileDepth = au::flow::detail::gTileDepth;
        au::flow::detail::gTileDepth = 0; ++au::flow::detail::gDepth;
        mBusy.fetch_add(1, std::memory_order_relaxed); // May count cooperative stacks, bounded by worker count when scaling.
        int code = 0;
        trace(node, "CPU", 'B');
        if (node->mClosure.mInvoke) {
            try {
                if (mTracing.load(std::memory_order_relaxed)) {
                    au::perf::XPerfScoped scope(node->mName);
                    code = node->mClosure.mInvoke(node->mClosure.mPayload);
                } else code = node->mClosure.mInvoke(node->mClosure.mPayload);
            } catch (...) {
                code = au::flow::kErrorUnhandledException;
                std::lock_guard<std::mutex> lock(node->mErrorMutex);
                node->mBodyDomain = au::flow::ErrorDomain::Flow;
            }
        }
        trace(node, "CPU", 'E', code);
        mBusy.fetch_sub(1, std::memory_order_relaxed);
        --au::flow::detail::gDepth; au::flow::detail::gTileDepth = tileDepth;
        au::flow::detail::gFrame = frame.mPrevious;
        if (node->mContinue && node->mContinue(node->mClosure.mPayload)) {
            node->mState.store(au::flow::TaskState::Ready, std::memory_order_release);
            mReadyCount.fetch_add(1, std::memory_order_relaxed); // Helper yields after one tile.
            mReady.push(&node->mReady); mWork.post();
            if (frame.mPrevious) {
                std::lock_guard<std::mutex> lock(mMutex);
                frame.mPrevious->mNode->mExecutingChild = nullptr;
            }
            return;
        }
        bodyEnd(node, code);
        if (frame.mPrevious) {
            std::lock_guard<std::mutex> lock(mMutex);
            frame.mPrevious->mNode->mExecutingChild = nullptr;
        }
    }

    int wait(au::flow::detail::Node* node, std::uint64_t timeout, au_flow_result* out) noexcept
    {
        if (!owns(node)) return au::err::kErrorInvalidHandle;
        au::flow::detail::NodeRef pin(node);
        if (node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft) return au::err::kErrorNotReady;
        if (out && out->mStructSize != sizeof(au_flow_result)) return au::err::kErrorVersionMismatch;
        const auto now = std::chrono::steady_clock::now();
        const auto maxMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::time_point::max() - now).count();
        const auto deadline = timeout >= static_cast<std::uint64_t>(maxMs)
            ? std::chrono::steady_clock::time_point::max() : now + std::chrono::milliseconds(timeout);
        au::flow::detail::Node* current = au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode : nullptr;
        if (current && !au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) {
            try {
                std::lock_guard<std::mutex> lock(mMutex);
                bool deadlock = au::flow::detail::gDepth >= 32 || reaches(node, current, true);
                for (au::flow::detail::Node* p = current; p; p = p->mParent.get()) if (p == node) deadlock = true;
                for (au::flow::detail::Frame* frame = au::flow::detail::gFrame; frame; frame = frame->mPrevious)
                    if (reaches(node, frame->mNode, true)) deadlock = true;
                if (deadlock) return au::flow::kErrorWouldDeadlock;
                current->mWaitTarget = node;
            } catch (...) { return au::err::kErrorNoMemory; }
        }
        int ret = 0;
        while (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) {
            if (std::chrono::steady_clock::now() >= deadline) { ret = au::err::kErrorTimeout; break; }
            if (current && helpOne(false, deadline)) continue;
            std::unique_lock<std::mutex> lock(node->mWaitMutex);
            if (current) {
                node->mChanged.wait_until(lock, std::min(deadline,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
            } else if (deadline == std::chrono::steady_clock::time_point::max()) {
                node->mChanged.wait(lock, [node] { return au::flow::detail::finalState(node->mState.load(std::memory_order_acquire)); });
            } else {
                node->mChanged.wait_until(lock, deadline, [node] { return au::flow::detail::finalState(node->mState.load(std::memory_order_acquire)); });
            }
        }
        if (current) { std::lock_guard<std::mutex> lock(mMutex); current->mWaitTarget = nullptr; }
        if (!ret && out) *out = node->mResult;
        return ret;
    }
    int waitAll() noexcept
    {
        if (au::flow::detail::gFrame) return au::flow::kErrorWouldDeadlock;
        try {
            std::uint64_t watermark;
            std::size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                watermark = mRootOrder;
                for (au::flow::detail::Node* n = mActive; n; n = n->mActiveNext) if (n->mRootOrder) ++count;
            }
            std::vector<au::flow::detail::NodeRef> roots;
#ifdef AURA_FLOW_TESTING
            if (au::flow::testing::SnapshotHook* hook = au::flow::detail::gSnapshotHook.load(std::memory_order_acquire))
                hook->mInvoke(hook->mPayload); // Outside the submission mutex.
#endif
            roots.reserve(count);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (au::flow::detail::Node* n = mActive; n; n = n->mActiveNext)
                    if (n->mRootOrder && n->mRootOrder <= watermark) roots.emplace_back(n);
            }
            for (const auto& root : roots) { const int ret = wait(root.get(), UINT64_MAX, nullptr); if (ret) return ret; }
            return 0;
        } catch (...) { return au::err::kErrorNoMemory; }
    }

    int shutdown() noexcept
    {
        if (au::flow::detail::gFrame) return au::flow::kErrorWouldDeadlock;
        std::lock_guard<std::mutex> closing(mCloseMutex);
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mPhase == Phase::Closed) return 0;
            mPhase = Phase::Stopping;
            mDrained.wait(lock, [this] { return mAccepted == 0; });
        }
        {
            std::unique_lock<std::mutex> lock(mWorkerMutex);
            mClosingQueries = true;
            mPolicyChanged.wait(lock, [this] { return mAffinityQueries == 0; });
            mStop.store(true, std::memory_order_release); // Workers may now consume control permits and leave.
        }
        mWork.post(8);
        for (auto& worker : mWorkers) if (worker.mThread.joinable()) worker.mThread.join();
        { std::lock_guard<std::mutex> lock(mMutex); mPhase = Phase::Closed; }
        return 0;
    }

    int ensureWorker(int* native = nullptr) noexcept
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        if (mStop.load(std::memory_order_acquire)) return au::err::kErrorStateInvalid;
        if (mWorkerCount) return 0;
        const int code = startWorker();
        if (native) *native = mLastWorkerNative;
        return code;
    }
    void grow() noexcept
    {
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        if (mStop.load(std::memory_order_acquire)) return;
        const std::size_t demand = mReadyCount.load(std::memory_order_relaxed) + mBusy.load(std::memory_order_relaxed);
        while (mWorkerCount < std::min(mBudget.load(std::memory_order_relaxed), demand)) {
            if (startWorker() != 0) break;
        }
    }
    int startWorker() noexcept
    {
        mLastWorkerNative = 0;
#ifdef AURA_FLOW_TESTING
        if (au::flow::detail::fail(au::flow::testing::Failure::Worker)) return au::err::kErrorPlatformAPI;
#endif
        if (mWorkerCount == mWorkers.size()) return 0;
        const std::size_t id = mWorkerCount;
        try {
            mWorkers[id].mThread = std::thread([this, id] { workerLoop(id); });
            ++mWorkerCount;
            return 0;
        } catch (const std::system_error& error) {
            mLastWorkerNative = error.code().value();
            return au::err::kErrorPlatformAPI;
        } catch (const std::bad_alloc&) { return au::err::kErrorNoMemory; }
        catch (...) { return au::err::kErrorPlatformAPI; }
    }
    void workerLoop(std::size_t id) noexcept
    {
        au::flow::detail::gWorker = static_cast<int>(id);
        const std::uint64_t threadId = au::sys::flow::threadId();
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            mWorkers[id].mThreadId = threadId;
        }
        while (!mStop.load(std::memory_order_acquire)) {
            applyPolicy(id);
            if (id >= mBudget.load(std::memory_order_acquire)) {
                // Park retained surplus threads after a budget reduction.
                std::unique_lock<std::mutex> lock(mWorkerMutex);
                mPolicyChanged.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
            (void)helpOne(true, std::chrono::steady_clock::time_point::max());
        }
        au::flow::detail::gWorker = -1;
    }
    void applyPolicy(std::size_t id) noexcept
    {
        std::vector<std::uint32_t> cpus;
        std::uint64_t version;
        try {
            {
                std::lock_guard<std::mutex> lock(mWorkerMutex);
                version = mPolicyVersion;
                if (mWorkers[id].mAppliedVersion == version) return;
                cpus = mPolicy.empty() ? mAllowed : mPolicy;
            }
            const int code = version == 0 || cpus.empty() ? 0 : au::sys::flow::applyAffinity(cpus);
            std::vector<std::uint32_t> effective;
            const int query = au::sys::flow::currentAffinity(effective);
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            mWorkers[id].mAppliedVersion = version;
            mWorkers[id].mAffinityCode = code ? code : query;
            mWorkers[id].mEffective = std::move(effective);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            mWorkers[id].mAffinityCode = au::err::kErrorNoMemory;
        }
    }
    int affinity(std::uint32_t mode, const std::uint32_t* cpus, std::size_t count) noexcept
    {
        if (mode > 1 || (mode && (!cpus || !count))) return au::err::kErrorInvalidParam;
        if (mode && au::sys::flow::affinitySupported() != 0) return au::err::kErrorNotSupported;
        try {
            std::vector<std::uint32_t> selected;
            if (mode) for (std::size_t i = 0; i < count; ++i)
                if (std::find(mAllowed.begin(), mAllowed.end(), cpus[i]) != mAllowed.end()) selected.push_back(cpus[i]);
            std::sort(selected.begin(), selected.end());
            selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
            if (mode && selected.empty()) return au::err::kErrorInvalidParam;
            {
                std::lock_guard<std::mutex> lock(mWorkerMutex);
                if (mClosingQueries) return au::err::kErrorStateInvalid;
                mPolicy = std::move(selected); ++mPolicyVersion;
                mBudget.store(std::max<std::size_t>(1, std::min<std::size_t>(8,
                    mode ? mPolicy.size() : mAllowed.size())), std::memory_order_release); // New dispatch budget.
            }
            mPolicyChanged.notify_all();
            grow();
            return 0;
        } catch (...) { return au::err::kErrorNoMemory; }
    }
    int workers(au_flow_worker* out, std::size_t capacity, std::size_t* count) noexcept
    {
        if (!count || (capacity && !out)) return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        *count = mWorkerCount;
        for (std::size_t i = 0; i < std::min(capacity, mWorkerCount); ++i) {
            if (out[i].mStructSize != sizeof(au_flow_worker)) return au::err::kErrorVersionMismatch;
            out[i] = {sizeof(au_flow_worker), static_cast<std::uint32_t>(i), mWorkers[i].mThreadId,
                      mPolicyVersion, mWorkers[i].mAppliedVersion, mWorkers[i].mAffinityCode};
        }
        return capacity < mWorkerCount ? au::err::kErrorBufferUnderflow : 0;
    }
    int workerAffinity(std::uint32_t id, std::uint32_t* cpus, std::size_t capacity, std::size_t* count) noexcept
    {
        if (!count || (capacity && !cpus)) return au::err::kErrorInvalidParam;
        std::uint64_t thread;
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            if (id >= mWorkerCount) return au::err::kErrorOutOfRange;
            if (mClosingQueries) return au::err::kErrorStateInvalid;
            thread = mWorkers[id].mThreadId;
            if (!thread) return au::err::kErrorNotReady;
            ++mAffinityQueries; // Prevent exit/TID reuse during the native query.
        }
        std::vector<std::uint32_t> effective;
        int code = au::sys::flow::affinitySupported();
        if (!code) code = au::sys::flow::threadAffinity(thread, effective);
        { std::lock_guard<std::mutex> lock(mWorkerMutex); --mAffinityQueries; }
        mPolicyChanged.notify_all();
        if (code) return code;
        *count = effective.size();
        if (cpus) std::copy_n(effective.begin(), std::min(capacity, *count), cpus);
        return capacity < *count ? au::err::kErrorBufferUnderflow : 0;
    }
    int dot(au::flow::detail::Node* top, const char* path) noexcept
    {
        if (!owns(top)) return au::err::kErrorInvalidHandle;
        if (!path) return au::err::kErrorNullPointer;
        try {
            std::ostringstream text;
            text << "digraph AuraFlow {\n";
            std::vector<au::flow::detail::Node*> todo{top};
            std::unordered_set<au::flow::detail::Node*> seen;
            while (!todo.empty()) {
                au::flow::detail::Node* n = todo.back(); todo.pop_back();
                if (!seen.insert(n).second) continue;
                text << "  n" << n->mId << " [label=\"";
                for (char c : n->mName) {
                    if (c == '"' || c == '\\') text << '\\';
                    if (c == '\n' || c == '\r') text << "\\n"; else text << c;
                }
                text << " #" << n->mId << "\"];\n";
                for (const auto& p : n->mPreds) { text << "  n" << p->mId << " -> n" << n->mId << ";\n"; todo.push_back(p.get()); }
                for (const auto& c : n->mMembers) { text << "  n" << n->mId << " -> n" << c->mId << " [style=dashed];\n"; todo.push_back(c.get()); }
            }
            text << "}\n";
            return au::file::write(text.str(), path);
        } catch (...) { return au::err::kErrorNoMemory; }
    }
    std::atomic<bool> mTracing{false};
    struct TraceStorage { std::array<au::flow::detail::TraceBuffer, 9> mBuffers; };
    int tracing(bool enabled) noexcept
    {
        if (enabled && !mTraceStorage.load(std::memory_order_acquire)) {
            try {
                std::unique_ptr<TraceStorage> prepared(new TraceStorage());
                std::lock_guard<std::mutex> lock(mTraceMutex);
                if (!mTraceOwner) {
                    mTraceOwner = std::move(prepared);
                    mTraceStorage.store(mTraceOwner.get(), std::memory_order_release); // Stable until joined shutdown/destruction.
                }
            } catch (...) { return au::err::kErrorNoMemory; }
        }
        mTracing.store(enabled, std::memory_order_release);
        return 0;
    }
#ifdef AURA_FLOW_TESTING
    std::size_t testBudget() const noexcept { return mBudget.load(std::memory_order_acquire); }
    int testBudget(std::size_t budget) noexcept
    {
        if (!budget || budget > 8) return au::err::kErrorInvalidParam;
        { std::lock_guard<std::mutex> lock(mMutex);
          for (au::flow::detail::Node* node = mActive; node; node = node->mActiveNext)
              if (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) return au::err::kErrorBusy;
          mBudget.store(budget, std::memory_order_release); }
        mPolicyChanged.notify_all();
        return 0;
    }
#endif
    void trace(au::flow::detail::Node* node, const char* name, char phase, int code = 0) noexcept
    {
        if (!mTracing.load(std::memory_order_relaxed)) return; // Tracing is observational, default off.
        TraceStorage* storage = mTraceStorage.load(std::memory_order_acquire);
        if (!storage) return;
        const int worker = au::flow::detail::gWorker;
        const std::size_t index = worker < 0 ? 8 : static_cast<std::size_t>(worker);
        const std::uint64_t thread = worker < 0 ? au::sys::flow::threadId() : mWorkers[index].mThreadId;
        storage->mBuffers[index].record(node->mId, thread, name, phase, code);
    }
    int dumpTrace(const char* path) noexcept
    {
        if (!path) return au::err::kErrorNullPointer;
        try {
            std::ostringstream out;
            out << "{\"traceEvents\":[";
            bool first = true;
            std::size_t dropped = 0;
            TraceStorage* storage = mTraceStorage.load(std::memory_order_acquire);
            if (storage) for (const auto& buffer : storage->mBuffers) {
                dropped += buffer.dropped();
                for (const auto& slot : buffer.slots()) {
                    if (!slot.mPublished.load(std::memory_order_acquire)) continue; // Safe concurrent export.
                    if (!first) out << ',';
                    first = false;
                    out << "{\"name\":\"" << slot.mName << "\",\"cat\":\"au.flow\",\"ph\":\""
                        << slot.mPhase << "\",\"pid\":1,\"tid\":" << slot.mThread << ",\"ts\":" << slot.mTime;
                    if (slot.mPhase == 'b' || slot.mPhase == 'e') out << ",\"id\":\"" << slot.mTask << '"';
                    if (slot.mPhase == 'i') out << ",\"s\":\"t\"";
                    out << ",\"args\":{\"task\":\"" << slot.mTask << "\",\"code\":" << slot.mCode << "}}";
                }
            }
            out << "],\"displayTimeUnit\":\"ms\",\"auraDroppedRecords\":" << dropped << "}\n";
            return au::file::write(out.str(), path);
        } catch (...) { return au::err::kErrorNoMemory; }
    }
    int tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept;
private:
    enum class Phase { Dormant, Running, Stopping, Closed };
    struct Worker {
        std::thread mThread;
        std::uint64_t mThreadId = 0;
        std::uint64_t mAppliedVersion = UINT64_MAX;
        int mAffinityCode = au::err::kErrorNotReady;
        std::vector<std::uint32_t> mEffective;
    };
    au::flow::detail::Slab<au::flow::detail::Node, 128> mNodes;
    au::flow::detail::Slab<au::flow::detail::Edge, 256> mEdges;
    au::flow::detail::Slab<au::flow::detail::ExternalToken, 64> mExternal;
    au::flow::detail::ReadyQueue mCompletions;
    std::mutex mExternalMutex;
    au::flow::detail::ExternalToken* mPendingExternal = nullptr;
    au::flow::detail::ExternalToken* mPendingExternalTail = nullptr;
    std::atomic<std::size_t> mPendingExternalCount{0};
    std::atomic<std::uint64_t> mNextId{1};
    std::mutex mMutex;
    std::condition_variable mDrained;
    Phase mPhase = Phase::Dormant;
    au::flow::detail::Node* mActive = nullptr;
    std::size_t mAccepted = 0;
    std::uint64_t mOrder = 0;
    std::uint64_t mRootOrder = 0;
    std::mutex mCloseMutex;
    au::flow::detail::ReadyQueue mReady;
    au::sys::flow::WorkSemaphore mWork;
    std::atomic<std::size_t> mReadyCount{0};
    std::atomic<std::size_t> mBusy{0};
    std::atomic<bool> mStop{false};
    std::mutex mWorkerMutex;
    std::condition_variable mPolicyChanged;
    std::array<Worker, 8> mWorkers;
    std::size_t mWorkerCount = 0;
    std::size_t mAffinityQueries = 0;
    bool mClosingQueries = false;
    int mLastWorkerNative = 0; // Protected by mWorkerMutex, copied into first-submit rejection.
    std::atomic<std::size_t> mBudget{1};
    std::vector<std::uint32_t> mAllowed;
    std::vector<std::uint32_t> mPolicy;
    std::uint64_t mPolicyVersion = 0;
    std::mutex mTraceMutex;
    std::unique_ptr<TraceStorage> mTraceOwner;
    std::atomic<TraceStorage*> mTraceStorage{nullptr};
};

au::flow::detail::Core& core() noexcept { static au::flow::detail::Core instance; return instance; }
void retain(au::flow::detail::Node* node) noexcept
{
    if (node) node->mRefs.fetch_add(1, std::memory_order_relaxed); // Caller already owns a live reference.
}
void release(au::flow::detail::Node* node) noexcept
{
    if (!node || node->mRefs.fetch_sub(1, std::memory_order_acq_rel) != 1) return; // See all prior owners before destroying.
    node->mReclaimNext = au::flow::detail::gReclaim; au::flow::detail::gReclaim = node;
    if (au::flow::detail::gReclaiming) return;
    au::flow::detail::gReclaiming = true;
    while (au::flow::detail::gReclaim) {
        node = au::flow::detail::gReclaim; au::flow::detail::gReclaim = node->mReclaimNext;
        for (const auto& member : node->mMembers) if (member->mGroup == node) member->mGroup = nullptr;
        node->mOwner->recycle(node);
    }
    au::flow::detail::gReclaiming = false;
}

struct Loop {
    Loop(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept
        : mRange(range), mTile(tile), mInvoke(invoke), mPayload(payload) {}
    const std::size_t mRange;
    const std::size_t mTile;
    au_flow_tile mInvoke;
    void* mPayload;
    std::atomic<std::size_t> mNext{0};
    std::atomic<bool> mFailed{false};
    std::mutex mMutex;
    std::size_t mErrorOffset = SIZE_MAX;
    int mError = 0;
    bool mErrorException = false;
    static bool more(void* value) noexcept
    {
        au::flow::detail::Loop& loop = *static_cast<au::flow::detail::Loop*>(value);
        return !loop.mFailed.load(std::memory_order_acquire) && loop.mNext.load(std::memory_order_relaxed) < loop.mRange;
    }
    static int step(void* value) noexcept
    {
        au::flow::detail::Loop& loop = *static_cast<au::flow::detail::Loop*>(value);
        if (!more(value)) return 0;
        ++au::flow::detail::gTileDepth;
        std::size_t begin = loop.mNext.load(std::memory_order_relaxed); // Ticket only, callback data has separate lifetime.
        std::size_t count;
        do {
            if (begin == loop.mRange) { --au::flow::detail::gTileDepth; return 0; }
            count = std::min(loop.mTile, loop.mRange - begin);
        } while (!loop.mNext.compare_exchange_weak(begin, begin + count, std::memory_order_relaxed)); // Disjoint bounded intervals.
        const int code = loop.mInvoke(loop.mPayload, begin, count);
        if (code) {
            bool exception = false;
            if (code == au::flow::kErrorUnhandledException && au::flow::detail::gFrame) {
                std::lock_guard<std::mutex> lock(au::flow::detail::gFrame->mNode->mErrorMutex);
                exception = au::flow::detail::gFrame->mNode->mBodyDomain == au::flow::ErrorDomain::Flow;
            }
            { std::lock_guard<std::mutex> lock(loop.mMutex);
              if (begin < loop.mErrorOffset) { loop.mErrorOffset = begin; loop.mError = code; loop.mErrorException = exception; } }
            loop.mFailed.store(true, std::memory_order_release); // Publish stop request, not a finished callback.
        }
        --au::flow::detail::gTileDepth;
        return 0;
    }
    static int run(void* value) noexcept
    { while (more(value)) (void)step(value); return 0; }
};
int Core::tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept
{
    if (!tile || !invoke) return au::err::kErrorInvalidParam;
    if (!range) return 0;
    if (au::flow::detail::gTileDepth || au::flow::detail::gDepth >= 31) {
        for (std::size_t offset = 0; offset < range;) {
            const std::size_t count = std::min(tile, range - offset);
            const int code = invoke(payload, offset, count);
            if (code) {
                au::flow::ErrorDomain domain = au::flow::ErrorDomain::Callable;
                if (au::flow::detail::gFrame) {
                    std::lock_guard<std::mutex> lock(au::flow::detail::gFrame->mNode->mErrorMutex);
                    domain = au::flow::detail::gFrame->mNode->mBodyDomain;
                }
                scopedError(code, domain);
                return code;
            }
            offset += count;
        }
        return 0;
    }
    Loop loop{range, tile, invoke, payload};
    const std::size_t tiles = range / tile + (range % tile != 0);
    const std::size_t participants = std::min<std::size_t>(4, std::min(tiles, mBudget.load(std::memory_order_acquire)));
    std::array<au::flow::detail::NodeRef, 4> helpers;
    const bool managed = au::flow::detail::gFrame != nullptr;
    std::size_t accepted = 0;
    try {
        for (std::size_t i = managed ? 1 : 0; i < participants; ++i) {
            au::flow::detail::Node* helper = make(Kind::Work);
            au::flow::detail::NodeRef pin(helper); au::flow::detail::release(helper);
            helper->mClosure.mInvoke = &Loop::step; helper->mClosure.mPayload = &loop;
            helper->mContinue = &Loop::more;
            const int ret = submit(helper);
            if (ret) { if (!managed && !accepted) return ret; break; }
            helpers[accepted++] = std::move(pin);
        }
    } catch (const au::flow::detail::CreateFailure& error) {
        if (!managed && !accepted) return error.mCode;
    } catch (...) {
        if (!managed && !accepted) return au::err::kErrorNoMemory;
    }
    if (managed) (void)Loop::run(&loop);
    for (std::size_t i = 0; i < accepted; ++i) (void)wait(helpers[i].get(), UINT64_MAX, nullptr);
    if (loop.mErrorException) au_flow_record_exception();
    scopedError(loop.mError, loop.mErrorException ? au::flow::ErrorDomain::Flow : au::flow::ErrorDomain::Callable);
    return loop.mError;
}

int prepareExternal(const au::flow::detail::ExternalOps& ops, void* payload,
                    au::flow::detail::ExternalToken** out) noexcept
{ return au::flow::detail::core().prepareExternal(ops, payload, out); }
void armExternalCallback(au::flow::detail::ExternalToken* token) noexcept
{ if (token) token->mCallbacks.fetch_add(1, std::memory_order_relaxed); /* Registrar pins the token before registration. */ }
void leaveExternalCallback(au::flow::detail::ExternalToken* token) noexcept
{ if (token) token->mCallbacks.fetch_sub(1, std::memory_order_release); /* No token access after publishing callback exit. */ }
void notifyExternal(au::flow::detail::ExternalToken* token, const au::flow::detail::ExternalStatus& status) noexcept
{ if (token) token->mCore->notifyExternal(token, status); }
void pollExternal(au::flow::detail::ExternalToken* token) noexcept
{
    if (token) {
        token->mPolling.store(true, std::memory_order_release); // Payload is prepared before enabling fallback query.
        token->mCore->queueExternal(token);
    }
}
void leaveExternalRegistrar(au::flow::detail::ExternalToken* token) noexcept
{ if (token) token->mRegistrar.store(false, std::memory_order_release); /* Last registrar access. */ }

} // namespace detail
} // namespace flow
} // namespace au

#ifdef AURA_FLOW_TESTING
void au::flow::testing::failAfter(au::flow::testing::Failure failure, int count) noexcept
{ au::flow::detail::gFailure[static_cast<unsigned int>(failure)].store(count, std::memory_order_relaxed); }
void au::flow::testing::snapshotHook(au::flow::testing::SnapshotHook* hook) noexcept
{ au::flow::detail::gSnapshotHook.store(hook, std::memory_order_release); }
std::size_t au::flow::testing::budget() noexcept { return au::flow::detail::core().testBudget(); }
int au::flow::testing::setBudget(std::size_t budget) noexcept { return au::flow::detail::core().testBudget(budget); }
#endif

au::flow::XFlow& au::flow::XFlow::get() noexcept { static au::flow::XFlow facade; return facade; }
const char* au::flow::getErrorStr(int code) noexcept
{
    switch (code) {
        case au::flow::kErrorDependencyFailed: return "ErrorDependencyFailed";
        case au::flow::kErrorCycleDetected: return "ErrorCycleDetected";
        case au::flow::kErrorWouldDeadlock: return "ErrorWouldDeadlock";
        case au::flow::kErrorUnhandledException: return "ErrorUnhandledException";
        default: return au::err::getErrorStr(code);
    }
}
int au::flow::XFlow::getWorkers(au::flow::WorkerInfo* out, std::size_t capacity, std::size_t& count) const noexcept
{
    if (capacity && !out) return au::err::kErrorInvalidParam;
    std::array<au_flow_worker, 8> values{};
    for (auto& value : values) value.mStructSize = sizeof(value);
    const int ret = au_flow_workers(values.data(), std::min<std::size_t>(capacity, 8), &count);
    for (std::size_t i = 0; i < std::min(capacity, count); ++i)
        out[i] = {sizeof(au::flow::WorkerInfo), values[i].mWorkerId, values[i].mThreadId,
                  values[i].mRequestedVersion, values[i].mAppliedVersion, values[i].mAffinityCode};
    return ret;
}

uint32_t au_flow_version() noexcept { return AURA_FLOW_ABI_VERSION; }
int au_flow_make(au_flow_invoke invoke, au_flow_destroy destroy, void* payload, au_flow_handle* out) noexcept
{
    au::flow::detail::Closure owner; owner.mPayload = payload; owner.mDestroy = destroy;
    if (!out || !invoke) return au::err::kErrorInvalidParam;
    *out = nullptr;
    try {
        au::flow::detail::Node* node = au::flow::detail::core().make(au::flow::detail::Kind::Work);
        node->mClosure.mInvoke = invoke; node->mClosure.mDestroy = destroy; node->mClosure.mPayload = payload;
        owner.mPayload = nullptr; *out = node; return 0;
    } catch (const au::flow::detail::CreateFailure& error) { return error.mCode; }
    catch (...) { return au::err::kErrorNoMemory; }
}
int au_flow_group(au_flow_handle* out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    try {
        au::flow::detail::Node* node = au::flow::detail::core().make(au::flow::detail::Kind::Group);
        au::flow::detail::NodeRef pin(node); au::flow::detail::release(node);
        au::flow::detail::Node* entry = au::flow::detail::core().make(au::flow::detail::Kind::Entry);
        node->mEntry = au::flow::detail::NodeRef(entry); au::flow::detail::release(entry);
        au::flow::detail::retain(node); *out = node;
        return 0;
    } catch (const au::flow::detail::CreateFailure& error) { return error.mCode; }
    catch (...) { return au::err::kErrorNoMemory; }
}
void au_flow_retain(au_flow_handle handle) noexcept { au::flow::detail::retain(static_cast<au::flow::detail::Node*>(handle)); }
void au_flow_release(au_flow_handle handle) noexcept { au::flow::detail::release(static_cast<au::flow::detail::Node*>(handle)); }
uint64_t au_flow_id(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return au::flow::detail::core().owns(node) ? node->mId : 0;
}
uint32_t au_flow_state(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return static_cast<uint32_t>(au::flow::detail::core().owns(node) ? node->mState.load(std::memory_order_acquire)
        : au::flow::TaskState::Invalid);
}
int au_flow_submission(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    return au::flow::detail::core().owns(node) ? node->mSubmission.load(std::memory_order_acquire) : au::err::kErrorInvalidHandle;
}
int au_flow_result_get(au_flow_handle handle, au_flow_result* out) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (!au::flow::detail::core().owns(node)) return au::err::kErrorInvalidHandle;
    if (!out) return au::err::kErrorNullPointer;
    if (out->mStructSize != sizeof(*out)) return au::err::kErrorVersionMismatch;
    if (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) return au::err::kErrorNotReady;
    *out = node->mResult; return 0;
}
int au_flow_name(au_flow_handle handle, const char* name) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (!au::flow::detail::core().owns(node)) return au::err::kErrorInvalidHandle;
    if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft) return au::err::kErrorStateInvalid;
    if (!name) return node->mBuildError = au::err::kErrorNullPointer;
    try { node->mName = name; return 0; }
    catch (...) { return node->mBuildError = au::err::kErrorNoMemory; }
}
int au_flow_precede(au_flow_handle before, au_flow_handle after) noexcept
{ return au::flow::detail::core().precede(static_cast<au::flow::detail::Node*>(before), static_cast<au::flow::detail::Node*>(after)); }
int au_flow_builder_error(au_flow_handle handle, int code) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (au::flow::detail::core().owns(node) &&
        node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft && !node->mBuildError)
        node->mBuildError = code;
    return code;
}
int au_flow_add(au_flow_handle group, au_flow_handle member) noexcept
{ return au::flow::detail::core().add(static_cast<au::flow::detail::Node*>(group), static_cast<au::flow::detail::Node*>(member)); }
void au_flow_abandon(au_flow_handle group) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(group);
    if (au::flow::detail::core().owns(node) && node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft)
        au::flow::detail::core().reject(node, au::err::kErrorAborted);
}
int au_flow_submit(au_flow_handle handle) noexcept
{ return au::flow::detail::core().submit(static_cast<au::flow::detail::Node*>(handle)); }
void au_flow_record_error(int code) noexcept
{ au::flow::detail::core().scopedError(code, code == au::err::kErrorNoMemory
    ? au::flow::ErrorDomain::Memory : au::flow::ErrorDomain::Flow); }
void au_flow_record_exception() noexcept
{
    if (au::flow::detail::gFrame) {
        au::flow::detail::Node* node = au::flow::detail::gFrame->mNode;
        std::lock_guard<std::mutex> lock(node->mErrorMutex);
        node->mBodyDomain = au::flow::ErrorDomain::Flow;
    }
}
int au_flow_wait(au_flow_handle handle, uint64_t timeout, au_flow_result* out) noexcept
{ return au::flow::detail::core().wait(static_cast<au::flow::detail::Node*>(handle), timeout, out); }
int au_flow_wait_all() noexcept { return au::flow::detail::core().waitAll(); }
int au_flow_tiled(size_t range, size_t tile, au_flow_tile invoke, void* payload) noexcept
{ return au::flow::detail::core().tiled(range, tile, invoke, payload); }
int au_flow_affinity(uint32_t mode, const uint32_t* cpus, size_t count) noexcept
{ return au::flow::detail::core().affinity(mode, cpus, count); }
int au_flow_workers(au_flow_worker* out, size_t capacity, size_t* count) noexcept
{ return au::flow::detail::core().workers(out, capacity, count); }
int au_flow_worker_affinity(uint32_t id, uint32_t* cpus, size_t capacity, size_t* count) noexcept
{ return au::flow::detail::core().workerAffinity(id, cpus, capacity, count); }
int au_flow_tracing(int enabled) noexcept
{ return au::flow::detail::core().tracing(enabled != 0); }
int au_flow_dot(au_flow_handle handle, const char* path) noexcept
{ return au::flow::detail::core().dot(static_cast<au::flow::detail::Node*>(handle), path); }
int au_flow_trace(const char* path) noexcept
{ return au::flow::detail::core().dumpTrace(path); }
int au_flow_shutdown() noexcept { return au::flow::detail::core().shutdown(); }
