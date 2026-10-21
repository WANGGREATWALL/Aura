#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "file/xfile.h"
#include "flow/diagnostics.h"
#include "flow/node.h"
#include "flow/trace_buffer.h"
#include "flow/xflow.h"
#include "gtest/gtest.h"

TEST(XFlow, CapturedDynamicHistoryDoesNotOwnCompletedExecutionNodes)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.setTracing(false), 0);
    ASSERT_EQ(flow.resetTrace(), 0);
    ASSERT_EQ(flow.setGraphCapture(true), 0);
    au_flow_handle child  = nullptr;
    auto           parent = flow.enqueue([&]() -> int {
        int code = au_flow_make([](void*) -> int { return 23; }, nullptr, nullptr, &child);
        if (code)
            return code;
        code = au_flow_name(child, "historical child");
        if (code)
            return code;
        code = au_flow_submit(child);
        if (code)
            return code;
        return au_flow_wait(child, UINT64_MAX, nullptr);  // Operation succeeds; parent still inherits child's 23.
    });
    EXPECT_EQ(parent.wait(), 23);
    ASSERT_NE(child, nullptr);
    const auto id = au_flow_id(child);
    // A final state can precede the worker's last dispatch pin release. Wait
    // for that epilogue, without assuming Task::wait drains scheduler refs.
    auto*      node     = static_cast<au::flow::detail::Node*>(child);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node->mRefs.load(std::memory_order_acquire) != 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_EQ(node->mRefs.load(std::memory_order_acquire), 1u);
    au_flow_release(child);
    child = nullptr;  // No executable node owner remains; only value history.
    EXPECT_EQ(flow.setGraphCapture(false), 0);
    ASSERT_EQ(flow.dumpDot(parent, "flow-history.dot"), 0);
    std::string dot;
    ASSERT_EQ(au::file::read("flow-history.dot", dot), 0);
    EXPECT_NE(dot.find("historical child"), std::string::npos);
    EXPECT_NE(dot.find("n" + std::to_string(id)), std::string::npos);
    EXPECT_NE(dot.find("error=23"), std::string::npos);
    EXPECT_EQ(au::file::removeFile("flow-history.dot"), 0);
}
TEST(XFlow, TraceWindowsReleaseAndRepublishSafelyAcrossConcurrentReaders)
{
    au::flow::detail::Diagnostics diagnostics;
    EXPECT_EQ(diagnostics.retainedBytes(), 0u);
    const int enabled = diagnostics.setEnabled(true);
    if (enabled == au::err::kErrorNotSupported) {
        EXPECT_EQ(diagnostics.reset(), 0);
        EXPECT_EQ(diagnostics.retainedBytes(), 0u);
        return;
    }
    ASSERT_EQ(enabled, 0);
    EXPECT_GT(diagnostics.retainedBytes(), 0u);
    EXPECT_EQ(diagnostics.reset(), au::err::kErrorStateInvalid);
    std::atomic<bool> done{false};
    std::thread       writer([&] {
        while (!done.load(std::memory_order_acquire))
            diagnostics.record(0, 1, 1, "record", 'i');
    });
    std::thread       reader([&] {
        for (int i = 0; i < 24; ++i)
            EXPECT_EQ(diagnostics.dumpTrace("flow-reset.json"), 0);
    });
    for (int i = 0; i < 24; ++i) {
        EXPECT_EQ(diagnostics.setEnabled(false), 0);
        EXPECT_EQ(diagnostics.reset(), 0);
        EXPECT_EQ(diagnostics.setEnabled(true), 0);
    }
    reader.join();
    done.store(true, std::memory_order_release);
    writer.join();
    EXPECT_EQ(diagnostics.setEnabled(false), 0);
    EXPECT_EQ(diagnostics.reset(), 0);
    EXPECT_EQ(diagnostics.retainedBytes(), 0u);
    EXPECT_EQ(au::file::removeFile("flow-reset.json"), 0);
}
TEST(XFlow, TraceCapacityPreservesImmutablePublishedSlotsAndReportsDrops)
{
    au::flow::detail::TraceBuffer buffer(4);
    for (int i = 0; i < 20; ++i)
        buffer.record(static_cast<std::uint64_t>(i), 1, "bounded", 'i', 0);
    EXPECT_EQ(buffer.dropped(), 16u);
    for (std::size_t i = 0; i < buffer.capacity(); ++i) {
        EXPECT_TRUE(buffer.slots()[i].mPublished.load(std::memory_order_acquire));
        EXPECT_EQ(buffer.slots()[i].mTask, i);
    }
}

