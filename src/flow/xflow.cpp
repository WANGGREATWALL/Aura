#include "flow/xflow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flow/ready_queue.h"
#include "flow/slab.h"
#include "flow/diagnostics.h"
#include "flow/testing.h"
#include "flow/completion.h"
#include "flow/payload_pool.h"
#include "mm/access_context.h"
#include "mm/registry.h"
#include "log/xlogger.h"
#include "perf/xperf.h"
#include "sys/flow_runtime.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

class AU_LOCAL Core;
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
    NodeRef(au::flow::detail::NodeRef&& other) noexcept : mValue(other.release()) {}
    ~NodeRef() noexcept { au::flow::detail::release(mValue); }
    au::flow::detail::NodeRef& operator=(au::flow::detail::NodeRef other) noexcept
    { std::swap(mValue, other.mValue); return *this; }
    au::flow::detail::Node* get() const noexcept { return mValue; }
    au::flow::detail::Node* operator->() const noexcept { return mValue; }
    static au::flow::detail::NodeRef adopt(au::flow::detail::Node* value) noexcept
    { au::flow::detail::NodeRef ref; ref.mValue = value; return ref; }
    au::flow::detail::Node* release() noexcept { return std::exchange(mValue, nullptr); }
private:
    au::flow::detail::Node* mValue = nullptr;
};

struct Closure {
    Closure(au_flow_invoke invoke = nullptr, au_flow_destroy destroy = nullptr, void* payload = nullptr) noexcept
        : mInvoke(invoke), mDestroy(destroy), mPayload(payload) {}
    Closure(const au::flow::detail::Closure&) = delete;
    au::flow::detail::Closure& operator=(const au::flow::detail::Closure&) = delete;
    Closure(au::flow::detail::Closure&& other) noexcept
        : Closure(other.mInvoke, other.mDestroy, std::exchange(other.mPayload, nullptr)) {}
    au::flow::detail::Closure& operator=(au::flow::detail::Closure&& other) noexcept
    {
        if (this != &other) {
            clear(); mInvoke = other.mInvoke; mDestroy = other.mDestroy;
            mPayload = std::exchange(other.mPayload, nullptr);
        }
        return *this;
    }
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
    Node(au::flow::detail::Core* owner, std::uint64_t id, au::flow::detail::Kind kind)
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
    au::flow::detail::Edge* mDynamicChildren = nullptr; // Diagnostic edges only, protected for prepend by Core::mMutex.
    bool mGraphCaptured = false; // Immutable after acceptance; tracing opt-in is inherited by structured descendants.
    std::atomic<std::size_t> mGraphDropped{0};
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
    au_flow_result mScoped{};
    bool mSkipped = false;
    au_flow_result mResult{};
    std::mutex mWaitMutex;
    std::condition_variable mChanged;
    std::size_t mCooperativeWaiters = 0; // Protected by mWaitMutex; finish signals the unified gate.
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
    bool mCpuActive = false; // Logical callable slice, suspended at known scheduler/device waits.
};
struct ExternalToken {
    au::flow::detail::Core* mCore = nullptr;
    au::flow::detail::NodeRef mParent;
    au::flow::detail::ExternalOps mOps;
    void* mPayload = nullptr;
    std::uint64_t mOrder = 0;
    std::atomic<unsigned int> mNotification{0}; // 0 pending, 1 publishing, 2 terminal status published.
    std::atomic<bool> mRegistrar{true};
    std::atomic<bool> mQueued{false};
    au::flow::detail::ExternalStatus mStatus;
    au::flow::detail::ReadyLink mLink;
    au::flow::detail::ExternalToken* mPendingNext = nullptr;
};
// One invocation, owned by the thread currently executing a tile. The loop
// gathers these records only after every participant has joined.
struct TileContext {
    au::flow::detail::ExternalStatus mError;
    std::atomic<bool>* mStop;
    bool mException = false;
    void record(int code, au::flow::ErrorDomain domain, int native, std::uint64_t cause) noexcept
    {
        if (!code) return;
        if (!mError.mCode) mError = {code, native, domain, cause};
        mStop->store(true, std::memory_order_release); // Stop new tickets, not callbacks already in flight.
    }
};
// Definitions live in this translation unit, never GNU-unique public inline TLS.
thread_local au::flow::detail::Frame* gFrame = nullptr;
thread_local int gWorker = -1;
thread_local unsigned int gDepth = 0;
thread_local unsigned int gTileDepth = 0;
thread_local au::flow::detail::TileContext* gTileContext = nullptr;
thread_local unsigned int gCleanupDepth = 0;
thread_local au::flow::detail::Node* gFinish = nullptr;
thread_local bool gFinishing = false;
thread_local au::flow::detail::Node* gReclaim = nullptr;
thread_local bool gReclaiming = false;
thread_local unsigned int gCompletionBurst = 0;

// Cooperative work and cleanup belong to their own task, never to a suspended
// tile on this thread. Restore after their entire dispatch/cleanup has ended.
class TileSuspension {
public:
    TileSuspension() noexcept : mContext(au::flow::detail::gTileContext), mDepth(au::flow::detail::gTileDepth)
    { au::flow::detail::gTileContext = nullptr; au::flow::detail::gTileDepth = 0; }
    ~TileSuspension() noexcept
    { au::flow::detail::gTileContext = mContext; au::flow::detail::gTileDepth = mDepth; }
    TileSuspension(const TileSuspension&) = delete;
    TileSuspension& operator=(const TileSuspension&) = delete;
private:
    au::flow::detail::TileContext* mContext;
    unsigned int mDepth;
};

class TileExtent {
public:
    TileExtent(au::flow::detail::TileContext* context, const au::mm::detail::AccessErrorSink& sink) noexcept
        : mPrevious(au::flow::detail::gTileContext), mDepth(au::flow::detail::gTileDepth),
          mSink(au::mm::detail::setAccessErrorSink(sink))
    { au::flow::detail::gTileContext = context; ++au::flow::detail::gTileDepth; }
    ~TileExtent() noexcept
    {
        au::mm::detail::setAccessErrorSink(mSink);
        au::flow::detail::gTileContext = mPrevious;
        au::flow::detail::gTileDepth = mDepth;
    }
    TileExtent(const TileExtent&) = delete;
    TileExtent& operator=(const TileExtent&) = delete;
private:
    au::flow::detail::TileContext* mPrevious;
    unsigned int mDepth;
    au::mm::detail::AccessErrorSink mSink;
};

#ifdef AURA_FLOW_TESTING
std::array<std::atomic<int>, 5> gFailure{{-1, -1, -1, -1, -1}};
std::atomic<std::size_t> gOutstandingLimit{SIZE_MAX};
std::atomic<au::flow::testing::SnapshotHook*> gSnapshotHook{nullptr};
std::mutex gRuntimeHookMutex;
std::condition_variable gRuntimeHookChanged;
au::flow::testing::RuntimeHook* gRuntimeHook = nullptr;
std::size_t gRuntimeReaders = 0;
void runtimePoint(au::flow::testing::RuntimePoint point, std::size_t id) noexcept
{
    au::flow::testing::RuntimeHook* hook;
    {
        std::lock_guard<std::mutex> lock(au::flow::detail::gRuntimeHookMutex);
        hook = au::flow::detail::gRuntimeHook;
        if (!hook) return;
        ++au::flow::detail::gRuntimeReaders;
    }
    hook->mInvoke(hook->mPayload, point, id); // No scheduler or seam lock held.
    {
        std::lock_guard<std::mutex> lock(au::flow::detail::gRuntimeHookMutex);
        --au::flow::detail::gRuntimeReaders;
    }
    au::flow::detail::gRuntimeHookChanged.notify_all();
}

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

bool reserveOutstanding(au::flow::detail::Node* node) noexcept
{
    std::size_t limit = SIZE_MAX;
#ifdef AURA_FLOW_TESTING
    limit = au::flow::detail::gOutstandingLimit.load(std::memory_order_relaxed); // Fault-model ceiling only.
#endif
    std::size_t current = node->mOutstanding.load(std::memory_order_relaxed);
    do {
        if (current >= limit) return false;
    } while (!node->mOutstanding.compare_exchange_weak(current, current + 1, std::memory_order_relaxed));
    // Reservation only: the accepting body keeps the parent alive. Last release
    // is acq_rel and gathers each child's/event's independently published writes.
    return true;
}
bool finalState(au::flow::TaskState state) noexcept
{ return state >= au::flow::TaskState::Succeeded; }

namespace {
// Internal linkage also localizes libstdc++'s generated thread-state vtable.
// A member lambda can leave that vtable preemptible across RTLD_GLOBAL plugins
// even when the worker function itself has hidden visibility.
struct WorkerEntry {
    au::flow::detail::Core* mCore;
    std::size_t mId;
    void operator()() const noexcept;
};
}

