#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "flow/completion.h"
#include "flow/testing.h"
#include "flow/xflow.h"
#include "perf/xtimer.h"
namespace {
int scenario(std::size_t width, unsigned int depth)
{
    auto& flow = au::flow::XFlow::get();
    struct Drain
    {
        std::atomic<bool> mDone{false};
        ~Drain()
        {
            mDone.store(true, std::memory_order_release);
            (void)au::flow::XFlow::get().waitAll();
        }
    } drain;
    std::atomic<std::size_t>            entered{0};
    const au::flow::detail::ExternalOps ops{
        [](void* payload, bool* done, au::flow::detail::ExternalStatus*) noexcept -> int {
            *done = static_cast<Drain*>(payload)->mDone.load(std::memory_order_acquire);
            return 0;
        },
        nullptr, [](void*) noexcept {}};
    const auto body = [&]() -> int {
        au::flow::detail::ExternalToken* token = nullptr;
        const int                        code  = au::flow::detail::prepareExternal(ops, &drain, &token);
        if (code)
            return code;
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        entered.fetch_add(1, std::memory_order_release);
        return 0;
    };
    au::flow::Tasks graph;
    for (std::size_t i = 0; i < width; ++i) {
        const int code = graph.add(flow.makeTask(body));
        if (code)
            return code;
    }
    const auto                      target = flow.enqueue(graph);
    std::array<au::flow::Task, 256> unrelated;
    for (auto& task : unrelated)
        task = flow.enqueue(body);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (entered.load(std::memory_order_acquire) != width + unrelated.size() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (entered.load(std::memory_order_acquire) != width + unrelated.size())
        return 1;
    std::array<std::uint64_t, 31>    samples{};
    au::flow::testing::GraphStats    observed;
    std::function<int(unsigned int)> nested;
    nested = [&](unsigned int remaining) -> int {
        if (remaining)
            return flow.enqueue([&, remaining]() -> int { return nested(remaining - 1); }).wait();
        for (unsigned int i = 0; i < 5; ++i)
            if (flow.waitFor(target, 0) != au::err::kErrorTimeout)
                return 2;
        (void)au::flow::testing::graphStats(true);
        for (auto& sample : samples) {
            const auto start = std::chrono::steady_clock::now();
            if (flow.waitFor(target, 0) != au::err::kErrorTimeout)
                return 3;
            sample = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }
        observed = au::flow::testing::graphStats();
        return 0;
    };
    const int code = flow.enqueue([&]() -> int { return nested(depth); }).wait();
    if (code)
        return code;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "width=%zu nested_depth=%u unrelated=%zu samples=%zu p50_ns=%llu p95_ns=%llu search_count=%llu "
        "visited_nodes=%llu edges=%llu active_scans=%llu search_ns=%llu max_search_ns=%llu wait_checks=%llu "
        "wait_lock_ns=%llu max_wait_lock_ns=%llu node_bytes=%zu\n",
        width, depth, unrelated.size(), samples.size(), static_cast<unsigned long long>(samples[15]),
        static_cast<unsigned long long>(samples[29]), static_cast<unsigned long long>(observed.mSearches),
        static_cast<unsigned long long>(observed.mNodes), static_cast<unsigned long long>(observed.mEdges),
        static_cast<unsigned long long>(observed.mActiveScans),
        static_cast<unsigned long long>(observed.mSearchNanoseconds),
        static_cast<unsigned long long>(observed.mMaximumNanoseconds),
        static_cast<unsigned long long>(observed.mWaitChecks),
        static_cast<unsigned long long>(observed.mWaitLockNanoseconds),
        static_cast<unsigned long long>(observed.mMaximumWaitLockNanoseconds), observed.mNodeBytes);
    return 0;
}
}  // namespace
int main()
{
    auto& flow = au::flow::XFlow::get();
    au::perf::Config::get().setEnabled(false);
    if (au::flow::testing::setBudget(1))
        return 1;
    std::puts("scope=instrumented_host_completion_graph not_production_latency gpu_hardware=unrun");
    for (const auto width : {32u, 128u, 512u})
        for (const auto depth : {0u, 7u}) {
            const int code = scenario(width, depth);
            if (code)
                return code;
        }
    return flow.shutdown();
}
