#include "flow/ready_queue.h"
#include "gtest/gtest.h"
#include <array>
#include <atomic>
#include <thread>

namespace {
struct PublicationPause {
    std::atomic<bool> mChosen{false}, mEntered{false}, mRelease{false};
    static void pause(void* payload) noexcept
    {
        auto& self = *static_cast<PublicationPause*>(payload);
        if (self.mChosen.exchange(true, std::memory_order_acq_rel)) return; // Exactly one publisher/consumer pauses.
        self.mEntered.store(true, std::memory_order_release);
        while (!self.mRelease.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    void entered() { while (!mEntered.load(std::memory_order_acquire)) std::this_thread::yield(); }
};
}

TEST(XFlow, ReadyQueueSequenceExhaustionAndImmediateReuse)
{
    au::flow::detail::BasicReadyQueue<unsigned char, 4> queue;
    std::array<au::flow::detail::ReadyLink, 11> links;
    // The SAME implementation exhausts the 8-bit fast-path sequence and keeps
    // accepting well beyond that space through overflow, without ticket reuse.
    for (std::size_t round = 0; round < 1024; ++round) {
        std::array<bool, 11> seen{};
        for (std::size_t i = 0; i < links.size(); ++i) { links[i].mValue = &links[i]; queue.push(&links[i]); }
        for (std::size_t i = 0; i < links.size(); ++i) {
            au::flow::detail::ReadyLink* item = nullptr;
            ASSERT_TRUE(queue.pop(item, (i & 1) != 0));
            const auto index = static_cast<std::size_t>(item - links.data());
            ASSERT_LT(index, links.size()); EXPECT_FALSE(seen[index]); seen[index] = true;
            EXPECT_EQ(item->mValue, item);
        }
        au::flow::detail::ReadyLink* item = nullptr; EXPECT_FALSE(queue.pop(item, true));
    }
}

TEST(XFlow, ReadyQueuePausedPublishersAndConsumerGate)
{
    {
        au::flow::detail::BasicReadyQueue<unsigned char, 4> queue;
        PublicationPause pause; queue.testHooks({&PublicationPause::pause, nullptr, nullptr, &pause});
        std::array<au::flow::detail::ReadyLink, 5> links;
        std::thread producer([&] { queue.push(&links[0]); }); pause.entered();
        for (std::size_t i = 1; i < links.size(); ++i) queue.push(&links[i]);
        au::flow::detail::ReadyLink* item = nullptr;
        EXPECT_TRUE(queue.pop(item, true)); EXPECT_EQ(item, &links[4]); // Overflow progresses while the first ring publisher is paused.
        EXPECT_FALSE(queue.pop(item, false)); // Reserved ring ticket is not an empty queue proof.
        pause.mRelease.store(true, std::memory_order_release); producer.join();
        for (std::size_t i = 0; i < 4; ++i) { EXPECT_TRUE(queue.pop(item, false)); EXPECT_EQ(item, &links[i]); }
    }
    {
        au::flow::detail::BasicReadyQueue<unsigned char, 4> queue;
        std::array<au::flow::detail::ReadyLink, 6> links;
        for (std::size_t i = 0; i < 4; ++i) queue.push(&links[i]);
        PublicationPause pause; queue.testHooks({nullptr, &PublicationPause::pause, nullptr, &pause});
        std::thread producer([&] { queue.push(&links[4]); }); pause.entered();
        queue.push(&links[5]); // Links to the first producer; sentinel link is still unpublished.
        au::flow::detail::ReadyLink* item = nullptr;
        for (std::size_t i = 0; i < 4; ++i) { EXPECT_TRUE(queue.pop(item, true)); EXPECT_EQ(item, &links[i]); }
        EXPECT_FALSE(queue.pop(item, true));
        pause.mRelease.store(true, std::memory_order_release); producer.join();
        for (std::size_t i = 4; i < 6; ++i) { EXPECT_TRUE(queue.pop(item, true)); EXPECT_EQ(item, &links[i]); }
    }
    {
        au::flow::detail::BasicReadyQueue<unsigned char, 4> queue;
        std::array<au::flow::detail::ReadyLink, 5> links;
        for (auto& link : links) queue.push(&link);
        au::flow::detail::ReadyLink* item = nullptr;
        for (std::size_t i = 0; i < 4; ++i) EXPECT_TRUE(queue.pop(item, false));
        PublicationPause pause; queue.testHooks({nullptr, nullptr, &PublicationPause::pause, &pause});
        au::flow::detail::ReadyLink* first = nullptr;
        std::thread consumer([&] { EXPECT_TRUE(queue.pop(first, true)); }); pause.entered();
        EXPECT_FALSE(queue.pop(item, true)); // No spinning on a descheduled consumer.
        pause.mRelease.store(true, std::memory_order_release); consumer.join(); EXPECT_EQ(first, &links[4]);
    }
    {
        au::flow::detail::BasicReadyQueue<unsigned char, 4> queue;
        PublicationPause pause;
        queue.testHooks({nullptr, nullptr, nullptr, &pause, &PublicationPause::pause});
        std::array<au::flow::detail::ReadyLink, 6> links;
        std::thread producer([&] { queue.push(&links[0]); }); pause.entered();
        // More than one complete 8-bit ticket space passes while the first
        // observer is paused BEFORE CAS. A wrapping position can ABA here.
        au::flow::detail::ReadyLink* item = nullptr;
        for (std::size_t i = 0; i < 508; ++i) { queue.push(&links[1]); EXPECT_TRUE(queue.pop(item, false)); EXPECT_EQ(item, &links[1]); }
        for (std::size_t i = 1; i < 5; ++i) queue.push(&links[i]);
        pause.mRelease.store(true, std::memory_order_release); producer.join();
        std::array<bool, 6> seen{};
        for (std::size_t i = 0; i < 5; ++i) {
            ASSERT_TRUE(queue.pop(item, true));
            const std::size_t index = static_cast<std::size_t>(item - links.data());
            ASSERT_LT(index, 5u); EXPECT_FALSE(seen[index]); seen[index] = true;
        }
        EXPECT_FALSE(queue.pop(item, true));
    }
}
