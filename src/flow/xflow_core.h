#ifndef AURA_FLOW_XFLOW_CORE_H_
#define AURA_FLOW_XFLOW_CORE_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "flow/xflow_api.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
constexpr std::size_t kMaxWorkers     = 8;
constexpr std::size_t kMaxTileHelpers = 3;
constexpr unsigned    kMaxWaitDepth   = 128;

enum class State
{
    Created,
    Pending,
    Ready,
    Running,
    Done
};

struct Node;
typedef std::shared_ptr<Node> NodePtr;

struct PayloadDeleter
{
    XFlowDeleterFn mDestroy   = nullptr;
    std::size_t    mAlignment = 0;  // Zero means externally managed or inline storage.

    void operator()(void* data) const noexcept;
};

// Shared by owning handles, accepted work and DAG predecessors; no custom pool.
struct Node
{
    std::mutex                            mMutex;
    State                                 mState = State::Created;
    int                                   mBuild = 0, mDependencyError = 0, mCode = 0;
    std::size_t                           mPending = 0;
    std::weak_ptr<Node>                   mGroupExit;  // Validate whole-group submission without an ownership cycle.
    std::vector<NodePtr>                  mPreds;
    std::vector<std::weak_ptr<Node>>      mSuccessors;
    Node *                                mNext = nullptr, *mWaiting = nullptr, *mExecuting = nullptr;
    std::list<NodePtr>::iterator          mAccepted;
    XFlowTaskFn                           mFunction;
    std::unique_ptr<void, PayloadDeleter> mPayload;
    XFlowDeleterFn                        mPendingDestroy = nullptr;
    std::atomic<bool>                     mPayloadReady{true};
    std::string                           mName;
    std::int64_t                          mCost = 0;

    Node(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy)
        : mFunction(fn), mPayload(data, PayloadDeleter{destroy, 0}), mName(name ? name : "")
    {
    }
};

struct TileJob
{
    std::atomic<std::size_t> mNext{0};
    std::atomic<std::size_t> mHelpers{0};
    std::size_t              mRange, mTile, mTotal;
    XFlowTileExFn            mFunction;
    void*                    mData;
    TileJob*                 mLink = nullptr;
    void                     run(std::size_t worker) noexcept;
};

// mMutex serializes graph edits, state transitions and worker/tile bookkeeping.
// Node mutexes protect public queries and payload commit; state writes hold both.
// Lock order: Core::mMutex -> node mutex(es). User callbacks and final owners are released outside both.
struct Core
{
    std::mutex                           mMutex;
    std::condition_variable              mChanged;
    std::list<NodePtr>                   mAccepted;
    Node *                               mHead = nullptr, *mTail = nullptr;
    TileJob*                             mTiles = nullptr;
    std::array<std::thread, kMaxWorkers> mThreads;
    std::size_t                          mCount = 0, mIdle = 0, mTileCallers = 0;
    std::vector<std::uint32_t>           mDefaultCpus;
    bool                                 mClosing = false, mStop = false, mReleasing = false;
    std::uint64_t                        mMask = 0, mDefaultMask = 0;
    bool                                 mDefaultAffinity = true;
    int                                  mAffinityStatus  = 0;
    std::atomic<std::uint64_t>           mAffinityGen{1};
    Core();

    ~Core();

    // Graph/queue helpers below require mMutex; they do not acquire it themselves.
    bool reaches(Node*, Node*, bool waits = false);

    int edge(const NodePtr&, const NodePtr&);

    int submit(const NodePtr&);

    void ready(Node*);

    void grow();

    TileJob* tile();

    // Temporarily unlock for OS/user work; the supplied lock is owned on entry and return.
    void applyAffinity(std::unique_lock<std::mutex>&);

    bool step(std::unique_lock<std::mutex>&);

    void worker(std::size_t index);

    int wait(const NodePtr&);
};

Core& core();
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

struct AU_LOCAL XFlowTaskNode
{
    static constexpr std::size_t kInlinePayloadCapacity = 64;

    std::atomic<std::size_t> mRefs{1};
    alignas(std::max_align_t) unsigned char mInlinePayload[kInlinePayloadCapacity];

    au::flow::detail::Node mStorage;
    // One C owning pin, explicitly cleared by the final C release. Aliasing
    // shared owners in the DAG/accepted list retain this entire allocation.
    au::flow::detail::NodePtr mNode;
    bool                      mUsesInlinePayload = false;

    XFlowTaskNode(XFlowTaskFn fn, XFlowDeleterFn destroy) : mStorage(nullptr, fn, nullptr, destroy) {}
};

struct AU_LOCAL XFlowGroupNode
{
    std::atomic<std::size_t>               mRefs{1};
    au::flow::detail::NodePtr              mEntry, mExit;
    std::vector<au::flow::detail::NodePtr> mNodes;
    int                                    mBuild = 0;
};
#endif
