#include "flow/xflow.h"
#include "perf/xtimer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
std::atomic<bool> gCounting{false};
std::atomic<std::uint64_t> gAllocations{0};
std::atomic<std::uint64_t> gBytes{0};

void countAllocation(std::size_t size) noexcept
{
    if (gCounting.load(std::memory_order_relaxed)) {
        gAllocations.fetch_add(1, std::memory_order_relaxed);
        gBytes.fetch_add(size, std::memory_order_relaxed);
    }
}

void* allocate(std::size_t size)
{
    if (void* data = std::malloc(size ? size : 1)) {
        countAllocation(size);
        return data;
    }
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::size_t alignment)
{
    void* data = nullptr;
#if defined(_WIN32)
    data = ::_aligned_malloc(size ? size : 1, alignment);
#else
    if (size > SIZE_MAX - (alignment - 1)) throw std::bad_alloc();
    const std::size_t rounded = std::max(alignment, (size + alignment - 1) & ~(alignment - 1));
    data = std::aligned_alloc(alignment, rounded);
#endif
    if (!data) throw std::bad_alloc();
    countAllocation(size);
    return data;
}

void freeAligned(void* data) noexcept
{
#if defined(_WIN32)
    ::_aligned_free(data);
#else
    std::free(data);
#endif
}

typedef std::chrono::steady_clock Clock;
constexpr std::size_t kSamples = 101;
constexpr std::size_t kWarmups = 16;
constexpr std::size_t kAllocationSamples = 25;
constexpr std::size_t kWidth = 1920;
constexpr std::size_t kHeight = 1080;

template<class F> bool measure(const char* name, F&& operation)
{
    for (std::size_t i = 0; i < kWarmups; ++i) if (operation()) return false;
    std::array<double, kSamples> samples{};
    for (double& sample : samples) {
        const auto start = Clock::now();
        const int code = operation();
        sample = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        if (code) return false;
    }
    std::sort(samples.begin(), samples.end());
    gAllocations.store(0, std::memory_order_relaxed);
    gBytes.store(0, std::memory_order_relaxed);
    gCounting.store(true, std::memory_order_release);
    int code = 0;
    for (std::size_t i = 0; i < kAllocationSamples && !code; ++i) code = operation();
    gCounting.store(false, std::memory_order_release);
    if (code) return false;
    std::printf("case=%s samples=%zu p50_ns=%.0f p95_ns=%.0f max_ns=%.0f cpp_allocs_per_call=%.3f cpp_bytes_per_call=%.3f\n",
        name, samples.size(), samples[50], samples[95], samples.back(),
        static_cast<double>(gAllocations.load(std::memory_order_relaxed)) / kAllocationSamples,
        static_cast<double>(gBytes.load(std::memory_order_relaxed)) / kAllocationSamples);
    return true;
}

void process(const std::vector<std::uint32_t>& input, std::vector<std::uint32_t>& output,
             std::size_t begin, std::size_t count, unsigned int lane)
{
    for (std::size_t y = begin; y < begin + count; ++y) {
        for (std::size_t x = y * kWidth; x < (y + 1) * kWidth; ++x) {
            output[x] = (input[x] ^ (lane + 1)) + 17u * static_cast<unsigned int>(x & 255);
        }
    }
}

bool verify(const std::vector<std::uint32_t>& input, const std::vector<std::uint32_t>& output, unsigned int lane)
{
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (output[i] != (input[i] ^ (lane + 1)) + 17u * static_cast<unsigned int>(i & 255)) return false;
    }
    return true;
}

void idleSample(au::flow::XFlow& flow)
{
    std::size_t workers = 0;
    std::array<au::flow::WorkerInfo, 8> info{};
    (void)flow.getWorkers(info.data(), info.size(), workers);
#if defined(__linux__)
    ::rusage before{}, after{};
    const bool beforeOk = ::getrusage(RUSAGE_SELF, &before) == 0;
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::printf("idle_window_ms=150 aura_worker_records=%zu", workers);
#if defined(__linux__)
    const bool afterOk = ::getrusage(RUSAGE_SELF, &after) == 0;
    if (beforeOk && afterOk) {
        std::printf(" process_voluntary_context_switches=%ld process_involuntary_context_switches=%ld",
            after.ru_nvcsw - before.ru_nvcsw, after.ru_nivcsw - before.ru_nivcsw);
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> rollup(std::fopen("/proc/self/smaps_rollup", "r"), &std::fclose);
    if (rollup) {
        char line[256];
        while (std::fgets(line, sizeof(line), rollup.get())) {
            unsigned long value = 0;
            if (std::sscanf(line, "Pss: %lu kB", &value) == 1) {
                std::printf(" process_pss_kb=%lu", value);
                break;
            }
        }
    }
#endif
    std::printf("\n");
}
} // namespace