class AU_LOCAL Core {
public:
    au::flow::detail::PayloadPool& payloads() noexcept { return mPayloads; }
    au::mm::detail::AccessErrorSink memorySink(bool mayWait = true) noexcept
    {
        return {[](void* payload, int code, int native, std::uint64_t cause, bool device) noexcept {
            static_cast<au::flow::detail::Core*>(payload)->scopedError(code,
                device ? au::flow::ErrorDomain::OpenCL : au::flow::ErrorDomain::Memory, native, cause);
        }, this, mayWait};
    }
    class CleanupExtent {
    public:
        CleanupExtent(au::flow::detail::Core* core, au::flow::detail::Node* node) noexcept
            : mFrame{node, au::flow::detail::gFrame}, mSink(au::mm::detail::setAccessErrorSink(core->memorySink(false)))
        { au::flow::detail::gFrame = node ? &mFrame : nullptr; ++au::flow::detail::gCleanupDepth; }
        ~CleanupExtent() noexcept
        {
            --au::flow::detail::gCleanupDepth; au::flow::detail::gFrame = mFrame.mPrevious;
            au::mm::detail::setAccessErrorSink(mSink);
        }
        CleanupExtent(const CleanupExtent&) = delete;
        CleanupExtent& operator=(const CleanupExtent&) = delete;
    private:
        au::flow::detail::TileSuspension mTileSuspension;
        au::flow::detail::Frame mFrame;
        au::mm::detail::AccessErrorSink mSink;
    };
    Core() noexcept
    {
        // Construct mm first so structured cleanup during static shutdown never
        // discovers an already destroyed owner registry. No workers are created.
        (void)au::mm::Registry::instance();
        if (au::sys::flow::allowedCpus(mAllowed) != 0) {
            // A query failure must not turn a usable CPU scheduler into an error.
            mBudget.store(1, std::memory_order_relaxed); // Not shared during construction.
        } else {
            mBudget.store(std::min<std::size_t>(8, mAllowed.size()), std::memory_order_relaxed);
        }
    }
    ~Core() noexcept { (void)shutdown(); }
    au::flow::detail::NodeRef make(au::flow::detail::Kind kind)
    {
#ifdef AURA_FLOW_TESTING
        if (au::flow::detail::fail(au::flow::testing::Failure::Node)) throw std::bad_alloc();
#endif
        std::uint64_t id = mNextId.load(std::memory_order_relaxed); // Identity only.
        do {
            if (id == UINT64_MAX) throw au::flow::detail::CreateFailure{au::err::kErrorOverflow};
        } while (!mNextId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)); // Saturate; never wrap to a stale ID.
        auto node = au::flow::detail::NodeRef::adopt(mNodes.create(this, id, kind));
        node->mName = std::to_string(id); // Cold label creation; the owner also handles construction rollback.
        return node;
    }
    void recycle(au::flow::detail::Node* node) noexcept
    {
        while (node->mDynamicChildren) {
            auto* edge = node->mDynamicChildren; node->mDynamicChildren = edge->mNext; mEdges.destroy(edge);
        }
        mNodes.destroy(node); // Iterative NodeRef reclamation also handles retained diagnostic descendants.
    }
    bool owns(const au::flow::detail::Node* node) const noexcept { return node && node->mOwner == this; }
    int builderError(au::flow::detail::Node* node, int code) noexcept
    {
        if (owns(node) && node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft && !node->mBuildError)
            node->mBuildError = code;
        return code; // The operation reports its own error; submission preserves the first build failure.
    }
    void scopedError(int code, au::flow::ErrorDomain domain = au::flow::ErrorDomain::Flow, int native = 0,
                     std::uint64_t cause = 0) noexcept
    {
        if (au::flow::detail::gTileContext) {
            au::flow::detail::gTileContext->record(code, domain, native, cause);
            return;
        }
        if (!code || !au::flow::detail::gFrame) return;
        au::flow::detail::Node* node = au::flow::detail::gFrame->mNode;
        std::lock_guard<std::mutex> lock(node->mErrorMutex);
        if (!node->mScoped.mCode) {
            node->mScoped = result(node, code, domain); node->mScoped.mNativeCode = native;
            if (cause) node->mScoped.mCauseTaskId = cause;
        }
    }
    int prepareExternal(const au::flow::detail::ExternalOps& ops, void* payload,
                        au::flow::detail::ExternalToken** out) noexcept
    {
        if (!out || !ops.mRelease) return au::err::kErrorInvalidParam;
        *out = nullptr;
        if (au::flow::detail::gTileContext) {
            scopedError(au::err::kErrorStateInvalid);
            return au::err::kErrorStateInvalid;
        }
        if (au::flow::detail::gCleanupDepth) return au::err::kErrorStateInvalid;
        if (!au::flow::detail::gFrame) return 0;
        au::flow::detail::ExternalToken* token = nullptr;
        try {
            token = mExternal.create();
            token->mCore = this;
            token->mParent = au::flow::detail::NodeRef(au::flow::detail::gFrame->mNode);
            token->mOps = ops; token->mPayload = payload; token->mLink.mValue = token;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mOrder == UINT64_MAX || !au::flow::detail::reserveOutstanding(token->mParent.get())) {
                    mExternal.destroy(token); return au::err::kErrorOverflow;
                }
                token->mOrder = ++mOrder;
            }
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
        au::flow::detail::Core::CleanupExtent extent(this, token->mParent.get());
        if (token->mNotification.load(std::memory_order_acquire) != 2 && token->mOps.mPoll) {
            bool done = false;
            au::flow::detail::ExternalStatus status;
            const int query = token->mOps.mPoll(token->mPayload, &done, &status);
            if (query) {
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(token->mParent->mErrorMutex);
                    if (!token->mParent->mScoped.mCode) {
                        token->mParent->mScoped = result(token->mParent.get(), query, status.mDomain == au::flow::ErrorDomain::None
                            ? au::flow::ErrorDomain::OpenCL : status.mDomain);
                        token->mParent->mScoped.mNativeCode = status.mNativeCode;
                        if (status.mCauseTaskId) token->mParent->mScoped.mCauseTaskId = status.mCauseTaskId;
                        first = true;
                    }
                }
                if (first) XLOG_E("au::flow external query failed task=%llu code=%d native=%d\n",
                    static_cast<unsigned long long>(token->mParent->mId), query, status.mNativeCode);
            }
            if (done) notifyExternal(token, status);
        }
        if (token->mNotification.load(std::memory_order_acquire) != 2) return false;
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
            if (status.mCauseTaskId) failure.mCauseTaskId = status.mCauseTaskId;
            inherit(parent.get(), failure, token->mOrder);
        }
        mExternal.destroy(token);
        if (parent->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(parent.get()); // Release event reservation last.
        return true;
    }
    void deferExternal(au::flow::detail::ExternalToken* token, bool announce = true) noexcept
    {
        std::lock_guard<std::mutex> lock(mExternalMutex);
        token->mPendingNext = nullptr;
        if (mPendingExternalTail) mPendingExternalTail->mPendingNext = token;
        else mPendingExternal = token;
        mPendingExternalTail = token;
        const auto previous = mPendingExternalCount.fetch_add(1, std::memory_order_release);
        if (announce && !previous) mWork.control(); // First query claimant must wake an indefinitely sleeping worker.
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
        if (!consumeExternal(token)) { deferExternal(token, false); return false; }
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
        if (!owns(before) || !owns(after)) return builderError(after, au::err::kErrorInvalidHandle);
        if (after->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
            return au::err::kErrorStateInvalid;
        try {
            if (reaches(before, after, false)) return builderError(after, au::flow::kErrorCycleDetected);
            for (const auto& pred : after->mPreds) if (pred.get() == before) return 0;
            after->mPreds.emplace_back(before);
            return 0;
        } catch (...) { return builderError(after, au::err::kErrorNoMemory); }
    }
    int add(au::flow::detail::Node* group, au::flow::detail::Node* member) noexcept
    {
        if (!owns(group) || !owns(member)) return builderError(group, au::err::kErrorInvalidHandle);
        if (group->mKind != au::flow::detail::Kind::Group) return au::err::kErrorInvalidParam;
        if (group->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft)
            return au::err::kErrorStateInvalid;
        if (member->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || member->mGroup)
            return builderError(group, au::err::kErrorStateInvalid);
        try {
            if (reaches(member, group, false)) return builderError(group, au::flow::kErrorCycleDetected);
            group->mMembers.emplace_back(member); member->mGroup = group;
            return 0;
        } catch (...) { return builderError(group, au::err::kErrorNoMemory); }
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

    class WorkerAdmission {
    public:
        explicit WorkerAdmission(au::flow::detail::Core* core) noexcept : mCore(core) {}
        int accept(int* native = nullptr) noexcept
        {
            std::unique_lock<std::mutex> lock(mCore->mWorkerMutex);
            if (mCore->mClosingQueries) return au::err::kErrorStateInvalid;
            if (mCore->mAdmissions == SIZE_MAX) return au::err::kErrorOverflow;
            ++mCore->mAdmissions;
            mAccepted = true;
            const int code = mCore->eligibleWorkers() ? 0 : mCore->startWorker(lock);
            if (native) *native = code ? mCore->mLastWorkerNative : 0;
            lock.unlock();
#ifdef AURA_FLOW_TESTING
            if (!code) au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::AfterAdmission,
                au::flow::detail::gWorker < 0 ? 8 : static_cast<std::size_t>(au::flow::detail::gWorker));
#endif
            return code;
        }
        ~WorkerAdmission() noexcept
        {
            if (!mAccepted) return;
            bool wake;
            {
                std::lock_guard<std::mutex> lock(mCore->mWorkerMutex);
                --mCore->mAdmissions;
                wake = mCore->mRetirePending.load(std::memory_order_acquire);
            }
            mCore->mPolicyChanged.notify_all();
            if (wake) mCore->mWork.control();
        }
        WorkerAdmission(const WorkerAdmission&) = delete;
        WorkerAdmission& operator=(const WorkerAdmission&) = delete;
    private:
        au::flow::detail::Core* mCore;
        bool mAccepted = false;
    };
    int submit(au::flow::detail::Node* top, bool reportFailure = true) noexcept
    {
        int ret = 0;
        int native = 0;
        if (au::flow::detail::gTileContext) ret = au::err::kErrorStateInvalid;
        else if (au::flow::detail::gCleanupDepth) ret = au::flow::kErrorWouldDeadlock;
        else if (!owns(top)) ret = au::err::kErrorInvalidHandle;
        else if (top->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft || top->mGroup)
            ret = au::err::kErrorStateInvalid;
        if (ret) { if (reportFailure) scopedError(ret); return ret; }
        WorkerAdmission admission(this);
        std::vector<Prepared> batch;
        std::vector<Registration> registrations;
        std::unique_ptr<au::flow::detail::Edge, EdgeOwner> diagnostic(nullptr, EdgeOwner{this});
        const auto* dynamicParent = au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode : nullptr;
        const bool captureGraph = mDiagnostics.enabled() || (dynamicParent && dynamicParent->mGraphCaptured);
        bool diagnosticDropped = false;
        if (captureGraph && dynamicParent) {
            try { diagnostic.reset(mEdges.create()); diagnostic->mTarget = au::flow::detail::NodeRef(top); }
            catch (...) { diagnosticDropped = true; } // Losing diagnostics cannot reject work.
        }
        try {
            batch.push_back({top, au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode : nullptr, nullptr, {}});
            for (std::size_t i = 0; i < batch.size(); ++i) {
                au::flow::detail::Node* node = batch[i].mNode;
                if (node->mBuildError) { ret = node->mBuildError; break; }
                if (node->mState.load(std::memory_order_acquire) != au::flow::TaskState::Draft) {
                    ret = au::err::kErrorStateInvalid; break;
                }
                if (node->mKind == au::flow::detail::Kind::Group) {
                    if (node->mMembers.size() == SIZE_MAX) { ret = au::err::kErrorOverflow; break; }
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
                ret = admission.accept(&native);
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
                else if (mOrder > UINT64_MAX - batch.size() || (!batch[0].mParent && mRootOrder == UINT64_MAX) ||
                         mAccepted > SIZE_MAX - batch.size())
                    ret = au::err::kErrorOverflow;
#ifdef AURA_FLOW_TESTING
                else if (au::flow::detail::fail(au::flow::testing::Failure::Order)) ret = au::err::kErrorOverflow;
#endif
                if (!ret && batch[0].mParent && !au::flow::detail::reserveOutstanding(batch[0].mParent))
                    ret = au::err::kErrorOverflow;
                if (!ret) {
                    const std::uint64_t rootOrder = batch[0].mParent ? 0 : ++mRootOrder;
                    if (diagnostic) {
                        diagnostic->mNext = batch[0].mParent->mDynamicChildren;
                        batch[0].mParent->mDynamicChildren = diagnostic.release();
                    }
                    if (diagnosticDropped) batch[0].mParent->mGraphDropped.fetch_add(1, std::memory_order_relaxed); // Diagnostic only.

                    for (const Prepared& p : batch) {
                        au::flow::detail::Node* node = p.mNode;
                        node->mOrder = ++mOrder; node->mGraphCaptured = captureGraph;
                        node->mRootOrder = node == top ? rootOrder : 0;
                        node->mParent = au::flow::detail::NodeRef(p.mParent);
                        node->mGate = au::flow::detail::NodeRef(p.mGate);
                        node->mUnresolved.store(p.mPreds.size(), std::memory_order_relaxed); // Before commit release.
                        node->mOutstanding.store(node->mKind == Kind::Group ? node->mMembers.size() + 1 : 1,
                                                 std::memory_order_relaxed); // Children + entry, or body reservation.
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
            reject(top, ret, native);
            if (reportFailure) {
                scopedError(ret);
                XLOG_E("au::flow submission rejected task=%llu code=%d\n", static_cast<unsigned long long>(top->mId), ret);
            }
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
        const bool inheritedSkipped = target->mInherited.mState == static_cast<std::uint32_t>(au::flow::TaskState::Skipped);
        if (!target->mInherited.mCode || (inheritedSkipped && !skipped) ||
            (inheritedSkipped == skipped && order < target->mInheritedOrder)) {
            target->mInherited = source; target->mInheritedOrder = order;
        }
    }
    void resolve(au::flow::detail::Node* target, const au_flow_result& source, std::uint64_t order,
                 au::flow::detail::Node** continuation = nullptr) noexcept
    {
        inherit(target, source, order);
        if (target->mUnresolved.fetch_sub(1, std::memory_order_acq_rel) == 1)
            activate(target, continuation); // Gather predecessor writes.
    }
    void publishReady(au::flow::detail::Node* node) noexcept
    {
        mReadyCount.fetch_add(1, std::memory_order_relaxed); // Scaling hint only.
        mReady.push(&node->mReady);
        mWork.post();
        grow();
    }
    void activate(au::flow::detail::Node* node, au::flow::detail::Node** continuation = nullptr) noexcept
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
        trace(node, "ready", 'i');
        // Only the dispatching worker may retain one immediate successor. No
        // permit was posted for it, and the accepted reference keeps it alive.
        // Older queued work and deferred device polls take precedence; the outer
        // dispatch loop additionally bounds consecutive handoffs to eight.
        if (continuation && !*continuation && node->mKind == Kind::Work && !node->mContinue &&
            mReadyCount.load(std::memory_order_relaxed) == 0 &&
            mPendingExternalCount.load(std::memory_order_acquire) == 0) {
            *continuation = node;
            return;
        }
        publishReady(node);
    }
    void bodyEnd(au::flow::detail::Node* node, int code,
                 au::flow::detail::Node** continuation = nullptr) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(node->mErrorMutex);
            node->mBodyCode = code;
            if (code != au::flow::kErrorUnhandledException) node->mBodyDomain = au::flow::ErrorDomain::Callable;
        }
        node->mState.store(au::flow::TaskState::Waiting, std::memory_order_release); // Body cannot run again.
        if (node->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finish(node, continuation); // Gather child/event writes.
    }
    void finish(au::flow::detail::Node* node, au::flow::detail::Node** continuation = nullptr) noexcept
    {
        node->mFinishNext = au::flow::detail::gFinish; au::flow::detail::gFinish = node;
        if (au::flow::detail::gFinishing) return;
        au::flow::detail::gFinishing = true;
        while (au::flow::detail::gFinish) {
            node = au::flow::detail::gFinish; au::flow::detail::gFinish = node->mFinishNext;
            {
                au::flow::detail::Core::CleanupExtent extent(this, node);
                node->mClosure.clear(); // No locks; cleanup cannot extend a closed body reservation.
            }
            {
                std::lock_guard<std::mutex> lock(node->mErrorMutex);
                node->mResult = node->mBodyCode ? result(node, node->mBodyCode, node->mBodyDomain)
                    : node->mScoped.mCode ? node->mScoped : node->mInherited.mCode ? node->mInherited
                    : result(node, 0, au::flow::ErrorDomain::None);
                // Preserve the callable's chosen int. When it forwards the
                // same reported failure, retain native provenance instead of
                // erasing it merely because it crossed an int-returning API.
                if (node->mBodyCode && node->mBodyDomain == au::flow::ErrorDomain::Callable) {
                    if (node->mScoped.mCode == node->mBodyCode) node->mResult = node->mScoped;
                    else if (node->mInherited.mCode == node->mBodyCode && node->mInherited.mCauseTaskId == node->mId)
                        node->mResult = node->mInherited;
                }
                node->mResult.mState = static_cast<std::uint32_t>(node->mSkipped ? au::flow::TaskState::Skipped
                    : node->mResult.mCode ? au::flow::TaskState::Failed : au::flow::TaskState::Succeeded);
            }
            trace(node, "Task", 'e', node->mResult.mCode);
            bool cooperative;
            {
                std::lock_guard<std::mutex> lock(node->mWaitMutex);
                node->mState.store(static_cast<au::flow::TaskState>(node->mResult.mState), std::memory_order_release);
                cooperative = node->mCooperativeWaiters != 0;
            }
            node->mChanged.notify_all();
            if (cooperative) mWork.control();
            au::flow::detail::Edge* edge = node->mSuccessors.exchange(au::flow::detail::closedEdges(),
                std::memory_order_acq_rel); // Release final result; acquire complete edge list.
            au::flow::detail::Node** single = edge && !edge->mNext ? continuation : nullptr;
            while (edge) {
                au::flow::detail::Edge* next = edge->mNext;
                resolve(edge->mTarget.get(), node->mResult, node->mOrder, single); mEdges.destroy(edge); edge = next;
            }
            au::flow::detail::NodeRef parent;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                parent = std::move(node->mParent);
                if (node->mActivePrev) node->mActivePrev->mActiveNext = node->mActiveNext;
                else mActive = node->mActiveNext;
                if (node->mActiveNext) node->mActiveNext->mActivePrev = node->mActivePrev;
                --mAccepted;
                if (!mAccepted && mRetirePending.load(std::memory_order_acquire)) mWork.control();
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

    void notifyExternalWaiters() noexcept { mWork.control(); }
    void releasePoller() noexcept
    {
        int owner = au::flow::detail::gWorker;
        // Most CPU dispatches have no lease. Avoid compare-exchange traffic
        // on a shared cache line, and take the worker lock only for a handoff.
        if (owner < 0 || mPollOwner.load(std::memory_order_acquire) != owner) return;
        bool released;
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            released = mPollOwner.compare_exchange_strong(owner, -1, std::memory_order_acq_rel);
        }
        if (released) mWork.control(); // Retirement and relinquishing the last query consumer are serialized.
    }
    bool pumpExternal(std::chrono::steady_clock::time_point& deadline) noexcept
    {
        const int id = au::flow::detail::gWorker;
        if (!mPendingExternalCount.load(std::memory_order_acquire)) { releasePoller(); return false; }
        if (id < 0 || static_cast<std::size_t>(id) >= mBudget.load(std::memory_order_acquire)) return false;
        int unclaimed = -1;
        auto& worker = mWorkers[static_cast<std::size_t>(id)];
        if (mPollOwner.load(std::memory_order_acquire) == -1 &&
            mPollOwner.compare_exchange_strong(unclaimed, id, std::memory_order_acq_rel)) {
            worker.mPollDue = std::chrono::steady_clock::now();
            worker.mPollDelay = kPollMinimum;
            worker.mPollRemaining = 0;
        }
        if (mPollOwner.load(std::memory_order_acquire) != id) return false;
        bool progress = false;
        if (std::chrono::steady_clock::now() >= worker.mPollDue &&
            !mPolling.test_and_set(std::memory_order_acquire)) {
            if (!worker.mPollRemaining) {
                worker.mPollRemaining = mPendingExternalCount.load(std::memory_order_acquire);
                worker.mPollProgress = false;
            }
#ifdef AURA_FLOW_TESTING
            mPollPasses.fetch_add(1, std::memory_order_relaxed);
            au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::BeforePendingQuery, static_cast<std::size_t>(id));
#endif
            const auto attempts = std::min<std::size_t>(kPollBatch, worker.mPollRemaining);
            for (std::size_t i = 0; i < attempts; ++i) progress = pollPendingExternal() || progress;
            worker.mPollRemaining -= attempts;
            worker.mPollProgress = worker.mPollProgress || progress;
            if (!worker.mPollRemaining) {
                worker.mPollDelay = worker.mPollProgress ? kPollMinimum : std::min(kPollMaximum, worker.mPollDelay * 2);
                worker.mPollDue = std::chrono::steady_clock::now() + worker.mPollDelay;
            } else worker.mPollDue = std::chrono::steady_clock::now(); // Finish a round fairly, eight queries per dispatch.
            mPolling.clear(std::memory_order_release);
        }
        if (!mPendingExternalCount.load(std::memory_order_acquire)) releasePoller();
        else deadline = std::min(deadline, worker.mPollDue);
        return progress;
    }
    bool helpOne(bool blocking, std::chrono::steady_clock::time_point deadline,
                 au::flow::detail::Node* awaited = nullptr, const std::uint64_t* observed = nullptr,
                 bool (*externalReady)(void*) noexcept = nullptr, void* externalPayload = nullptr) noexcept
    {
        if (au::flow::detail::gDepth >= 32) return false;
        const auto epoch = observed ? *observed : mWork.epoch();
        if (mStop.load(std::memory_order_acquire)) return false;
        if (au::flow::detail::gWorker >= 0) applyPolicy(static_cast<std::size_t>(au::flow::detail::gWorker));
        const bool eligible = au::flow::detail::gWorker < 0 ||
            static_cast<std::size_t>(au::flow::detail::gWorker) < mBudget.load(std::memory_order_acquire);
        if (eligible && (au::flow::detail::gCompletionBurst < 8 || mReadyCount.load(std::memory_order_relaxed) == 0) &&
            pumpExternal(deadline)) {
            ++au::flow::detail::gCompletionBurst;
            return true;
        }
        if (awaited && au::flow::detail::finalState(awaited->mState.load(std::memory_order_acquire))) return false;
        if (externalReady && externalReady(externalPayload)) return false;
        if (!eligible) releasePoller();
        if (blocking) {
#ifdef AURA_FLOW_TESTING
            au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::BeforeSleep,
                au::flow::detail::gWorker < 0 ? 8 : static_cast<std::size_t>(au::flow::detail::gWorker));
#endif
            const auto wake = mWork.acquireUntil(epoch, deadline, eligible);
#ifdef AURA_FLOW_TESTING
            if (wake == au::sys::flow::WorkSemaphore::Wake::Timeout) mWaitTimeouts.fetch_add(1, std::memory_order_relaxed);
            else if (wake == au::sys::flow::WorkSemaphore::Wake::Control) mControlWakes.fetch_add(1, std::memory_order_relaxed);
            else mWorkWakes.fetch_add(1, std::memory_order_relaxed);
#endif
            if (wake != au::sys::flow::WorkSemaphore::Wake::Work) return false;
        } else if (!eligible || !mWork.tryAcquire()) return false;
        if (au::flow::detail::gWorker >= 0 &&
            static_cast<std::size_t>(au::flow::detail::gWorker) >= mBudget.load(std::memory_order_acquire)) {
            mWork.post(); // Return an acquired real permit if the budget changed.
            return false;
        }
        releasePoller(); // Dispatch may run a long callable; transfer timer ownership first.
        au::flow::detail::ReadyLink* link = nullptr;
        bool overflowFirst = false;
        bool completion = false;
        unsigned int attempts = 0;
        for (;;) {
            if (au::flow::detail::gCompletionBurst < 8 && mCompletions.pop(link, overflowFirst)) { completion = true; break; }
            if (mReady.pop(link, overflowFirst)) break;
            if (mCompletions.pop(link, overflowFirst)) { completion = true; break; }
            if (++attempts == kQueueProbeLimit) {
#ifdef AURA_FLOW_TESTING
                au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::QueueProbeMiss,
                    au::flow::detail::gWorker < 0 ? 8 : static_cast<std::size_t>(au::flow::detail::gWorker));
#endif
                mWork.post(); // Preserve the real permit; recheck target/control/timeout outside this bounded probe.
                return false;
            }
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
        au::flow::detail::Node* node = static_cast<au::flow::detail::Node*>(link->mValue);
        for (unsigned int burst = 0; node; ++burst) {
            au::flow::detail::Node* next = nullptr;
            run(node, au::flow::detail::gWorker >= 0 && burst < 7 ? &next : nullptr);
            node = next;
            if (node) {
                applyPolicy(static_cast<std::size_t>(au::flow::detail::gWorker));
                if (static_cast<std::size_t>(au::flow::detail::gWorker) >= mBudget.load(std::memory_order_acquire)) {
                    publishReady(node); // Budget changed: return this unposted continuation to the queue.
                    break;
                }
            }
        }
        return true;
    }
    void run(au::flow::detail::Node* node, au::flow::detail::Node** continuation = nullptr) noexcept
    {
        au::flow::detail::CpuSlicePause outerSlice;
        au::flow::detail::TileSuspension tileSuspension;
        au::flow::detail::NodeRef pin(node);
        node->mState.store(au::flow::TaskState::Running, std::memory_order_release);
        Frame frame{node, au::flow::detail::gFrame};
        if (frame.mPrevious) {
            std::lock_guard<std::mutex> lock(mMutex);
            frame.mPrevious->mNode->mExecutingChild = node;
        }
        au::flow::detail::gFrame = &frame;
        const auto memorySink = au::mm::detail::setAccessErrorSink(this->memorySink());
        ++au::flow::detail::gDepth;
        // External tiled callers provide extra capacity, not busy pool workers.
        if (au::flow::detail::gWorker >= 0) mBusy.fetch_add(1, std::memory_order_relaxed); // Scaling hint; cooperative stacks may overlap.
        if (mPendingExternalCount.load(std::memory_order_acquire)) grow(); // Preserve a query consumer when dispatch takes its leader.
        int code = 0;
        trace(node, "callable wall", 'B'); // Includes all waits inside the function.
        frame.mCpuActive = true; trace(node, "CPU slice", 'B'); // Excludes known waits; not OS scheduled CPU time.
        if (node->mClosure.mInvoke) {
            try {
                if (mDiagnostics.enabled()) {
                    au::perf::XPerfScoped scope(node->mName);
                    code = node->mClosure.mInvoke(node->mClosure.mPayload);
                } else code = node->mClosure.mInvoke(node->mClosure.mPayload);
            } catch (...) {
                code = au::flow::kErrorUnhandledException;
                std::lock_guard<std::mutex> lock(node->mErrorMutex);
                node->mBodyDomain = au::flow::ErrorDomain::Flow;
            }
        }
        if (frame.mCpuActive) { trace(node, "CPU slice", 'E', code); frame.mCpuActive = false; }
        trace(node, "callable wall", 'E', code);
        au::mm::detail::setAccessErrorSink(memorySink);
        if (au::flow::detail::gWorker >= 0) mBusy.fetch_sub(1, std::memory_order_relaxed);
        --au::flow::detail::gDepth;
        au::flow::detail::gFrame = frame.mPrevious;
        if (node->mContinue && node->mContinue(node->mClosure.mPayload)) {
            node->mState.store(au::flow::TaskState::Ready, std::memory_order_release);
            mReadyCount.fetch_add(1, std::memory_order_relaxed); // Helper yields after a bounded tile batch.
            mReady.push(&node->mReady); mWork.post();
            grow();
            if (frame.mPrevious) {
                std::lock_guard<std::mutex> lock(mMutex);
                frame.mPrevious->mNode->mExecutingChild = nullptr;
            }
            return;
        }
        bodyEnd(node, code, continuation);
        if (frame.mPrevious) {
            std::lock_guard<std::mutex> lock(mMutex);
            frame.mPrevious->mNode->mExecutingChild = nullptr;
        }
    }

    void leaveCooperativeWait() noexcept
    {
        if (!au::flow::detail::gFrame || au::flow::detail::gWorker < 0 || au::flow::detail::gCleanupDepth) return;
        releasePoller(); // The callable resumes here; it must not retain an idle query lease.
        if (mPendingExternalCount.load(std::memory_order_acquire)) grow();
    }
    class CooperativeWait {
    public:
        CooperativeWait(au::flow::detail::Core* core, au::flow::detail::Node* node) noexcept
            : mCore(core), mNode(au::flow::detail::gFrame && !au::flow::detail::gCleanupDepth ? node : nullptr)
        {
            if (mNode) { std::lock_guard<std::mutex> lock(mNode->mWaitMutex); ++mNode->mCooperativeWaiters; }
        }
        ~CooperativeWait() noexcept
        {
            if (!mNode) return;
            { std::lock_guard<std::mutex> lock(mNode->mWaitMutex); --mNode->mCooperativeWaiters; }
            mCore->leaveCooperativeWait();
        }
        CooperativeWait(const CooperativeWait&) = delete;
        CooperativeWait& operator=(const CooperativeWait&) = delete;
    private:
        au::flow::detail::Core* mCore;
        au::flow::detail::Node* mNode;
    };

    int wait(au::flow::detail::Node* node, std::uint64_t timeout, au_flow_result* out) noexcept
    {
        au::flow::detail::CpuSlicePause waiting;
        if (!owns(node)) return au::err::kErrorInvalidHandle;
        au::flow::detail::NodeRef pin(node);
        if (node->mState.load(std::memory_order_acquire) == au::flow::TaskState::Draft) return au::err::kErrorNotReady;
        if (out && out->mStructSize != sizeof(au_flow_result)) return au::err::kErrorVersionMismatch;
        if (au::flow::detail::gCleanupDepth && !au::flow::detail::finalState(node->mState.load(std::memory_order_acquire)))
            return au::flow::kErrorWouldDeadlock;
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
        CooperativeWait registration(this, node);
        int ret = 0;
        while (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) {
            if (std::chrono::steady_clock::now() >= deadline) { ret = au::err::kErrorTimeout; break; }
            if (current) { (void)helpOne(true, deadline, node); continue; }
            std::unique_lock<std::mutex> lock(node->mWaitMutex);
            if (deadline == std::chrono::steady_clock::time_point::max()) {
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
        if (au::flow::detail::gFrame || au::flow::detail::gTileContext || au::flow::detail::gCleanupDepth)
            return au::flow::kErrorWouldDeadlock;
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
        if (au::flow::detail::gFrame || au::flow::detail::gTileContext || au::flow::detail::gCleanupDepth)
            return au::flow::kErrorWouldDeadlock;
        std::lock_guard<std::mutex> closing(mCloseMutex);
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mPhase == Phase::Closed) return 0;
            mPhase = Phase::Stopping;
            mDrained.wait(lock, [this] { return mAccepted == 0 && mJobs == 0; });
        }
        {
            std::unique_lock<std::mutex> lock(mWorkerMutex);
            mClosingQueries = true;
            mPolicyChanged.wait(lock, [this] { return mAffinityQueries == 0 && mAdmissions == 0 && mJoiners == 0; });
            mStop.store(true, std::memory_order_release);
        }
        mWork.control(); // Control wakes never manufacture queue permits.
        for (auto& worker : mWorkers) if (worker.mThread.joinable()) worker.mThread.join();
        { std::lock_guard<std::mutex> lock(mMutex); mPhase = Phase::Closed; }
        return 0;
    }

    std::size_t eligibleWorkers() const noexcept
    {
        std::size_t count = 0;
        const std::size_t budget = mBudget.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < budget; ++i) if (mWorkers[i].mState == WorkerState::Running) ++count;
        return count; // mWorkerMutex held.
    }
    std::size_t workDemand() const noexcept
    {
        return std::min(mBudget.load(std::memory_order_acquire),
            mReadyCount.load(std::memory_order_relaxed) + mBusy.load(std::memory_order_relaxed) +
            (mPendingExternalCount.load(std::memory_order_acquire) ? 1 : 0));
    }
    void grow(bool force = false) noexcept
    {
        if (!force && mEligibleCount.load(std::memory_order_acquire) >= workDemand()) return;
        std::unique_lock<std::mutex> lock(mWorkerMutex);
        // startWorker can unlock to join; policy and demand may change during
        // that interval. Never spin toward a capacity above the current budget.
        while (!mClosingQueries && eligibleWorkers() < workDemand()) if (startWorker(lock)) break;
    }
    int startWorker(std::unique_lock<std::mutex>& lock) noexcept
    {
        mLastWorkerNative = 0;
        for (;;) {
            if (mClosingQueries) return au::err::kErrorStateInvalid;
            const std::size_t budget = mBudget.load(std::memory_order_acquire);
            bool retiring = false;
            for (std::size_t id = 0; id < budget; ++id) {
                auto& worker = mWorkers[id];
                if (worker.mState == WorkerState::Exited) {
                    std::thread previous = std::move(worker.mThread);
                    worker.mState = WorkerState::Joining;
                    ++mJoiners;
                    lock.unlock();
                    previous.join(); // No graph, worker, gate or provider lock held.
                    lock.lock();
                    --mJoiners;
                    worker.mState = WorkerState::Free;
                    mPolicyChanged.notify_all();
                    break; // Recheck shutdown/budget and all slots after the unlocked join.
                }
                if (worker.mState == WorkerState::Retiring || worker.mState == WorkerState::Joining) {
                    retiring = true;
                    continue;
                }
                if (worker.mState != WorkerState::Free) continue;
#ifdef AURA_FLOW_TESTING
                if (au::flow::detail::fail(au::flow::testing::Failure::Worker)) return au::err::kErrorPlatformAPI;
#endif
                if (worker.mGeneration == UINT64_MAX) return au::err::kErrorOverflow;
                try {
                    worker.mThreadId = 0;
                    worker.mAppliedVersion.store(UINT64_MAX, std::memory_order_relaxed); // New generation, not running yet.
                    worker.mAffinityCode = au::err::kErrorNotReady;
                    worker.mThread = std::thread(au::flow::detail::WorkerEntry{this, id});
                    ++worker.mGeneration;
                    worker.mState = WorkerState::Running;
                    mWorkerCount.fetch_add(1, std::memory_order_release);
                    mEligibleCount.store(eligibleWorkers(), std::memory_order_release); // Enqueue-side capacity hint.
                    return 0;
                } catch (const std::system_error& error) {
                    mLastWorkerNative = error.code().value();
                    return au::err::kErrorPlatformAPI;
                } catch (const std::bad_alloc&) { return au::err::kErrorNoMemory; }
                catch (...) { return au::err::kErrorPlatformAPI; }
            }
            bool reusable = false;
            for (std::size_t id = 0; id < budget; ++id)
                if (mWorkers[id].mState == WorkerState::Free || mWorkers[id].mState == WorkerState::Exited) reusable = true;
            if (reusable) continue;
            if (!retiring) return 0; // Budget already filled by another grow/admission.
            mPolicyChanged.wait(lock); // A retiring/joining slot must finish before reuse.
        }
    }
    void updateRetirementPending() noexcept
    {
        bool pending = false;
        for (const auto& worker : mWorkers) pending = pending || worker.mRetirePending;
        mRetirePending.store(pending, std::memory_order_release);
    }
    bool retire(std::size_t id) noexcept
    {
        std::lock_guard<std::mutex> workers(mWorkerMutex);
        auto& worker = mWorkers[id];
        if (id < mBudget.load(std::memory_order_acquire) && mPendingExternalCount.load(std::memory_order_acquire)) {
            int unclaimed = -1;
            if (mPollOwner.compare_exchange_strong(unclaimed, static_cast<int>(id), std::memory_order_acq_rel)) {
                worker.mPollDue = std::chrono::steady_clock::now();
                worker.mPollDelay = kPollMinimum;
                worker.mPollRemaining = 0;
                return false; // Become the idle query consumer instead of retiring behind a busy worker.
            }
            if (unclaimed == static_cast<int>(id)) return false;
        }
        // Worker -> graph is the only nested order. Admissions never hold the
        // graph mutex while obtaining the worker mutex or joining a thread.
        std::lock_guard<std::mutex> graph(mMutex);
        worker.mRetirePending = true;
        updateRetirementPending(); // Before checking counters, under their writers' mutexes.
        const bool lastConsumer = mWorkerCount.load(std::memory_order_relaxed) == 1 ||
            (id < mBudget.load(std::memory_order_acquire) && eligibleWorkers() == 1);
        if (worker.mPins || (lastConsumer && (mAdmissions || mAccepted))) return false;
        worker.mRetirePending = false;
        worker.mState = WorkerState::Retiring;
        mWorkerCount.fetch_sub(1, std::memory_order_release);
        mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
        updateRetirementPending();
        return true;
    }
    void workerLoop(std::size_t id) noexcept
    {
        au::flow::detail::gWorker = static_cast<int>(id);
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            mWorkers[id].mThreadId = au::sys::flow::threadId();
        }
        auto lastActive = std::chrono::steady_clock::now();
        while (!mStop.load(std::memory_order_acquire)) {
            const auto epoch = mWork.epoch(); // Before checking policy/retirement predicates.
            applyPolicy(id);
            const auto idleDeadline = lastActive + kIdleTimeout;
            auto deadline = idleDeadline;
            if (std::chrono::steady_clock::now() >= idleDeadline) {
                if (retire(id)) {
#ifdef AURA_FLOW_TESTING
                    mRetirements.fetch_add(1, std::memory_order_relaxed);
                    au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::AfterRetire, id);
#endif
                    break;
                }
                deadline = std::chrono::steady_clock::time_point::max(); // Counters/pins signal changes, no periodic retry.
            }
            if (id >= mBudget.load(std::memory_order_acquire)) {
                releasePoller();
                (void)mWork.acquireUntil(epoch, deadline, false);
                continue;
            }
            if (helpOne(true, deadline, nullptr, &epoch)) {
                lastActive = std::chrono::steady_clock::now();
                if (!mRetirePending.load(std::memory_order_acquire)) continue;
                std::lock_guard<std::mutex> lock(mWorkerMutex);
                if (mWorkers[id].mRetirePending) {
                    mWorkers[id].mRetirePending = false;
                    updateRetirementPending();
                }
            }
        }
        releasePoller();
        au::flow::detail::gWorker = -1;
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            auto& worker = mWorkers[id];
            if (worker.mState == WorkerState::Running) mWorkerCount.fetch_sub(1, std::memory_order_release);
            worker.mRetirePending = false;
            worker.mState = WorkerState::Exited; // No Core access after notifying slot waiters.
            mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
            updateRetirementPending();
        }
        mPolicyChanged.notify_all();
    }
    void applyPolicy(std::size_t id) noexcept
    {
        if (mWorkers[id].mAppliedVersion.load(std::memory_order_acquire) ==
            mPolicyVersion.load(std::memory_order_acquire)) return;
        std::vector<std::uint32_t> cpus;
        std::uint64_t version;
        try {
            {
                std::lock_guard<std::mutex> lock(mWorkerMutex);
                version = mPolicyVersion.load(std::memory_order_relaxed);
                if (mWorkers[id].mAppliedVersion.load(std::memory_order_relaxed) == version) return;
                cpus = mPolicy.empty() ? mAllowed : mPolicy;
            }
            const int code = version == 0 || cpus.empty() ? 0 : au::sys::flow::applyAffinity(cpus);
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            mWorkers[id].mAppliedVersion.store(version, std::memory_order_release);
            mWorkers[id].mAffinityCode = code; // Effective affinity is queried on demand, not cached.
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
                std::unique_lock<std::mutex> lock(mWorkerMutex);
                if (mClosingQueries) return au::err::kErrorStateInvalid;
                const auto budget = std::max<std::size_t>(1, std::min<std::size_t>(8,
                    mode ? selected.size() : mAllowed.size()));
                bool consumer = false;
                for (std::size_t id = 0; id < budget; ++id)
                    consumer = consumer || mWorkers[id].mState == WorkerState::Running;
                if (!consumer && mWorkerCount.load(std::memory_order_relaxed)) {
                    const int code = startWorker(lock);
                    if (code) return code; // Do not strand accepted work by narrowing to empty live slots.
                }
                if (mClosingQueries) return au::err::kErrorStateInvalid;
                if (mPolicyVersion.load(std::memory_order_relaxed) == UINT64_MAX) return au::err::kErrorOverflow;
                mPolicy = std::move(selected);
                mPolicyVersion.fetch_add(1, std::memory_order_release); // Publish policy edits before dispatch fast checks.
                mBudget.store(budget, std::memory_order_release);
                mEligibleCount.store(eligibleWorkers(), std::memory_order_release);
            }
            mPolicyChanged.notify_all();
            mWork.control();
            grow(true);
            return 0;
        } catch (...) { return au::err::kErrorNoMemory; }
    }
    int workers(au_flow_worker* out, std::size_t capacity, std::size_t* count) noexcept
    {
        if (!count || (capacity && !out)) return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        *count = 0;
        for (std::size_t id = 0; id < mWorkers.size(); ++id) {
            const auto& worker = mWorkers[id];
            if (worker.mState != WorkerState::Running) continue;
            const auto index = (*count)++;
            if (index >= capacity) continue;
            if (out[index].mStructSize != sizeof(au_flow_worker)) return au::err::kErrorVersionMismatch;
            out[index] = {sizeof(au_flow_worker), static_cast<std::uint32_t>(id), worker.mThreadId,
                mPolicyVersion.load(std::memory_order_relaxed), worker.mAppliedVersion.load(std::memory_order_acquire),
                worker.mAffinityCode};
        }
        return capacity < *count ? au::err::kErrorBufferUnderflow : 0;
    }

    int workerAffinity(std::uint32_t id, std::uint32_t* cpus, std::size_t capacity, std::size_t* count) noexcept
    {
        if (!count || (capacity && !cpus)) return au::err::kErrorInvalidParam;
        std::uint64_t thread;
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            if (id >= mWorkers.size()) return au::err::kErrorOutOfRange;
            if (mClosingQueries) return au::err::kErrorStateInvalid;
            if (mWorkers[id].mState != WorkerState::Running) return au::err::kErrorNotReady;
            thread = mWorkers[id].mThreadId;
            if (!thread) return au::err::kErrorNotReady;
            if (mAffinityQueries == SIZE_MAX || mWorkers[id].mPins == SIZE_MAX) return au::err::kErrorOverflow;
            ++mAffinityQueries;
            ++mWorkers[id].mPins; // Pin this generation until the native query returns.
        }
