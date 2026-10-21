#ifndef AURA_TEST_FLOW_REFERENCE_QUEUE_H_
#define AURA_TEST_FLOW_REFERENCE_QUEUE_H_
// Experiment only. Include flow/ready_queue.h first for ReadyLink/QueueHooks.
#include <mutex>
namespace au {
namespace flow {
namespace detail {
// Elastic intrusive FIFO: never allocates after acceptance, including overflow.
// Lock release/acquire publishes the initialized node and detaches before reuse.
class MutexReadyQueue final
{
public:
    MutexReadyQueue() noexcept                         = default;
    MutexReadyQueue(const MutexReadyQueue&)            = delete;
    MutexReadyQueue& operator=(const MutexReadyQueue&) = delete;
    void             push(ReadyLink* item) noexcept
    {
#ifdef AURA_FLOW_TESTING
        if (mHooks.mRingReserved)
            mHooks.mRingReserved(mHooks.mPayload);
#endif
        std::lock_guard<std::mutex> lock(mMutex);
        item->mNext.store(nullptr, std::memory_order_relaxed);
        if (mTail)
            mTail->mNext.store(item, std::memory_order_relaxed);
        else
            mHead = item;
        mTail = item;
    }
    bool pop(ReadyLink*& out, bool) noexcept
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mHead)
            return false;
        out   = mHead;
        mHead = out->mNext.load(std::memory_order_relaxed);
        if (!mHead)
            mTail = nullptr;
        return true;
    }
#ifdef AURA_FLOW_TESTING
    void testHooks(const QueueHooks& hooks) noexcept { mHooks = hooks; }
#endif
private:
    std::mutex mMutex;
    ReadyLink* mHead = nullptr;
    ReadyLink* mTail = nullptr;
#ifdef AURA_FLOW_TESTING
    QueueHooks mHooks;
#endif
};
}  // namespace detail
}  // namespace flow
}  // namespace au
#endif