// The optional benchmark interposes C++ allocations for the whole process.
// Timing and counting run separately; these counters exclude malloc/driver
// allocations and are not a measure of live memory. There is no CTest SLA.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* data) noexcept { std::free(data); }
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete(void* data, std::size_t) noexcept { std::free(data); }
void operator delete[](void* data, std::size_t) noexcept { std::free(data); }
void* operator new(std::size_t size, std::align_val_t alignment)
{ return allocateAligned(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment)
{ return allocateAligned(size, static_cast<std::size_t>(alignment)); }
void operator delete(void* data, std::align_val_t) noexcept { freeAligned(data); }
void operator delete[](void* data, std::align_val_t) noexcept { freeAligned(data); }
void operator delete(void* data, std::size_t, std::align_val_t) noexcept { freeAligned(data); }
void operator delete[](void* data, std::size_t, std::align_val_t) noexcept { freeAligned(data); }

int main()
{
    au::perf::Config::get().setEnabled(false);
    auto& flow = au::flow::XFlow::get();
    std::printf("scope=host_cpu diagnostics=removed perf_output=off timer=steady_clock samples=%zu allocation_samples=%zu\n",
        kSamples, kAllocationSamples);
    const auto cold = Clock::now();
    if (flow.parallelizeTiledTasks(1080, 16, [](std::size_t, std::size_t) {})) return 2;
    std::printf("cold_external_empty_1080x16_ns=%lld\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - cold).count()));
    auto empty = [&](std::size_t range) {
        return flow.parallelizeTiledTasks(range, 16, [](std::size_t, std::size_t) {});
    };
    if (!measure("external_empty_range0", [&] { return empty(0); }) ||
        !measure("external_empty_range1", [&] { return empty(1); }) ||
        !measure("external_empty_1080x16", [&] { return empty(1080); })) return 3;
    if (!measure("managed_empty_1080x16", [&] {
        return flow.enqueue([&]() -> int { return empty(1080); }).wait();
    })) return 4;

    std::vector<std::uint32_t> input(kWidth * kHeight);
    std::array<std::vector<std::uint32_t>, 3> output;
    for (auto& lane : output) lane.resize(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint32_t>(i & 0x3fff);
    for (std::size_t tile : {std::size_t(16), std::size_t(64)}) {
        char name[64];
        std::snprintf(name, sizeof(name), "external_rows_1920x1080_tile%zu", tile);
        if (!measure(name, [&] {
            return flow.parallelizeTiledTasks(kHeight, tile, [&](std::size_t first, std::size_t count) {
                process(input, output[0], first, count, 0);
            });
        }) || !verify(input, output[0], 0)) return 5;
    }
    if (!measure("synthetic_cpu_fanout3_fanin", [&] {
        std::array<au::flow::Task, 3> tasks;
        for (unsigned int lane = 0; lane < tasks.size(); ++lane) {
            tasks[lane] = flow.enqueue([&, lane]() -> int {
                process(input, output[lane], 0, kHeight, lane);
                return 0;
            });
        }
        const auto tail = flow.enqueueAfter({tasks[0], tasks[1], tasks[2]}, []() -> int { return 0; });
        int code = tail.wait();
        for (const auto& task : tasks) {
            const int taskCode = task.wait();
            if (!code) code = taskCode;
        }
        return code;
    })) return 6;
    for (unsigned int lane = 0; lane < output.size(); ++lane) if (!verify(input, output[lane], lane)) return 7;
    idleSample(flow);
    std::printf("gpu_chain=unrun FlashPE=unavailable GitLab_backend=unavailable context_switches_are_not_wakeup_counts\n");
    return flow.shutdown();
}