#ifdef AURA_FLOW_TESTING
        au::flow::detail::runtimePoint(au::flow::testing::RuntimePoint::QueryPinned, id);
#endif
        std::vector<std::uint32_t> effective;
        int code = au::sys::flow::affinitySupported();
        if (!code) code = au::sys::flow::threadAffinity(thread, effective);
        bool wake;
        {
            std::lock_guard<std::mutex> lock(mWorkerMutex);
            --mAffinityQueries;
            --mWorkers[id].mPins;
            wake = mWorkers[id].mRetirePending;
        }
        mPolicyChanged.notify_all();
        if (wake) mWork.control();
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
            au::flow::detail::GraphSnapshot graph;
            graph.mCaptured = top->mGraphCaptured;
            std::vector<NodeRef> todo{NodeRef(top)};
            while (!todo.empty()) {
                NodeRef pin = std::move(todo.back()); todo.pop_back();
                auto* node = pin.get();
                if (!graph.mIndex.emplace(node->mId, graph.mNodes.size()).second) continue;
                au::flow::detail::GraphNodeSnapshot item;
                item.mId = node->mId; item.mName = node->mName; item.mGroup = node->mKind == Kind::Group;
                item.mState = node->mState.load(std::memory_order_acquire);
                if (au::flow::detail::finalState(item.mState)) {
                    item.mCode = node->mResult.mCode; item.mCause = node->mResult.mCauseTaskId;
                }
                Edge* children;
                { std::lock_guard<std::mutex> lock(mMutex); children = node->mDynamicChildren; }
                for (auto* edge = children; edge; edge = edge->mNext) {
                    item.mChildren.push_back(edge->mTarget->mId); todo.emplace_back(edge->mTarget);
                }
                for (const auto& member : node->mMembers) {
                    item.mMembers.push_back(member->mId); todo.emplace_back(member);
                }
                for (const auto& pred : node->mPreds) {
                    item.mPreds.push_back(pred->mId); todo.emplace_back(pred);
                }
                graph.mDropped += node->mGraphDropped.load(std::memory_order_relaxed);
                graph.mNodes.push_back(std::move(item));
            }
            return au::flow::detail::writeDot(graph, path);
        } catch (...) { return au::err::kErrorNoMemory; }
    }
    bool tracingEnabled() const noexcept { return mDiagnostics.enabled(); }
    int tracing(bool enabled) noexcept { return mDiagnostics.setEnabled(enabled); }
    int dumpTrace(const char* path) const noexcept { return mDiagnostics.dumpTrace(path); }
