// Production-linked mechanism experiment. C++ allocation accounting is separate
// from timing; malloc/driver residency is not inferred from these counters.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "flow/external_token.h"
#include "flow/payload_pool.h"
#include "flow/slab.h"
#include "flow_reference_allocators.h"
#include "flow_reference_queue.h"
#include "perf/xtimer.h"

namespace {
std::atomic<bool>        gCounting{false};
std::atomic<std::size_t> gCalls{0}, gBytes{0}, gLive{0}, gPeak{0};
struct Allocation
{
    void*       mBase;
    std::size_t mSize;
    bool        mCounted;
};
void* allocate(std::size_t size, std::size_t alignment)
{
    alignment = std::max(alignment, alignof(std::max_align_t));
    if (size > SIZE_MAX - alignment - sizeof(Allocation))
        throw std::bad_alloc();
    void* base = std::malloc(size + alignment + sizeof(Allocation));
    if (!base)
        throw std::bad_alloc();
    const auto begin    = reinterpret_cast<std::uintptr_t>(base) + sizeof(Allocation);
    const auto address  = (begin + alignment - 1) & ~(alignment - 1);
    auto*      info     = reinterpret_cast<Allocation*>(address) - 1;
    const bool counting = gCounting.load(std::memory_order_relaxed);
    ::new (info) Allocation{base, size, counting};
    if (counting) {
        gCalls.fetch_add(1, std::memory_order_relaxed);
        gBytes.fetch_add(size, std::memory_order_relaxed);
        const auto live = gLive.fetch_add(size, std::memory_order_relaxed) + size;
        auto       peak = gPeak.load(std::memory_order_relaxed);
        while (peak < live && !gPeak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
        }
    }
    return reinterpret_cast<void*>(address);
}
void deallocate(void* value) noexcept
{
    if (!value)
        return;
    auto* info = reinterpret_cast<Allocation*>(value) - 1;
    if (info->mCounted)
        gLive.fetch_sub(info->mSize, std::memory_order_relaxed);
    std::free(info->mBase);
}
void counting(bool enabled)
{
    if (enabled) {
        gCalls.store(0, std::memory_order_relaxed);
        gBytes.store(0, std::memory_order_relaxed);
        gPeak.store(gLive.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    gCounting.store(enabled, std::memory_order_relaxed);
}
std::size_t pss()
{
#if defined(__linux__)
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen("/proc/self/smaps_rollup", "r"), &std::fclose);
    if (file) {
        char          line[128];
        unsigned long kb = 0;
        while (std::fgets(line, sizeof(line), file.get()))
            if (std::sscanf(line, "Pss: %lu kB", &kb) == 1)
                return kb;
    }
#endif
    return 0;
}
typedef std::chrono::steady_clock Clock;
std::uint64_t                     ns(Clock::duration time)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time).count());
}

