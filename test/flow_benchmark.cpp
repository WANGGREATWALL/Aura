#include "flow/xflow.h"
#include "perf/xtimer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

// Optional standalone host measurements, never a performance assertion in CTest.
// Each reported sample includes enqueue + completion + handle release. It is
// not an Android/GPU/energy measurement or an estimate of image-operator speedup.
int main()
{
    au::perf::Config::get().setEnabled(false); // Measure binary trace cost without independent perf log/trace_marker I/O.
    std::printf("host_only_perf_output=off_samples=7_tasks_per_sample=512\n");
    auto& flow = au::flow::XFlow::get();
    std::vector<au::flow::Task> tasks; tasks.reserve(512);
    std::atomic<unsigned int> calls{0};
    auto batch = [&](std::size_t count) -> bool {
        for (std::size_t i = 0; i < count; ++i) {
            tasks.push_back(flow.enqueue([&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; }));
            if (tasks.back().submissionCode()) return false;
        }
        for (const auto& task : tasks) { au::flow::TaskResult result{}; if (flow.wait(task, &result) || result.mCode) return false; }
        tasks.clear(); return true;
    };
    const auto coldStart = std::chrono::steady_clock::now();
    if (!batch(512)) return 1;
    const auto coldNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - coldStart).count();
    std::printf("cold_512_tasks_total_ns=%lld\n", static_cast<long long>(coldNs));
    for (bool tracing : {false, true}) {
        if (flow.setTracing(tracing)) return 2;
        std::array<double, 7> samples{};
        for (auto& sample : samples) {
            const auto start = std::chrono::steady_clock::now();
            if (!batch(512)) return 3;
            sample = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()) / 512.0;
        }
        std::sort(samples.begin(), samples.end());
        std::printf("warm_512_tasks_trace_%s_ns_per_task_min=%.1f median=%.1f max=%.1f\n",
            tracing ? "on" : "off", samples.front(), samples[samples.size() / 2], samples.back());
    }
    if (flow.setTracing(false)) return 4;
    std::vector<unsigned int> pixels(1920 * 1080, 0);
    for (std::size_t rows : {std::size_t(1), std::size_t(16), std::size_t(64)}) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned int repeat = 0; repeat < 16; ++repeat) {
            auto task = flow.enqueue([&]() -> int {
                return flow.parallelizeTiledTasks(1080, rows, [&](std::size_t begin, std::size_t count) {
                    for (std::size_t y = begin; y < begin + count; ++y)
                        for (std::size_t x = 0; x < 1920; ++x) pixels[y * 1920 + x] += 1;
                });
            });
            au::flow::TaskResult result{}; if (flow.wait(task, &result) || result.mCode) return 5;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("tiled_1920x1080_rows_%zu_ns_per_frame=%.1f\n", rows, static_cast<double>(elapsed) / 16.0);
    }
    if (pixels.front() != 48 || pixels.back() != 48 || calls.load(std::memory_order_relaxed) != 15 * 512) return 6;
    return flow.shutdown();
}