#ifdef AURA_FLOW_TESTING
    int queueHooks(const au::flow::detail::QueueHooks& hooks) noexcept
    {
        std::lock_guard<std::mutex> workers(mWorkerMutex);
        std::lock_guard<std::mutex> graph(mMutex);
        if (mWorkerCount.load(std::memory_order_acquire) || mAdmissions || mAccepted || mJobs)
            return au::err::kErrorBusy;
        mReady.testHooks(hooks); // Private test installation at a quiescent queue only.
        return 0;
    }
    au::flow::testing::RuntimeStats runtimeStats() noexcept
    {
        au::flow::testing::RuntimeStats stats;
        std::lock_guard<std::mutex> lock(mWorkerMutex);
        stats.mLive = mWorkerCount.load(std::memory_order_acquire);
        stats.mPollOwner = mPollOwner.load(std::memory_order_acquire);
        stats.mWaitTimeouts = mWaitTimeouts.load(std::memory_order_relaxed);
        stats.mControlWakes = mControlWakes.load(std::memory_order_relaxed);
        stats.mWorkWakes = mWorkWakes.load(std::memory_order_relaxed);
        stats.mPollPasses = mPollPasses.load(std::memory_order_relaxed);
        stats.mRetirements = mRetirements.load(std::memory_order_relaxed);
        for (std::size_t id = 0; id < mWorkers.size(); ++id) {
            stats.mGenerations[id] = mWorkers[id].mGeneration;
            if (mWorkers[id].mRetirePending) ++stats.mRetireBlocked;
        }
        return stats;
    }
    std::size_t testBudget() const noexcept { return mBudget.load(std::memory_order_acquire); }
    int testBudget(std::size_t budget) noexcept
    {
        if (!budget || budget > 8) return au::err::kErrorInvalidParam;
        { std::lock_guard<std::mutex> lock(mMutex);
          if (mJobs) return au::err::kErrorBusy;
          for (au::flow::detail::Node* node = mActive; node; node = node->mActiveNext)
              if (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) return au::err::kErrorBusy;
          }
        { std::lock_guard<std::mutex> lock(mWorkerMutex);
          mBudget.store(budget, std::memory_order_release);
          mEligibleCount.store(eligibleWorkers(), std::memory_order_release); }
        mPolicyChanged.notify_all();
        mWork.control();
        grow(true);
        return 0;
    }
