#include <array>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

#include "flow/payload_pool.h"
#include "flow/ready_queue.h"
#include "flow/slab.h"
#include "flow_reference_allocators.h"
#include "flow_reference_queue.h"
#include "gtest/gtest.h"

namespace {
template <class Q>
void churn()
{
    struct Item
    {
        au::flow::detail::ReadyLink mLink;
        std::atomic<unsigned int>   mState{0};
        unsigned int                mEpoch = 0;
        std::atomic<unsigned int>   mSeen{0};
    };
    constexpr std::size_t   count = 64, rounds = 64, total = count * rounds;
    Q                       queue;
    std::array<Item, count> items;
    for (auto& item : items)
        item.mLink.mValue = &item;
    std::atomic<std::size_t>   consumed{0};
    std::atomic<bool>          bad{false}, start{false};
    std::array<std::thread, 8> threads;
    for (std::size_t p = 0; p < 4; ++p)
        threads[p] = std::thread([&, p] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (unsigned int epoch = 1; epoch <= rounds; ++epoch)
                for (std::size_t i = p; i < count; i += 4) {
                    auto& item = items[i];
                    while (item.mState.load(std::memory_order_acquire) != 0)
                        std::this_thread::yield();
                    item.mEpoch = epoch;
                    item.mState.store(1, std::memory_order_relaxed);
                    if ((i + epoch) % 7 == 0)
                        std::this_thread::yield();
                    queue.push(&item.mLink);
                }
        });
    for (std::size_t c = 0; c < 4; ++c)
        threads[c + 4] = std::thread([&, c] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (consumed.load(std::memory_order_relaxed) < total) {
                au::flow::detail::ReadyLink* link = nullptr;
                if (!queue.pop(link, (c & 1) != 0)) {
                    std::this_thread::yield();
                    continue;
                }
                auto* item = static_cast<Item*>(link->mValue);
                if (item->mState.exchange(2, std::memory_order_relaxed) != 1)
                    bad.store(true, std::memory_order_relaxed);
                if (item->mSeen.fetch_add(1, std::memory_order_relaxed) + 1 != item->mEpoch)
                    bad.store(true, std::memory_order_relaxed);
                if (item->mEpoch % 9 == 0)
                    std::this_thread::yield();
                item->mState.store(0,
                                   std::memory_order_release);  // Last item access; producer can reuse it immediately.
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();
    EXPECT_FALSE(bad.load(std::memory_order_relaxed));
    EXPECT_EQ(consumed.load(std::memory_order_relaxed), total);
    for (const auto& item : items)
        EXPECT_EQ(item.mSeen.load(std::memory_order_relaxed), rounds);
}
template <class Q>
void pausedPublisher()
{
    Q queue;
    struct Pause
    {
        std::atomic<bool> mChosen{false}, mEntered{false}, mRelease{false};
    } pause;
    au::flow::detail::QueueHooks hooks{};
    hooks.mPayload      = &pause;
    hooks.mRingReserved = [](void* payload) noexcept {
        auto& p = *static_cast<Pause*>(payload);
        if (p.mChosen.exchange(true, std::memory_order_acq_rel))
            return;
        p.mEntered.store(true, std::memory_order_release);
        while (!p.mRelease.load(std::memory_order_acquire))
            std::this_thread::yield();
    };
    queue.testHooks(hooks);
    std::array<au::flow::detail::ReadyLink, 65> links;
    std::thread                                 first([&] { queue.push(&links[0]); });
    while (!pause.mEntered.load(std::memory_order_acquire))
        std::this_thread::yield();
    for (std::size_t i = 1; i < links.size(); ++i)
        queue.push(&links[i]);
    au::flow::detail::ReadyLink* out        = nullptr;
    const bool                   progressed = queue.pop(out, true);
    std::array<bool, 65>         seen{};
    if (progressed)
        seen[static_cast<std::size_t>(out - links.data())] = true;
    pause.mRelease.store(true, std::memory_order_release);
    first.join();
    EXPECT_TRUE(progressed);  // Intrusive overflow / FIFO can progress without this unpublished item.
    for (std::size_t i = progressed ? 1 : 0; i < links.size(); ++i) {
        EXPECT_TRUE(queue.pop(out, true));
        const auto index = static_cast<std::size_t>(out - links.data());
        ASSERT_LT(index, seen.size());
        EXPECT_FALSE(seen[index]);
        seen[index] = true;
    }
    for (bool value : seen)
        EXPECT_TRUE(value);
}
}  // namespace
TEST(XFlow, MechanismQueuesMatchElasticPublicationAndImmediateReuseContracts)
{
    churn<au::flow::detail::BasicReadyQueue<unsigned char, 4>>();
    churn<au::flow::detail::MutexReadyQueue>();
    pausedPublisher<au::flow::detail::BasicReadyQueue<unsigned char, 4>>();
    pausedPublisher<au::flow::detail::MutexReadyQueue>();
}
TEST(XFlow, DirectAllocationReferencePreservesAlignmentRollbackAndExactDestruction)
{
    struct alignas(128) Record
    {
        std::unique_ptr<int>       mValue;
        std::atomic<unsigned int>* mDestroyed;
        Record(std::atomic<unsigned int>& counter, int n) : mValue(new int(n)), mDestroyed(&counter)
        {
            if (n < 0)
                throw std::runtime_error("rollback");
        }
        ~Record() noexcept { mDestroyed->fetch_add(1, std::memory_order_relaxed); }
    };
    std::atomic<unsigned int>                  destroyed{0};
    au::flow::detail::SystemRecords<Record, 2> records;
    EXPECT_THROW(records.create(destroyed, -1), std::runtime_error);
    std::array<std::thread, 4> threads;
    for (auto& thread : threads)
        thread = std::thread([&] {
            for (int i = 0; i < 256; ++i) {
                auto* value = records.create(destroyed, i);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value) % alignof(Record), 0u);
                EXPECT_EQ(*value->mValue, i);
                records.destroy(value);
            }
        });
    for (auto& thread : threads)
        thread.join();
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1024u);
    au::flow::detail::SystemPayloadAllocator payload;
    for (std::size_t alignment : {alignof(std::max_align_t), std::size_t(128)}) {
        void* bytes = payload.allocate(8192, alignment);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(bytes) % alignment, 0u);
        payload.release(bytes, 8192, alignment);
    }
}