template <class Q>
bool queue(const char* name)
{
    constexpr std::size_t                          count = 16384;
    Q                                              queue;
    std::unique_ptr<au::flow::detail::ReadyLink[]> links(new au::flow::detail::ReadyLink[count]);
    std::array<std::uint64_t, 31>                  samples{};
    for (auto& sample : samples) {
        const auto start = Clock::now();
        for (std::size_t i = 0; i < count; ++i)
            queue.push(&links[i]);
        for (std::size_t i = 0; i < count; ++i) {
            au::flow::detail::ReadyLink* out = nullptr;
            if (!queue.pop(out, (i & 1) != 0))
                return false;
        }
        sample = ns(Clock::now() - start);
    }
    std::sort(samples.begin(), samples.end());
    std::printf("queue=%s mode=burst links=%zu samples=31 p50_ns=%llu p95_ns=%llu queue_bytes=%zu\n", name, count,
                static_cast<unsigned long long>(samples[15]), static_cast<unsigned long long>(samples[29]), sizeof(Q));
    struct Item
    {
        Clock::time_point         mPublished;
        std::atomic<unsigned int> mSeen{0};
    };
    std::unique_ptr<Item[]> items(new Item[count]);
    for (std::size_t i = 0; i < count; ++i)
        links[i].mValue = &items[i];
    std::atomic<bool>                         start{false};
    std::atomic<std::size_t>                  consumed{0};
    std::array<std::vector<std::uint64_t>, 4> latencies;
    for (auto& values : latencies)
        values.reserve(count);
    std::array<std::thread, 8> threads;
    for (std::size_t p = 0; p < 4; ++p)
        threads[p] = std::thread([&, p] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::size_t i = p; i < count; i += 4) {
                items[i].mPublished = Clock::now();
                queue.push(&links[i]);
            }
        });
    for (std::size_t c = 0; c < 4; ++c)
        threads[c + 4] = std::thread([&, c] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (consumed.load(std::memory_order_relaxed) < count) {
                au::flow::detail::ReadyLink* out = nullptr;
                if (!queue.pop(out, (latencies[c].size() & 1) != 0)) {
                    std::this_thread::yield();
                    continue;
                }
                auto* item = static_cast<Item*>(out->mValue);
                latencies[c].push_back(ns(Clock::now() - item->mPublished));
                item->mSeen.fetch_add(1, std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    counting(true);
    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();
    const auto elapsed = ns(Clock::now() - begin);
    counting(false);
    for (std::size_t i = 0; i < count; ++i)
        if (items[i].mSeen.load(std::memory_order_relaxed) != 1)
            return false;
    const auto                 calls = gCalls.load(std::memory_order_relaxed);
    std::vector<std::uint64_t> merged;
    merged.reserve(count);
    for (const auto& values : latencies)
        merged.insert(merged.end(), values.begin(), values.end());
    std::sort(merged.begin(), merged.end());
    std::printf(
        "queue=%s mode=mpmc producers=4 consumers=4 links=%zu elapsed_ns=%llu latency_p50_ns=%llu latency_p95_ns=%llu "
        "cpp_allocs=%zu correct=1\n",
        name, count, static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(merged[count / 2]),
        static_cast<unsigned long long>(merged[count * 95 / 100]), calls);
    return calls == 0;
}

template <class T, std::size_t Page, class Maker>
void records(const char* name, Maker maker)
{
    constexpr std::size_t count = 4096;
    std::vector<T*>       values(count);
    for (bool pooled : {true, false}) {
        const auto before = pss();
        counting(true);
        const auto  start    = Clock::now();
        std::size_t released = 0, retained = 0;
        {
            au::flow::detail::Slab<T, Page>          pool;
            au::flow::detail::SystemRecords<T, Page> plain;
            for (auto& value : values)
                value = pooled ? maker(pool) : maker(plain);
            const auto peak = gPeak.load(std::memory_order_relaxed);
            for (auto* value : values) {
                if (pooled)
                    pool.destroy(value);
                else
                    plain.destroy(value);
            }
            retained           = gLive.load(std::memory_order_relaxed);
            const auto elapsed = ns(Clock::now() - start);
            const auto calls = gCalls.load(std::memory_order_relaxed), bytes = gBytes.load(std::memory_order_relaxed);
            const auto resident = pss();
            counting(false);
            std::printf(
                "allocator=%s backend=%s count=%zu object_bytes=%zu elapsed_ns=%llu calls=%zu allocated_bytes=%zu "
                "peak_cpp_bytes=%zu retained_cpp_bytes=%zu pss_before_kb=%zu pss_after_release_kb=%zu\n",
                name, pooled ? "pooled" : "system", count, sizeof(T), static_cast<unsigned long long>(elapsed), calls,
                bytes, peak, retained, before, resident);
        }
        released = gLive.load(std::memory_order_relaxed);
        std::printf("allocator=%s backend=%s after_allocator_destroy_cpp_bytes=%zu pss_after_destroy_kb=%zu\n", name,
                    pooled ? "pooled" : "system", released, pss());
        if (released)
            std::abort();
    }
}
void payloads()
{
    std::vector<void*> values(2048);
    for (std::size_t size : {32u, 128u, 768u, 3072u, 8192u})
        for (bool pooled : {true, false}) {
            const auto before = pss();
            counting(true);
            const auto start = Clock::now();
            {
                au::flow::detail::PayloadPool            pool;
                au::flow::detail::SystemPayloadAllocator plain;
                for (auto& value : values) {
                    value = pooled ? pool.allocate(size, alignof(std::max_align_t))
                                   : plain.allocate(size, alignof(std::max_align_t));
                    std::memset(value, 0x5a, size);
                }
                for (auto* value : values) {
                    if (pooled)
                        pool.release(value, size, alignof(std::max_align_t));
                    else
                        plain.release(value, size, alignof(std::max_align_t));
                }
                counting(false);
                std::printf(
                    "allocator=payload backend=%s size=%zu count=%zu elapsed_ns=%llu calls=%zu allocated_bytes=%zu "
                    "peak_cpp_bytes=%zu retained_cpp_bytes=%zu pss_before_kb=%zu pss_after_release_kb=%zu\n",
                    pooled ? "pooled" : "system", size, values.size(),
                    static_cast<unsigned long long>(ns(Clock::now() - start)), gCalls.load(std::memory_order_relaxed),
                    gBytes.load(std::memory_order_relaxed), gPeak.load(std::memory_order_relaxed),
                    gLive.load(std::memory_order_relaxed), before, pss());
            }
            if (gLive.load(std::memory_order_relaxed))
                std::abort();
        }
}
}  // namespace
void* operator new(std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void* operator new[](std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept { deallocate(value); }
void operator delete[](void* value) noexcept { deallocate(value); }
void operator delete(void* value, std::size_t) noexcept { deallocate(value); }
void operator delete[](void* value, std::size_t) noexcept { deallocate(value); }
void operator delete(void* value, std::align_val_t) noexcept { deallocate(value); }
void operator delete[](void* value, std::align_val_t) noexcept { deallocate(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { deallocate(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { deallocate(value); }
int  main()
{
    au::perf::Config::get().setEnabled(false);
    std::printf(
        "scope=host_mechanism cpp_bytes_exclude_allocator_headers_and_driver pss_is_process_residency node_bytes=%zu "
        "edge_bytes=%zu external_bytes=%zu\n",
        sizeof(au::flow::detail::Node), sizeof(au::flow::detail::Edge), sizeof(au::flow::detail::ExternalToken));
    if (!queue<au::flow::detail::BasicReadyQueue<std::uint64_t, 256>>("ring_mpsc") ||
        !queue<au::flow::detail::MutexReadyQueue>("mutex_fifo"))
        return 1;
    records<au::flow::detail::Node, 128>("node",
                                         [](auto& a) { return a.create(nullptr, 1, au::flow::detail::Kind::Work); });
    records<au::flow::detail::Edge, 256>("edge", [](auto& a) { return a.create(); });
    records<au::flow::detail::ExternalToken, 64>("external", [](auto& a) { return a.create(); });
    payloads();
    return 0;
}