#endif
    void trace(au::flow::detail::Node* node, const char* name, char phase, int code = 0) noexcept
    {
        if (!mDiagnostics.enabled()) return; // Tracing is observational, default off.
        const int worker = au::flow::detail::gWorker;
        const std::size_t index = worker < 0 ? 8 : static_cast<std::size_t>(worker);
        const std::uint64_t thread = worker < 0 ? au::sys::flow::threadId() : mWorkers[index].mThreadId;
        mDiagnostics.record(index, node->mId, thread, name, phase, code,
            node->mParent.get() ? node->mParent->mId : 0, node->mName.data(), node->mName.size());
    }
    class JobScope {
    public:
        explicit JobScope(au::flow::detail::Core* core) noexcept : mCore(core) {}
        int accept() noexcept
        {
            std::lock_guard<std::mutex> lock(mCore->mMutex);
            if (mCore->mPhase == Phase::Closed ||
                (mCore->mPhase == Phase::Stopping && !au::flow::detail::gFrame))
                return au::err::kErrorStateInvalid;
            if (mCore->mJobs == SIZE_MAX) return au::err::kErrorOverflow;
            ++mCore->mJobs;
            mAccepted = true;
            if (mCore->mPhase == Phase::Dormant) mCore->mPhase = Phase::Running;
            return 0;
        }
        ~JobScope() noexcept
        {
            if (!mAccepted) return;
            {
                std::lock_guard<std::mutex> lock(mCore->mMutex);
                --mCore->mJobs;
            }
            mCore->mDrained.notify_all();
        }
        JobScope(const JobScope&) = delete;
        JobScope& operator=(const JobScope&) = delete;
    private:
        au::flow::detail::Core* mCore;
        bool mAccepted = false;
    };

    // Only called by an accepted Job. Helpers have no DAG predecessors and do
    // not form independent waitAll roots. Retain Node identity for wait-cycle
    // checks and completion, but skip general graph preparation/allocations.
    std::size_t acceptHelpers(au::flow::detail::NodeRef* helpers, std::size_t count) noexcept
    {
        WorkerAdmission admission(this);
        if (!count || admission.accept()) return 0; // Optional: caller can finish every tile.
        std::size_t accepted = 0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            au::flow::detail::Node* parent = au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode : nullptr;
            for (; accepted < count; ++accepted) {
                if (mPhase == Phase::Closed || mOrder == UINT64_MAX || mAccepted == SIZE_MAX) break;
                if (parent && !au::flow::detail::reserveOutstanding(parent)) break;
                au::flow::detail::Node* node = helpers[accepted].get();
                node->mOrder = ++mOrder;
                node->mParent = au::flow::detail::NodeRef(parent);
                node->mCommitted.store(true, std::memory_order_relaxed); // Before ready-queue publication.
                node->mSubmission.store(0, std::memory_order_relaxed);
                au::flow::detail::retain(node); // Accepted reference, released by finish.
                node->mActiveNext = mActive;
                if (mActive) mActive->mActivePrev = node;
                mActive = node;
                ++mAccepted;
                node->mState.store(au::flow::TaskState::Ready, std::memory_order_release);
            }
        }
        for (std::size_t i = 0; i < accepted; ++i) {
            trace(helpers[i].get(), "Task", 'b');
            publishReady(helpers[i].get());
        }
        grow();
        return accepted;
    }
    void joinHelper(au::flow::detail::Node* node) noexcept
    {
        au::flow::detail::CpuSlicePause waiting;
        // This edge already exists: managed helpers reserve their parent's
        // completion, and all callbacks still use the normal wait-cycle checks.
        // Re-validating it through general wait() would allocate and could fail
        // before join, letting a helper retain a pointer to an expired Loop.
        CooperativeWait registration(this, node);
        while (!au::flow::detail::finalState(node->mState.load(std::memory_order_acquire))) {
            if (au::flow::detail::gFrame) {
                (void)helpOne(true, std::chrono::steady_clock::time_point::max(), node);
            } else {
                std::unique_lock<std::mutex> lock(node->mWaitMutex);
                node->mChanged.wait(lock, [node] {
                    return au::flow::detail::finalState(node->mState.load(std::memory_order_acquire));
                });
            }
        }
    }

    int tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept;
    void externalProfile(const au::flow::detail::ExternalProfile& profile) noexcept
    {
        if (!mDiagnostics.enabled()) return; // Observation only; never affects task completion.
        const int worker = au::flow::detail::gWorker;
        const std::size_t index = worker < 0 ? 8 : static_cast<std::size_t>(worker);
        const auto thread = worker < 0 ? au::sys::flow::threadId() : mWorkers[index].mThreadId;
        mDiagnostics.record(index, profile.mTask, thread, "GPU profile", 'i', 0, 0, nullptr, 0, &profile);
    }
