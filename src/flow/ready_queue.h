#ifndef AURA_FLOW_READY_QUEUE_H_
#define AURA_FLOW_READY_QUEUE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace au {
namespace flow {
namespace detail {

struct ReadyLink {
    std::atomic<au::flow::detail::ReadyLink*> mNext{nullptr};
    void* mValue = nullptr;
};

// Bounded sequence ring plus intrusive overflow, without a business capacity.
// Sequence-slot technique: Dmitry Vyukov's bounded MPMC algorithm. Overflow is
// the exchange/link MPSC technique, also used by gRPC. This implementation is
// written here; see docs/flow_implementation.md for provenance and validation.
class ReadyQueue {
public:
    ReadyQueue() noexcept : mTail(&mStub), mHead(&mStub)
    {
        for (std::size_t i = 0; i < mRing.size(); ++i)
            mRing[i].mSequence.store(i, std::memory_order_relaxed); // Not yet shared.
    }
    ReadyQueue(const au::flow::detail::ReadyQueue&) = delete;
    au::flow::detail::ReadyQueue& operator=(const au::flow::detail::ReadyQueue&) = delete;

    void push(au::flow::detail::ReadyLink* item) noexcept
    {
        std::size_t pos = mEnqueue.load(std::memory_order_relaxed); // Ticket hint.
        for (unsigned int attempt = 0; attempt < 8; ++attempt) {
            Slot& slot = mRing[pos & (mRing.size() - 1)];
            const std::size_t seq = slot.mSequence.load(std::memory_order_acquire); // Previous consumer released slot.
            const std::intptr_t delta = static_cast<std::intptr_t>(seq - pos);
            if (delta == 0) {
                if (mEnqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.mItem = item;
                    slot.mSequence.store(pos + 1, std::memory_order_release); // Publish initialized item.
                    return;
                }
            } else if (delta < 0) break;
            else pos = mEnqueue.load(std::memory_order_relaxed); // Retry a fresh ticket.
        }
        overflowPush(item);
    }

    // A false result is transient: the caller keeps its semaphore permit and
    // retries. Another consumer or an interrupted producer may own the next slot.
    bool pop(au::flow::detail::ReadyLink*& out, bool overflowFirst) noexcept
    {
        if (overflowFirst && overflowPop(out)) return true;
        std::size_t pos = mDequeue.load(std::memory_order_relaxed); // Ticket hint.
        for (unsigned int attempt = 0; attempt < 8; ++attempt) {
            Slot& slot = mRing[pos & (mRing.size() - 1)];
            const std::size_t seq = slot.mSequence.load(std::memory_order_acquire); // Producer's item publication.
            const std::intptr_t delta = static_cast<std::intptr_t>(seq - (pos + 1));
            if (delta == 0) {
                if (mDequeue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = slot.mItem;
                    slot.mSequence.store(pos + mRing.size(), std::memory_order_release); // Recycle only after reading.
                    return true;
                }
            } else if (delta < 0) break;
            else pos = mDequeue.load(std::memory_order_relaxed); // Retry a fresh ticket.
        }
        return !overflowFirst && overflowPop(out);
    }
private:
    struct Slot {
        std::atomic<std::size_t> mSequence{0};
        au::flow::detail::ReadyLink* mItem = nullptr;
    };
    void overflowPush(au::flow::detail::ReadyLink* item) noexcept
    {
        item->mNext.store(nullptr, std::memory_order_relaxed); // Private before exchanging tail.
        au::flow::detail::ReadyLink* prev = mTail.exchange(item, std::memory_order_acq_rel); // Acquire previous producer's node.
        prev->mNext.store(item, std::memory_order_release); // Complete the potentially interrupted link.
    }
    bool overflowPop(au::flow::detail::ReadyLink*& out) noexcept
    {
        if (mConsumer.test_and_set(std::memory_order_acquire)) return false; // One try; never spin under gate.
        au::flow::detail::ReadyLink* head = mHead;
        au::flow::detail::ReadyLink* next = head->mNext.load(std::memory_order_acquire); // Published producer link.
        bool found = false;
        if (head == &mStub && next) {
            mHead = next; head = next;
            next = head->mNext.load(std::memory_order_acquire); // Advance past sentinel.
        }
        if (head != &mStub) {
            if (!next && head == mTail.load(std::memory_order_acquire)) {
                overflowPush(&mStub); // Detach the last real node before allowing its reuse.
                next = head->mNext.load(std::memory_order_acquire);
            }
            if (next) { mHead = next; out = head; found = true; }
        }
        mConsumer.clear(std::memory_order_release); // Publish consumer head to the next gate owner.
        return found;
    }
    std::array<Slot, 256> mRing;
    alignas(64) std::atomic<std::size_t> mEnqueue{0};
    alignas(64) std::atomic<std::size_t> mDequeue{0};
    au::flow::detail::ReadyLink mStub;
    std::atomic<au::flow::detail::ReadyLink*> mTail;
    au::flow::detail::ReadyLink* mHead;
    std::atomic_flag mConsumer = ATOMIC_FLAG_INIT;
};

} // namespace detail
} // namespace flow
} // namespace au
#endif
