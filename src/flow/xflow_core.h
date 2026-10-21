#ifndef AURA_FLOW_XFLOW_CORE_H_
#define AURA_FLOW_XFLOW_CORE_H_
#include "flow/xflow_api.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace au { namespace flow AU_LOCAL { namespace detail {
enum class State { Created, Pending, Ready, Running, Done };
struct Node;
typedef std::shared_ptr<Node> NodePtr;
// Shared by owning handles, accepted work and DAG predecessors; no custom pool.
struct Node {
    std::mutex mMutex;
    State mState = State::Created;
    int mBuild = 0, mDependencyError = 0, mCode = 0;
    std::size_t mPending = 0;
    bool mGrouped = false;
    std::vector<NodePtr> mPreds;
    std::vector<std::weak_ptr<Node>> mSuccessors;
    Node *mNext = nullptr, *mWaiting = nullptr, *mExecuting = nullptr;
    std::list<NodePtr>::iterator mAccepted;
    XFlowTaskFn mFunction;
    std::unique_ptr<void, XFlowDeleterFn> mPayload;
    std::string mName;
    std::int64_t mCost = 0;
    Node(const char* name, XFlowTaskFn fn, void* data, XFlowDeleterFn destroy)
        : mFunction(fn), mPayload(data, destroy), mName(name ? name : "") {}
};
struct TileJob {
    std::atomic<std::size_t> mNext{0};
    std::atomic<std::size_t> mHelpers{0};
    std::size_t mRange, mTile, mTotal;
    XFlowTileFn mFunction;
    void* mData;
    TileJob* mLink = nullptr;
    void run() noexcept;
};
// Lock order: Core::mMutex -> node mutex(es). No callbacks under either.
struct Core {
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::list<NodePtr> mAccepted;
    Node *mHead = nullptr, *mTail = nullptr;
    TileJob* mTiles = nullptr;
    std::array<std::thread, 8> mThreads;
    std::size_t mCount = 0, mIdle = 0, mTileCallers = 0;
    std::vector<unsigned> mDefaultCpus;
    int mPolicy = XFLOW_AFFINITY_DEFAULT;
    bool mClosing = false, mStop = false;
    int mFirstError = 0;
    std::uint64_t mMask = 0, mDefaultMask = 0;
    std::atomic<std::uint64_t> mAffinityGen{1};
    Core();
    ~Core();
    bool reaches(Node*, Node*, bool waits = false);
    int edge(const NodePtr&, const NodePtr&);
    int submit(const std::vector<NodePtr>&);
    void ready(Node*);
    void grow();
    TileJob* tile();
    bool step(std::unique_lock<std::mutex>&);
    void worker();
    int wait(const NodePtr&);
};
Core& core();
}}}
struct AU_LOCAL XFlowTaskNode {
    std::atomic<std::size_t> mRefs{1};
    au::flow::detail::NodePtr mNode;
};
struct AU_LOCAL XFlowGroupNode {
    std::atomic<std::size_t> mRefs{1};
    au::flow::detail::NodePtr mEntry, mExit;
    std::vector<au::flow::detail::NodePtr> mNodes;
    int mBuild = 0;
};
#endif
