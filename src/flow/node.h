#ifndef AURA_FLOW_NODE_H_
#define AURA_FLOW_NODE_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "flow/ready_queue.h"
#include "flow/task_errors.h"
#include "flow/xflow.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
class AU_LOCAL Core;
struct Node;
void retain(Node*) noexcept;
void release(Node*) noexcept;

// Shared across accepted work, handles, dependency registration and parent
// scopes. Intrusive sharing avoids an allocation for each copied handle.
class NodeRef
{
public:
    NodeRef() noexcept = default;
    explicit NodeRef(Node* value) noexcept : mValue(value) { retain(mValue); }
    NodeRef(const NodeRef& other) noexcept : NodeRef(other.mValue) {}
    NodeRef(NodeRef&& other) noexcept : mValue(other.release()) {}
    ~NodeRef() noexcept { au::flow::detail::release(mValue); }
    NodeRef& operator=(NodeRef other) noexcept
    {
        std::swap(mValue, other.mValue);
        return *this;
    }
    Node*          get() const noexcept { return mValue; }
    Node*          operator->() const noexcept { return mValue; }
    static NodeRef adopt(Node* value) noexcept
    {
        NodeRef ref;
        ref.mValue = value;
        return ref;
    }
    Node* release() noexcept { return std::exchange(mValue, nullptr); }

private:
    Node* mValue = nullptr;
};

struct Closure
{
    Closure(au_flow_invoke invoke = nullptr, au_flow_destroy destroy = nullptr, void* payload = nullptr) noexcept
        : mInvoke(invoke), mDestroy(destroy), mPayload(payload)
    {
    }
    Closure(const Closure&)            = delete;
    Closure& operator=(const Closure&) = delete;
    Closure(Closure&& other) noexcept : Closure(other.mInvoke, other.mDestroy, std::exchange(other.mPayload, nullptr))
    {
    }
    Closure& operator=(Closure&& other) noexcept
    {
        if (this != &other) {
            clear();
            mInvoke  = other.mInvoke;
            mDestroy = other.mDestroy;
            mPayload = std::exchange(other.mPayload, nullptr);
        }
        return *this;
    }
    au_flow_invoke  mInvoke  = nullptr;
    au_flow_destroy mDestroy = nullptr;
    void*           mPayload = nullptr;
    ~Closure() noexcept { clear(); }
    void clear() noexcept
    {
        void* payload = mPayload;
        mPayload      = nullptr;
        if (payload && mDestroy)
            mDestroy(payload);
    }
};
struct Edge
{
    NodeRef mTarget;
    Edge*   mNext = nullptr;
};
Edge* closedEdges() noexcept;
enum class Kind
{
    Work,
    Group,
    Entry
};
struct CreateFailure
{
    int mCode;
};

struct Node
{
    Node(Core* owner, std::uint64_t id, Kind kind) : mOwner(owner), mId(id), mKind(kind) { mReady.mValue = this; }
    std::atomic<std::size_t>         mRefs{1};
    Core* const                      mOwner;
    const std::uint64_t              mId;
    const Kind                       mKind;
    std::atomic<au::flow::TaskState> mState{au::flow::TaskState::Draft};
    std::atomic<int>                 mSubmission{au::err::kErrorNotReady};
    Closure                          mClosure;
    bool (*mContinue)(void*) noexcept = nullptr;  // Private tiled helper continuation, never an ordinary Task.
    std::string          mName;
    std::vector<NodeRef> mPreds;
    std::vector<NodeRef> mMembers;
    NodeRef              mEntry;
    NodeRef              mGate;
    NodeRef              mParent;
    Edge*                mDynamicChildren = nullptr;  // Diagnostic edges only, protected for prepend by Core::mMutex.
    bool mGraphCaptured = false;  // Immutable after acceptance; tracing opt-in is inherited by structured descendants.
    std::atomic<std::size_t> mGraphDropped{0};
    Node*                    mGroup      = nullptr;  // Borrowed; group owns its draft members.
    int                      mBuildError = 0;
    std::atomic<Edge*>       mSuccessors{nullptr};
    std::atomic<std::size_t> mUnresolved{0};
    std::atomic<bool>        mCommitted{false};
    std::atomic<std::size_t> mOutstanding{1};  // Body reservation, plus accepted children/events.
    TaskErrors               mErrors;
    au_flow_result           mResult{};
    std::mutex               mWaitMutex;
    std::condition_variable  mChanged;
    std::size_t              mCooperativeWaiters = 0;  // Protected by mWaitMutex; finish signals the unified gate.
    Node*                    mWaitTarget         = nullptr;  // Protected by Core::mMutex.
    Node*                    mExecutingChild     = nullptr;  // Cooperative stack edge, protected by Core::mMutex.
    ReadyLink                mReady;
    // Borrowed completion adjacency; Core::mMutex protects links and traversal
    // scratch. Accepted references pin children until unlink at finish.
    Node*         mFirstChild   = nullptr;
    Node*         mSiblingPrev  = nullptr;
    Node*         mSiblingNext  = nullptr;
    Node*         mSearchNext   = nullptr;
    bool          mSearchQueued = false;
    bool          mSearchTarget = false;
    Node*         mActivePrev   = nullptr;
    Node*         mActiveNext   = nullptr;
    Node*         mFinishNext   = nullptr;
    Node*         mReclaimNext  = nullptr;
    std::uint64_t mOrder        = 0;
    std::uint64_t mRootOrder    = 0;
};

bool reserveOutstanding(Node* node) noexcept;
bool finalState(au::flow::TaskState state) noexcept;

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_NODE_H_
