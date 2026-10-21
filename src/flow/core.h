#ifndef AURA_FLOW_CORE_H_
#define AURA_FLOW_CORE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "flow/constants.h"
#include "flow/diagnostics.h"
#include "flow/execution_context.h"
#include "flow/external_token.h"
#include "flow/node.h"
#include "flow/payload_pool.h"
#include "flow/slab.h"
#include "flow/testing_internal.h"
#include "sys/flow_runtime.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
// Coordinates shared invariants. Implementations are separated by responsibility;
// ownership/lock rules live in docs/flow_locking.md. No locks are required on
// entry unless a method explicitly says otherwise below.
class AU_LOCAL Core
{
public:
    PayloadPool& payloads() noexcept { return mPayloads; }
    class CleanupExtent
    {
    public:
        CleanupExtent(Core* core, Node* node) noexcept;
        ~CleanupExtent() noexcept;
        CleanupExtent(const CleanupExtent&)            = delete;
        CleanupExtent& operator=(const CleanupExtent&) = delete;

    private:
        TileSuspension  mTileSuspension;
        Frame           mFrame;
        ExecutionErrors mErrors;
    };
    Core() noexcept;
    ~Core() noexcept;
    NodeRef make(Kind kind);
    void    recycle(Node* node) noexcept;
    bool    owns(const Node* node) const noexcept { return node && node->mOwner == this; }
    int     builderError(Node* node, int code) noexcept;
    void    scopedError(int code, au::flow::ErrorDomain domain = au::flow::ErrorDomain::Flow, int native = 0,
                        std::uint64_t cause = 0) noexcept;
    int     prepareExternal(const ExternalOps& ops, void* payload, ExternalToken** out) noexcept;
    void    queueExternal(ExternalToken* token) noexcept;
    void    notifyExternal(ExternalToken* token, const ExternalStatus& status) noexcept;
    bool    consumeExternal(ExternalToken* token) noexcept;
    void    deferExternal(ExternalToken* token, bool announce = true) noexcept;
    bool    pollPendingExternal() noexcept;

    // Follow strong draft references before inserting one. This rejects reference
    // cycles even for builders that are discarded without ever being submitted.

    // Acquire mMutex. Callers must still serialize edits/submission/destruction
    // of each Draft object; accepted predecessor graphs remain immutable.
    int precede(Node* before, Node* after) noexcept;
    int add(Node* group, Node* member) noexcept;
    // No scheduler mutex on entry: rejecting clears user-owned closures.
    void reject(Node* top, int error, int native = 0) noexcept;

    struct Prepared
    {
        Node*              mNode;
        Node*              mParent;
        Node*              mGate;
        std::vector<Node*> mPreds;
    };
    struct EdgeOwner
    {
        Core* mCore;
        void  operator()(Edge* edge) const noexcept
        {
            if (edge)
                mCore->mEdges.destroy(edge);
        }
    };
    struct Registration
    {
        Node*                                  mBefore;
        std::unique_ptr<Edge, Core::EdgeOwner> mEdge;
    };

    class WorkerAdmission
    {
    public:
        explicit WorkerAdmission(Core* core) noexcept : mCore(core) {}
        int accept(int* native = nullptr) noexcept;
        ~WorkerAdmission() noexcept;
        WorkerAdmission(const WorkerAdmission&)            = delete;
        WorkerAdmission& operator=(const WorkerAdmission&) = delete;

    private:
        Core* mCore;
        bool  mAccepted = false;
    };
    int submit(Node* top, bool reportFailure = true) noexcept;

    void inherit(Node* target, const au_flow_result& source, std::uint64_t order) noexcept;
    void resolve(Node* target, const au_flow_result& source, std::uint64_t order,
                 Node** continuation = nullptr) noexcept;
    void publishReady(Node* node) noexcept;
    void activate(Node* node, Node** continuation = nullptr) noexcept;
    void bodyEnd(Node* node, int code, Node** continuation = nullptr) noexcept;
    void finish(Node* node, Node** continuation = nullptr) noexcept;

    void notifyExternalWaiters() noexcept { mWork.control(); }
    void releasePoller() noexcept;
    bool pumpExternal(std::chrono::steady_clock::time_point& deadline) noexcept;
    // Dispatch may run user code; never enter holding graph/worker/provider locks.
    bool helpOne(bool blocking, std::chrono::steady_clock::time_point deadline, Node* awaited = nullptr,
                 const std::uint64_t* observed = nullptr, bool (*externalReady)(void*) noexcept = nullptr,
                 void*                externalPayload = nullptr) noexcept;
    void run(Node* node, Node** continuation = nullptr) noexcept;

    void leaveCooperativeWait() noexcept;
    class CooperativeWait
    {
    public:
        CooperativeWait(Core* core, Node* node) noexcept;
        ~CooperativeWait() noexcept;
        CooperativeWait(const CooperativeWait&)            = delete;
        CooperativeWait& operator=(const CooperativeWait&) = delete;

    private:
        Core* mCore;
        Node* mNode;
    };

    int wait(Node* node, std::uint64_t timeout, au_flow_result* out) noexcept;
    int waitAll() noexcept;

    int shutdown() noexcept;

    // Requires mWorkerMutex; the locked caller owns worker slot transitions.
    std::size_t eligibleWorkers() const noexcept;
    std::size_t workDemand() const noexcept;
    void        grow(bool force = false) noexcept;
    // Requires lock owning mWorkerMutex; temporarily unlocks for native join.
    int startWorker(std::unique_lock<std::mutex>& lock) noexcept;
    // Requires mWorkerMutex. retire itself acquires worker -> graph.
    void updateRetirementPending() noexcept;
    bool retire(std::size_t id) noexcept;
    void workerLoop(std::size_t id) noexcept;
    void applyPolicy(std::size_t id) noexcept;
    int  affinity(std::uint32_t mode, const std::uint32_t* cpus, std::size_t count) noexcept;
    int  workers(au_flow_worker* out, std::size_t capacity, std::size_t* count) noexcept;

