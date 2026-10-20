#ifndef AURA_FLOW_READY_QUEUE_H_
#define AURA_FLOW_READY_QUEUE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

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
#ifdef AURA_FLOW_TESTING
struct QueueHooks {
    void (*mRingReserved)(void*) noexcept = nullptr;
    void (*mOverflowExchanged)(void*) noexcept = nullptr;
    void (*mConsumerEntered)(void*) noexcept = nullptr;
    void* mPayload = nullptr;
    void (*mRingObserved)(void*) noexcept = nullptr;
};
#endif
template<class Counter, std::size_t Capacity> class BasicReadyQueue {
    static_assert(std::is_unsigned<Counter>::value, "Queue counters use defined unsigned modular arithmetic");
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0 &&
                  Capacity <= std::numeric_limits<Counter>::max() / 4, "Ring fits inside the sequence half-range");
public:
    BasicReadyQueue() noexcept : mTail(&mStub), mHead(&mStub)
    {
        for (std::size_t i = 0; i < mRing.size(); ++i)
            mRing[i].mSequence.store(static_cast<Counter>(i), std::memory_order_relaxed); // Not yet shared.
    }
    BasicReadyQueue(const BasicReadyQueue&) = delete;
    BasicReadyQueue& operator=(const BasicReadyQueue&) = delete;
#ifdef AURA_FLOW_TESTING
    // Installed only before publishing the queue to test threads. Hooks model
    // descheduling at otherwise unobservable publication/gate boundaries.
    void testHooks(const au::flow::detail::QueueHooks& hooks) noexcept { mHooks = hooks; }
#endif

    void push(au::flow::detail::ReadyLink* item) noexcept
    {
        Counter pos = mEnqueue.load(std::memory_order_relaxed); // Ticket hint.
        for (unsigned int attempt = 0; attempt < 8; ++attempt) {
            if (pos >= kRingEnd) break; // Retire this fast path before ANY ticket/slot generation can wrap.
            Slot& slot = mRing[pos & (mRing.size() - 1)];
            const Counter seq = slot.mSequence.load(std::memory_order_acquire); // Previous consumer released slot.
            const Counter delta = static_cast<Counter>(seq - pos);
            if (delta == 0) {
#ifdef AURA_FLOW_TESTING
                if (mHooks.mRingObserved) mHooks.mRingObserved(mHooks.mPayload);
#endif
                if (mEnqueue.compare_exchange_weak(pos, static_cast<Counter>(pos + 1), std::memory_order_relaxed)) {
#ifdef AURA_FLOW_TESTING
                    if (mHooks.mRingReserved) mHooks.mRingReserved(mHooks.mPayload);
#endif
                    slot.mItem = item;
                    slot.mSequence.store(static_cast<Counter>(pos + 1), std::memory_order_release); // Publish initialized item.
                    return;
                }
            } else if (behind(delta)) break;
            else pos = mEnqueue.load(std::memory_order_relaxed); // Retry a fresh ticket.
        }
        overflowPush(item);
    }

    // A false result is transient: the caller keeps its semaphore permit and
    // retries. Another consumer or an interrupted producer may own the next slot.
    bool pop(au::flow::detail::ReadyLink*& out, bool overflowFirst) noexcept
    {
        if (overflowFirst && overflowPop(out)) return true;
        Counter pos = mDequeue.load(std::memory_order_relaxed); // Ticket hint.
        for (unsigned int attempt = 0; attempt < 8; ++attempt) {
            if (pos >= kRingEnd) break;
            Slot& slot = mRing[pos & (mRing.size() - 1)];
            const Counter seq = slot.mSequence.load(std::memory_order_acquire); // Producer's item publication.
            const Counter delta = static_cast<Counter>(seq - static_cast<Counter>(pos + 1));
            if (delta == 0) {
                if (mDequeue.compare_exchange_weak(pos, static_cast<Counter>(pos + 1), std::memory_order_relaxed)) {
                    out = slot.mItem;
                    slot.mSequence.store(static_cast<Counter>(pos + mRing.size()), std::memory_order_release); // Recycle only after reading.
                    return true;
                }
            } else if (behind(delta)) break;
            else pos = mDequeue.load(std::memory_order_relaxed); // Retry a fresh ticket.
        }
        return !overflowFirst && overflowPop(out);
    }
private:
    // A paused observer must never see the same ticket after a full counter
    // cycle. Exhaustion permanently switches to the intrusive path; submission
    // remains unbounded and no sequence reset races with live observers.
    static constexpr Counter kRingEnd = static_cast<Counter>(std::numeric_limits<Counter>::max() -
        static_cast<Counter>(Capacity) * 2);
    static bool behind(Counter delta) noexcept
    { return (delta & (Counter(1) << (std::numeric_limits<Counter>::digits - 1))) != 0; }
    struct Slot {
        std::atomic<Counter> mSequence{0};
        au::flow::detail::ReadyLink* mItem = nullptr;
    };
    void overflowPush(au::flow::detail::ReadyLink* item) noexcept
    {
        item->mNext.store(nullptr, std::memory_order_relaxed); // Private before exchanging tail.
        au::flow::detail::ReadyLink* prev = mTail.exchange(item, std::memory_order_acq_rel); // Acquire previous producer's node.
#ifdef AURA_FLOW_TESTING
        if (item != &mStub && mHooks.mOverflowExchanged) mHooks.mOverflowExchanged(mHooks.mPayload);
#endif
        prev->mNext.store(item, std::memory_order_release); // Complete the potentially interrupted link.
    }
    bool overflowPop(au::flow::detail::ReadyLink*& out) noexcept
    {
        if (mConsumer.test_and_set(std::memory_order_acquire)) return false; // One try; never spin under gate.
#ifdef AURA_FLOW_TESTING
        if (mHooks.mConsumerEntered) mHooks.mConsumerEntered(mHooks.mPayload);
#endif
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
    std::array<Slot, Capacity> mRing;
    alignas(64) std::atomic<Counter> mEnqueue{0};
    alignas(64) std::atomic<Counter> mDequeue{0};
    au::flow::detail::ReadyLink mStub;
    std::atomic<au::flow::detail::ReadyLink*> mTail;
    au::flow::detail::ReadyLink* mHead;
    std::atomic_flag mConsumer = ATOMIC_FLAG_INIT;
#ifdef AURA_FLOW_TESTING
    au::flow::detail::QueueHooks mHooks;
#endif
};
// Prefer 64-bit tickets when they are lock-free. Older targets keep native
// lock-free counters and safely retire the ring sooner; no libatomic dependency.
typedef typename std::conditional<std::atomic<std::uint64_t>::is_always_lock_free,
    std::uint64_t, std::size_t>::type ReadyCounter;
typedef au::flow::detail::BasicReadyQueue<au::flow::detail::ReadyCounter, 256> ReadyQueue;

} // namespace detail
} // namespace flow
} // namespace au
#endif