private:
    static constexpr auto kIdleTimeout = std::chrono::seconds(2);
    static constexpr auto kPollMinimum = std::chrono::microseconds(200);
    static constexpr auto kPollMaximum = std::chrono::microseconds(2000);
    static constexpr unsigned int kPollBatch = 8;
    static constexpr unsigned int kQueueProbeLimit = 8;
    enum class Phase { Dormant, Running, Stopping, Closed };
    enum class WorkerState { Free, Running, Retiring, Exited, Joining };
    struct Worker {
        std::thread mThread;
        std::uint64_t mThreadId = 0;
        std::atomic<std::uint64_t> mAppliedVersion{UINT64_MAX};
        std::uint64_t mGeneration = 0;
        WorkerState mState = WorkerState::Free;
        std::size_t mPins = 0;
        bool mRetirePending = false;
        bool mPollProgress = false;
        std::size_t mPollRemaining = 0;
        std::chrono::steady_clock::time_point mPollDue;
        std::chrono::microseconds mPollDelay{200};
        int mAffinityCode = au::err::kErrorNotReady;
    };
    au::flow::detail::PayloadPool mPayloads;
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
    std::size_t mJobs = 0; // Accepted synchronous calls, protected by mMutex with Phase.
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
    std::atomic<std::size_t> mWorkerCount{0};
    std::atomic<std::size_t> mEligibleCount{0};
    std::size_t mAdmissions = 0;
    std::size_t mJoiners = 0;
    std::atomic<bool> mRetirePending{false};
    std::atomic<int> mPollOwner{-1};
    std::atomic_flag mPolling = ATOMIC_FLAG_INIT;
    std::size_t mAffinityQueries = 0;
    bool mClosingQueries = false;
    int mLastWorkerNative = 0; // Protected by mWorkerMutex, copied into first-submit rejection.
    std::atomic<std::size_t> mBudget{1};
    std::vector<std::uint32_t> mAllowed;
    std::vector<std::uint32_t> mPolicy;
    std::atomic<std::uint64_t> mPolicyVersion{0};