    int  workerAffinity(std::uint32_t id, std::uint32_t* cpus, std::size_t capacity, std::size_t* count) noexcept;
    int  dot(Node* top, const char* path) noexcept;
    bool tracingEnabled() const noexcept { return mDiagnostics.enabled(); }
    int  tracing(bool enabled) noexcept { return mDiagnostics.setEnabled(enabled); }
    int  dumpTrace(const char* path) const noexcept { return mDiagnostics.dumpTrace(path); }
#ifdef AURA_FLOW_TESTING
    int                             queueHooks(const QueueHooks& hooks) noexcept;
    au::flow::testing::RuntimeStats runtimeStats() noexcept;
    std::size_t                     testBudget() const noexcept;
    int                             testBudget(std::size_t budget) noexcept;
#endif
    void trace(Node* node, const char* name, char phase, int code = 0) noexcept;
    class JobScope
    {
    public:
        explicit JobScope(Core* core) noexcept : mCore(core) {}
        int accept() noexcept;
        ~JobScope() noexcept;
        JobScope(const JobScope&)            = delete;
        JobScope& operator=(const JobScope&) = delete;

    private:
        Core* mCore;
        bool  mAccepted = false;
    };

    // Only called by an accepted Job. Helpers have no DAG predecessors and do
    // not form independent waitAll roots. Retain Node identity for wait-cycle
    // checks and completion, but skip general graph preparation/allocations.
    std::size_t acceptHelpers(NodeRef* helpers, std::size_t count) noexcept;
    void        joinHelper(Node* node) noexcept;

    int  tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept;
    void externalProfile(const ExternalProfile& profile) noexcept;

private:
    enum class Phase
    {
        Dormant,
        Running,
        Stopping,
        Closed
    };
    enum class WorkerState
    {
        Free,
        Running,
        Retiring,
        Exited,
        Joining
    };
    struct Worker
    {
        std::thread                           mThread;
        std::uint64_t                         mThreadId = 0;
        std::atomic<std::uint64_t>            mAppliedVersion{UINT64_MAX};
        std::uint64_t                         mGeneration    = 0;
        WorkerState                           mState         = WorkerState::Free;
        std::size_t                           mPins          = 0;
        bool                                  mRetirePending = false;
        bool                                  mPollProgress  = false;
        std::size_t                           mPollRemaining = 0;
        std::chrono::steady_clock::time_point mPollDue;
        std::chrono::microseconds             mPollDelay{kPollMinimum};
        int                                   mAffinityCode = au::err::kErrorNotReady;
    };
    // Self-synchronized storage; object destruction precedes slab recycle locks.
    PayloadPool             mPayloads;
    Slab<Node, 128>         mNodes;
    Slab<Edge, 256>         mEdges;
    Slab<ExternalToken, 64> mExternal;
    // External queue is concurrent; pending-list links require mExternalMutex.
    ReadyQueue                 mCompletions;
    std::mutex                 mExternalMutex;
    ExternalToken*             mPendingExternal     = nullptr;
    ExternalToken*             mPendingExternalTail = nullptr;
    std::atomic<std::size_t>   mPendingExternalCount{0};
    std::atomic<std::uint64_t> mNextId{1};
    // Graph/acceptance state and Node completion adjacency/search scratch.
    std::mutex              mMutex;
    std::condition_variable mDrained;
    Phase                   mPhase     = Phase::Dormant;
    Node*                   mActive    = nullptr;
    std::size_t             mAccepted  = 0;
    std::size_t             mJobs      = 0;  // Accepted synchronous calls, protected by mMutex with Phase.
    std::uint64_t           mOrder     = 0;
    std::uint64_t           mRootOrder = 0;
    // Serializes shutdown callers; held across drain and native joins outside graph/worker locks.
    std::mutex                   mCloseMutex;
    ReadyQueue                   mReady;
    au::sys::flow::WorkSemaphore mWork;
    std::atomic<std::size_t>     mReadyCount{0};
    std::atomic<std::size_t>     mBusy{0};
    std::atomic<bool>            mStop{false};
    // Worker slots, pins and policy snapshots. Atomics below expose hints only
    // where documented; all lifetime decisions still use this mutex.
    std::mutex                      mWorkerMutex;
    std::condition_variable         mPolicyChanged;
    std::array<Worker, kMaxWorkers> mWorkers;
    std::atomic<std::size_t>        mWorkerCount{0};
    std::atomic<std::size_t>        mEligibleCount{0};
    std::size_t                     mAdmissions = 0;
    std::size_t                     mJoiners    = 0;
    std::atomic<bool>               mRetirePending{false};
    std::atomic<int>                mPollOwner{-1};
    std::atomic_flag                mPolling         = ATOMIC_FLAG_INIT;
    std::size_t                     mAffinityQueries = 0;
    bool                            mClosingQueries  = false;
    int                        mLastWorkerNative = 0;  // Protected by mWorkerMutex, copied into first-submit rejection.
    std::atomic<std::size_t>   mBudget{1};
    std::vector<std::uint32_t> mAllowed;
    std::vector<std::uint32_t> mPolicy;
    std::atomic<std::uint64_t> mPolicyVersion{0};
#ifdef AURA_FLOW_TESTING
    std::atomic<std::uint64_t> mWaitTimeouts{0}, mControlWakes{0}, mWorkWakes{0}, mPollPasses{0}, mRetirements{0};
#endif
    Diagnostics mDiagnostics;
};
Core& core() noexcept;

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_CORE_H_
