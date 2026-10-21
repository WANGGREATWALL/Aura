#include "flow/core.h"
#include "flow/graph_search.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
void Core::scopedError(int code, au::flow::ErrorDomain domain, int native, std::uint64_t cause) noexcept
{
    if (gTileContext) {
        gTileContext->record(code, domain, native, cause);
        return;
    }
    if (!code || !gFrame)
        return;
    Node* node = gFrame->mNode;
    node->mErrors.scoped(node->mId, code, domain, native, cause);
}

void Core::inherit(Node* target, const au_flow_result& source, std::uint64_t order) noexcept
{
    target->mErrors.inherit(source, order);
}

void Core::resolve(Node* target, const au_flow_result& source, std::uint64_t order, Node** continuation) noexcept
{
    inherit(target, source, order);
    if (target->mUnresolved.fetch_sub(1, std::memory_order_acq_rel) == 1)
        activate(target, continuation);  // Gather predecessor writes.
}

void Core::activate(Node* node, Node** continuation) noexcept
{
    if (node->mKind == Kind::Group || !node->mCommitted.load(std::memory_order_acquire) ||
        node->mUnresolved.load(std::memory_order_acquire) != 0)
        return;
    au::flow::TaskState expected = au::flow::TaskState::Pending;
    if (!node->mState.compare_exchange_strong(expected, au::flow::TaskState::Ready, std::memory_order_acq_rel))
        return;  // Exactly one ready publisher.
    if (node->mErrors.dependencyFailed()) {
        node->mErrors.skip();
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

void Core::bodyEnd(Node* node, int code, Node** continuation) noexcept
{
    node->mErrors.body(code);
    if (node->mDiagnostic)
        node->mDiagnostic->mHistory->state(node->mDiagnostic->mRecord, au::flow::TaskState::Waiting);
    node->mState.store(au::flow::TaskState::Waiting, std::memory_order_release);  // Body cannot run again.
    if (node->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
        finish(node, continuation);  // Gather child/event writes.
}

void Core::finish(Node* node, Node** continuation) noexcept
{
    node->mFinishNext = gFinish;
    gFinish           = node;
    if (gFinishing)
        return;
    gFinishing = true;
    while (gFinish) {
        node    = gFinish;
        gFinish = node->mFinishNext;
        {
            Core::CleanupExtent extent(this, node);
            node->mClosure.clear();  // No locks; cleanup cannot extend a closed body reservation.
        }
        node->mResult = node->mErrors.complete(node->mId);
        if (node->mDiagnostic)
            node->mDiagnostic->mHistory->complete(node->mDiagnostic->mRecord, node->mResult);
        trace(node, "Task", 'e', node->mResult.mCode);
        bool cooperative;
        {
            std::lock_guard<std::mutex> lock(node->mWaitMutex);
            node->mState.store(static_cast<au::flow::TaskState>(node->mResult.mState), std::memory_order_release);
            cooperative = node->mCooperativeWaiters != 0;
        }
        node->mChanged.notify_all();
        if (cooperative)
            mWork.control();
        Edge* edge =
            node->mSuccessors.exchange(closedEdges(),
                                       std::memory_order_acq_rel);  // Release final result; acquire complete edge list.
        Node** single = edge && !edge->mNext ? continuation : nullptr;
        while (edge) {
            Edge* next = edge->mNext;
            resolve(edge->mTarget.get(), node->mResult, node->mOrder, single);
            mEdges.destroy(edge);
            edge = next;
        }
        NodeRef parent;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            unlinkChild(node);
            parent = std::move(node->mParent);
            if (node->mActivePrev)
                node->mActivePrev->mActiveNext = node->mActiveNext;
            else
                mActive = node->mActiveNext;
            if (node->mActiveNext)
                node->mActiveNext->mActivePrev = node->mActivePrev;
            --mAccepted;
            if (!mAccepted && mRetirePending.load(std::memory_order_acquire))
                mWork.control();
        }
        if (parent.get()) {
            inherit(parent.get(), node->mResult, node->mOrder);
            if (node->mKind == Kind::Entry && node->mErrors.skipped())
                parent->mErrors.skip();
            if (parent->mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
                finish(parent.get());
        }
        mDrained.notify_all();
        release(node);
    }
    gFinishing = false;
}

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