#ifdef AURA_FLOW_TESTING
    std::atomic<std::uint64_t> mWaitTimeouts{0}, mControlWakes{0}, mWorkWakes{0}, mPollPasses{0}, mRetirements{0};
#endif
    au::flow::detail::Diagnostics mDiagnostics;
};

void WorkerEntry::operator()() const noexcept { mCore->workerLoop(mId); }
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
    au::flow::detail::ExternalStatus mError;
    bool mErrorException = false;
    // Bound queue traffic while retaining a scheduling opportunity for other
    // tasks. This is a measured tuning candidate, not a time/fairness guarantee.
    static constexpr unsigned int kHelperTileBatch = 8;
    static bool more(void* value) noexcept
    {
        auto& loop = *static_cast<au::flow::detail::Loop*>(value);
        return !loop.mFailed.load(std::memory_order_acquire) &&
            loop.mNext.load(std::memory_order_relaxed) < loop.mRange;
    }
    static int step(void* value) noexcept
    {
        auto& loop = *static_cast<au::flow::detail::Loop*>(value);
        if (!more(value)) return 0;
        std::size_t begin = loop.mNext.load(std::memory_order_relaxed); // Disjoint tickets, not data publication.
        std::size_t count;
        do {
            if (begin == loop.mRange) return 0;
            count = std::min(loop.mTile, loop.mRange - begin);
        } while (!loop.mNext.compare_exchange_weak(begin, begin + count, std::memory_order_relaxed));
        au::flow::detail::TileContext context{{}, &loop.mFailed};
        int code;
        {
            au::flow::detail::TileExtent extent(&context, au::flow::detail::core().memorySink());
            code = loop.mInvoke(loop.mPayload, begin, count);
        }
        // A callback's explicit error wins. Forwarding an equal scoped error
        // retains native provenance; void/zero still reports ignored violations.
        auto error = context.mError;
        const bool exception = code == au::flow::kErrorUnhandledException && context.mException;
        if (code && (code != error.mCode || exception))
            error = {code, 0, exception ? au::flow::ErrorDomain::Flow : au::flow::ErrorDomain::Callable, 0};
        if (error.mCode) {
            {
                std::lock_guard<std::mutex> lock(loop.mMutex);
                if (begin < loop.mErrorOffset) {
                    loop.mErrorOffset = begin;
                    loop.mError = error;
                    loop.mErrorException = exception;
                }
            }
            loop.mFailed.store(true, std::memory_order_release); // Stop new tiles; all in-flight tiles still join.
        }
        return 0;
    }
    static int batch(void* value) noexcept
    {
        for (unsigned int i = 0; i < kHelperTileBatch && more(value); ++i) (void)step(value);
        return 0;
    }
    static int run(void* value) noexcept
    { while (more(value)) (void)step(value); return 0; }
};
int Core::tiled(std::size_t range, std::size_t tile, au_flow_tile invoke, void* payload) noexcept
{
    if (!tile || !invoke) return au::err::kErrorInvalidParam;
    if (!range) return 0;
    if (au::flow::detail::gCleanupDepth) {
        scopedError(au::flow::kErrorWouldDeadlock);
        return au::flow::kErrorWouldDeadlock;
    }
    // Nested tiles borrow their enclosing Job. Job acceptance and Stopping
    // share mMutex, so shutdown cannot pass a zero counter before a late entry.
    JobScope job(this);
    if (!au::flow::detail::gTileContext) {
        const int code = job.accept();
        if (code) { scopedError(code); return code; }
    }
    Loop loop{range, tile, invoke, payload};
    std::array<au::flow::detail::NodeRef, 3> helpers;
    std::size_t prepared = 0;
    if (!au::flow::detail::gTileContext && au::flow::detail::gDepth < 31) {
        const std::size_t tiles = range / tile + (range % tile != 0);
        const std::size_t participants = std::min<std::size_t>(4,
            std::min(tiles, mBudget.load(std::memory_order_acquire)));
        try {
            for (std::size_t i = 1; i < participants; ++i) {
                auto helper = make(Kind::Work);
                helper->mClosure = au::flow::detail::Closure(&Loop::batch, nullptr, &loop);
                helper->mContinue = &Loop::more;
                helpers[prepared++] = std::move(helper);
            }
        } catch (...) { /* Optional helpers: caller fallback does not poison the Job. */ }
    }
    const std::size_t accepted = acceptHelpers(helpers.data(), prepared);
    (void)Loop::run(&loop); // Same TileContext for caller, helpers and serial fallbacks.
    for (std::size_t i = 0; i < accepted; ++i) joinHelper(helpers[i].get());
    if (loop.mErrorException) au_flow_record_exception();
    scopedError(loop.mError.mCode, loop.mError.mDomain, loop.mError.mNativeCode, loop.mError.mCauseTaskId);
    return loop.mError.mCode;
}