TEST(XFlow, DotHistoryExportsConcurrentDescendantsWithoutIncludingAncestorsOfTheRequestedTask)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.setTracing(false), 0);
    ASSERT_EQ(flow.setGraphCapture(true), 0);
    std::promise<void> begin;
    au::flow::Task     last;
    auto               gate   = begin.get_future().share();
    auto               parent = flow.enqueue([&, gate]() -> int {
        gate.wait();
        for (unsigned int i = 0; i < 128; ++i) {
            auto child = flow.makeTask([]() -> int { return 0; });
            int  code  = child.setName("captured-child");
            if (code)
                return code;
            code = flow.enqueue(child).wait();
            if (code)
                return code;
            last = child;
        }
        return 0;
    });
    begin.set_value();
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(flow.dumpDot(parent, "flow-growing.dot"), 0);
    EXPECT_EQ(parent.wait(), 0);
    EXPECT_EQ(flow.setGraphCapture(false), 0);
    EXPECT_EQ(flow.dumpDot(parent, "flow-growing.dot"), 0);
    std::string dot;
    EXPECT_EQ(au::file::read("flow-growing.dot", dot), 0);
    std::size_t count = 0, pos = 0;
    while ((pos = dot.find("label=\"captured-child", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 128u);
    EXPECT_EQ(flow.dumpDot(last, "flow-growing.dot"), 0);
    EXPECT_EQ(au::file::read("flow-growing.dot", dot), 0);
    EXPECT_EQ(dot.find("n" + std::to_string(parent.id()) + " ["), std::string::npos);
    EXPECT_EQ(flow.setGraphCapture(true), 0);
    auto consumer = flow.enqueueAfter(parent, []() -> int { return 0; });
    EXPECT_EQ(consumer.wait(), 0);
    EXPECT_EQ(flow.setGraphCapture(false), 0);
    EXPECT_EQ(flow.dumpDot(consumer, "flow-growing.dot"), 0);
    EXPECT_EQ(au::file::read("flow-growing.dot", dot), 0);
    count = 0;
    pos   = 0;
    while ((pos = dot.find("label=\"captured-child", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 128u);  // Merge a copied predecessor with its independent scope history.
    EXPECT_EQ(au::file::removeFile("flow-growing.dot"), 0);
}

TEST(XFlow, GraphCapturePolicyIsFixedAtRootAcceptance)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.setTracing(false), 0);
    std::promise<void> release;
    auto               gate = release.get_future().share();
    auto               old  = flow.enqueue([&, gate]() -> int {
        gate.wait();
        return flow.enqueue([]() -> int { return 0; }).wait();
    });
    EXPECT_EQ(flow.setGraphCapture(true), 0);
    release.set_value();
    EXPECT_EQ(old.wait(), 0);
    EXPECT_EQ(flow.dumpDot(old, "flow-window.dot"), 0);
    std::string dot;
    EXPECT_EQ(au::file::read("flow-window.dot", dot), 0);
    EXPECT_NE(dot.find("dynamic capture=disabled"), std::string::npos);
    EXPECT_EQ(dot.find("structured child"), std::string::npos);
    auto fresh = flow.enqueue([&]() -> int { return flow.enqueue([]() -> int { return 0; }).wait(); });
    EXPECT_EQ(fresh.wait(), 0);
    EXPECT_EQ(flow.setGraphCapture(false), 0);
    EXPECT_EQ(flow.dumpDot(fresh, "flow-window.dot"), 0);
    EXPECT_EQ(au::file::read("flow-window.dot", dot), 0);
    EXPECT_NE(dot.find("structured child"), std::string::npos);
    EXPECT_EQ(au::file::removeFile("flow-window.dot"), 0);
}
