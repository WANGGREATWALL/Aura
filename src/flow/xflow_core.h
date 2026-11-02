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
constexpr unsigned    kMaxTaskDepth   = 128;

enum class TaskState
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

// Shared by owning handles, accepted work and DAG predecessors.
struct Node
{
    std::string  mName;
    std::int64_t mCostUs = 0;
    std::mutex   mMutex;

    TaskState mState = TaskState::Created;

    int mBuildCode      = 0;
    int mDependencyCode = 0;
    int mResultCode     = 0;

    std::size_t mPendingDependencyCount = 0;

    std::vector<NodePtr>             mPredecessors;
    std::vector<std::weak_ptr<Node>> mSuccessors;

    Node* mNextReady = nullptr;
    Node* mWaiting   = nullptr;  // Cooperative wait target.
    Node* mExecuting = nullptr;  // Task currently helped on this task's call stack.

    // Position in Core::mAccepted; stable across list splices.
    std::list<NodePtr>::iterator mAcceptedPos;

    XFlowTaskFn                           mFunction;  // Task factories reject null functions.
    std::unique_ptr<void, PayloadDeleter> mPayload;

    // Enabled only after payload construction commits; never destroy an unconstructed object.
    XFlowDeleterFn    mPendingDestroy = nullptr;
    std::atomic<bool> mPayloadReady{true};

    explicit Node(XFlowTaskFn fn) : mFunction(fn) {}
};

struct TileJob
{
    std::atomic<std::size_t> mNextTileIndex{0};
    std::atomic<std::size_t> mActiveHelpers{0};
    std::size_t              mRange;
    std::size_t              mTile;
    std::size_t              mTileCount;
    XFlowTileExFn            mFunction;
    void*                    mData;
    TileJob*                 mNextJob = nullptr;

    void run(std::size_t workerIndex) noexcept;
};

// mMutex serializes graph edits, state transitions and worker/tile bookkeeping.
// Node mutexes protect public queries and payload commit; state writes hold both.
// Lock order: Core::mMutex -> node mutex. User callbacks and final owners are released outside both.
struct Core
{
    std::mutex                           mMutex;
    std::condition_variable              mChanged;
    std::list<NodePtr>                   mAccepted;
    Node*                                mReadyHead      = nullptr;
    Node*                                mReadyTail      = nullptr;
    TileJob*                             mActiveTileJobs = nullptr;
    std::array<std::thread, kMaxWorkers> mThreads;
    std::size_t                          mWorkerCount     = 0;
    std::size_t                          mIdleWorkerCount = 0;
    std::size_t                          mTileCallers     = 0;

    bool mClosing   = false;
    bool mStop      = false;
    bool mReleasing = false;

    // Snapshot of the initializing thread's affinity. A zero request restores the full snapshot.
    // Accepted configurations are applied asynchronously; acceptance does not confirm OS binding.
    std::vector<std::uint32_t> mInitialCpus;
    std::uint64_t              mRequestedCpuMask = 0;
    std::uint64_t              mInitialCpuMask   = 0;
    int                        mAffinityInitCode = 0;
    std::atomic<std::uint64_t> mAffinityVersion{1};

    Core();
    ~Core();

    // Graph/queue helpers below require mMutex; they do not acquire it themselves.
    bool     reaches(Node* from, Node* target, bool includeWaits = false);
    int      addDependency(const NodePtr& predecessor, const NodePtr& successor);
    int      submit(const NodePtr& root);
    void     enqueueReady(Node* node);
    void     growWorkers();
    TileJob* findTileJob();

    // Both require the supplied lock on entry and return; may unlock for OS/user work.
    void applyAffinity(std::unique_lock<std::mutex>& lock);
    bool helpOne(std::unique_lock<std::mutex>& lock);

    // Acquire mMutex internally; callers must not already hold it.
    void workerLoop(std::size_t workerIndex);
    int  wait(const NodePtr& task);
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

    explicit XFlowTaskNode(XFlowTaskFn fn) : mStorage(fn) {}
};

#endif