int prepareExternal(const au::flow::detail::ExternalOps& ops, void* payload,
                    au::flow::detail::ExternalToken** out) noexcept
{ return au::flow::detail::core().prepareExternal(ops, payload, out); }
std::uint64_t currentTaskId() noexcept
{ return au::flow::detail::gFrame ? au::flow::detail::gFrame->mNode->mId : 0; }
bool tracingEnabled() noexcept { return au::flow::detail::core().tracingEnabled(); }
bool cleanupActive() noexcept { return au::flow::detail::gCleanupDepth != 0; }
void* pauseCpuSlice() noexcept
{
    auto* frame = au::flow::detail::gFrame;
    if (!frame || !frame->mCpuActive) return nullptr;
    au::flow::detail::core().trace(frame->mNode, "CPU slice", 'E'); frame->mCpuActive = false;
    return frame;
}
void resumeCpuSlice(void* cookie) noexcept
{
    auto* frame = static_cast<au::flow::detail::Frame*>(cookie);
    if (!frame || frame != au::flow::detail::gFrame || frame->mCpuActive) return;
    frame->mCpuActive = true; au::flow::detail::core().trace(frame->mNode, "CPU slice", 'B');
}
bool assistExternalWait(bool (*ready)(void*) noexcept, void* payload, std::uint64_t retryMicroseconds) noexcept
{
    if (!au::flow::detail::gFrame || au::flow::detail::gWorker < 0 || au::flow::detail::gCleanupDepth ||
        au::flow::detail::gDepth >= 32) return false;
    const auto deadline = retryMicroseconds == UINT64_MAX ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + std::chrono::microseconds(retryMicroseconds);
    (void)au::flow::detail::core().helpOne(true, deadline, nullptr, nullptr, ready, payload);
    return true;
}
void notifyExternalWaiters() noexcept { au::flow::detail::core().notifyExternalWaiters(); }
void leaveExternalWait() noexcept { au::flow::detail::core().leaveCooperativeWait(); }

void cleanupExternal(au::flow::detail::ExternalToken* token, void (*fn)(void*) noexcept, void* payload) noexcept
{
    auto* owner = token ? token->mCore : &au::flow::detail::core();
    au::flow::detail::NodeRef pin(token ? token->mParent.get() : nullptr);
    au::flow::detail::Core::CleanupExtent extent(owner, pin.get());
    if (fn) fn(payload);
}
void recordExternalProfile(const au::flow::detail::ExternalProfile& profile) noexcept
{ au::flow::detail::core().externalProfile(profile); }
void recordExternalError(const au::flow::detail::ExternalStatus& status) noexcept
{ au::flow::detail::core().scopedError(status.mCode, status.mDomain, status.mNativeCode, status.mCauseTaskId); }
void notifyExternal(au::flow::detail::ExternalToken* token, const au::flow::detail::ExternalStatus& status) noexcept
{ if (token) token->mCore->notifyExternal(token, status); }
void pollExternal(au::flow::detail::ExternalToken* token) noexcept
{ if (token) token->mCore->queueExternal(token); }
void leaveExternalRegistrar(au::flow::detail::ExternalToken* token) noexcept
{ if (token) token->mRegistrar.store(false, std::memory_order_release); /* Last registrar access. */ }

} // namespace detail
} // namespace flow
} // namespace au

#ifdef AURA_FLOW_TESTING
int au::flow::testing::queueHooks(const au::flow::detail::QueueHooks& hooks) noexcept
{ return au::flow::detail::core().queueHooks(hooks); }
void au::flow::testing::runtimeHook(au::flow::testing::RuntimeHook* hook) noexcept
{
    std::unique_lock<std::mutex> lock(au::flow::detail::gRuntimeHookMutex);
    au::flow::detail::gRuntimeHook = nullptr;
    au::flow::detail::gRuntimeHookChanged.wait(lock, [] { return au::flow::detail::gRuntimeReaders == 0; });
    au::flow::detail::gRuntimeHook = hook;
}
au::flow::testing::RuntimeStats au::flow::testing::runtimeStats() noexcept
{ return au::flow::detail::core().runtimeStats(); }
void au::flow::testing::failAfter(au::flow::testing::Failure failure, int count) noexcept
{ au::flow::detail::gFailure[static_cast<unsigned int>(failure)].store(count, std::memory_order_relaxed); }
void au::flow::testing::setOutstandingLimit(std::size_t limit) noexcept
{ au::flow::detail::gOutstandingLimit.store(limit, std::memory_order_relaxed); }
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
int au_flow_payload_allocate(std::size_t size, std::size_t alignment, void** out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    if (!size || !alignment || (alignment & (alignment - 1))) return au::err::kErrorInvalidParam;
    if (size > SIZE_MAX - alignment) return au::err::kErrorOverflow;
    try {
#ifdef AURA_FLOW_TESTING
        if (au::flow::detail::fail(au::flow::testing::Failure::Payload)) return au::err::kErrorNoMemory;
#endif
        *out = au::flow::detail::core().payloads().allocate(size, alignment);
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
void au_flow_payload_release(void* payload, std::size_t size, std::size_t alignment) noexcept
{ if (payload) au::flow::detail::core().payloads().release(payload, size, alignment); }
int au_flow_make(au_flow_invoke invoke, au_flow_destroy destroy, void* payload, au_flow_handle* out) noexcept
{
    au::flow::detail::Closure owner(invoke, destroy, payload);
    if (out) *out = nullptr;
    if (!out || !invoke) return au::err::kErrorInvalidParam;
    try {
        auto node = au::flow::detail::core().make(au::flow::detail::Kind::Work);
        node->mClosure = std::move(owner);
        *out = node.release(); return 0;
    } catch (const au::flow::detail::CreateFailure& error) { return error.mCode; }
    catch (...) { return au::err::kErrorNoMemory; }
}
int au_flow_group(au_flow_handle* out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    try {
        auto node = au::flow::detail::core().make(au::flow::detail::Kind::Group);
        node->mEntry = au::flow::detail::core().make(au::flow::detail::Kind::Entry);
        *out = node.release();
        return 0;
    } catch (const au::flow::detail::CreateFailure& error) { return error.mCode; }
    catch (...) { return au::err::kErrorNoMemory; }
}
void au_flow_retain(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (node && au::flow::detail::core().owns(node)) au::flow::detail::retain(node);
}
void au_flow_release(au_flow_handle handle) noexcept
{
    auto* node = static_cast<au::flow::detail::Node*>(handle);
    if (node && au::flow::detail::core().owns(node)) au::flow::detail::release(node);
}
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
    if (!name) return au::flow::detail::core().builderError(node, au::err::kErrorNullPointer);
    try { node->mName = name; return 0; }
    catch (...) { return au::flow::detail::core().builderError(node, au::err::kErrorNoMemory); }
}
int au_flow_precede(au_flow_handle before, au_flow_handle after) noexcept
{ return au::flow::detail::core().precede(static_cast<au::flow::detail::Node*>(before), static_cast<au::flow::detail::Node*>(after)); }
int au_flow_builder_error(au_flow_handle handle, int code) noexcept
{
    return au::flow::detail::core().builderError(static_cast<au::flow::detail::Node*>(handle), code);
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
    if (au::flow::detail::gTileContext) {
        au::flow::detail::gTileContext->mException = true;
        au::flow::detail::core().scopedError(au::flow::kErrorUnhandledException, au::flow::ErrorDomain::Flow);
        return;
    }
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
