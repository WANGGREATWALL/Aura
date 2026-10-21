#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include "flow/completion.h"
#include "flow/xflow.h"
#include "perf/xtimer.h"

namespace {
typedef std::chrono::steady_clock Clock;
struct Event
{
    Clock::time_point         mStart, mEnd;
    std::atomic<unsigned int> mQueries{0}, mReleased{0};
};
struct Queue
{
    std::mutex        mMutex;
    Clock::time_point mEnd;
    void              enqueue(Event& event)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        event.mStart = std::max(Clock::now(), mEnd);
        event.mEnd   = event.mStart + std::chrono::microseconds(500);
        mEnd         = event.mEnd;
    }
};
int submit(Queue& queue, Event& event)
{
    const au::flow::detail::ExternalOps ops{
        [](void* value, bool* done, au::flow::detail::ExternalStatus*) noexcept -> int {
            auto& e = *static_cast<Event*>(value);
            e.mQueries.fetch_add(1, std::memory_order_relaxed);
            *done = Clock::now() >= e.mEnd;
            return 0;
        },
        nullptr,
        [](void* value) noexcept { static_cast<Event*>(value)->mReleased.fetch_add(1, std::memory_order_relaxed); }};
    au::flow::detail::ExternalToken* token = nullptr;
    const int                        code  = au::flow::detail::prepareExternal(ops, &event, &token);
    if (code)
        return code;
    if (!token)
        return au::err::kErrorStateInvalid;
    queue.enqueue(event);
    au::flow::detail::pollExternal(token);
    au::flow::detail::leaveExternalRegistrar(token);
    return 0;
}
int scenario(bool dag, std::vector<std::uint64_t>& gaps, std::vector<std::uint64_t>& frames, std::uint64_t& queries)
{
    std::array<Event, 5>          events;
    Queue                         queue;
    auto&                         flow = au::flow::XFlow::get();
    std::array<au::flow::Task, 5> tasks;
    const auto                    start = Clock::now();
    if (dag)
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            auto body = [&, i]() -> int { return submit(queue, events[i]); };
            tasks[i]  = i ? flow.enqueueAfter(tasks[i - 1], body) : flow.enqueue(body);
        }
    else
        tasks[0] = flow.enqueue([&]() -> int {
            for (auto& event : events) {
                const int code = submit(queue, event);
                if (code)
                    return code;
            }
            return 0;
        });
    int result = 0;
    for (const auto& task : tasks)
        if (task.valid()) {
            const int code = task.wait();
            if (!result)
                result = code;
        }
    if (result)
        return result;
    frames.push_back(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].mReleased.load(std::memory_order_relaxed) != 1)
            return 1;
        queries += events[i].mQueries.load(std::memory_order_relaxed);
        if (i)
            gaps.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(events[i].mStart - events[i - 1].mEnd).count()));
    }
    return 0;
}
}  // namespace
int main()
{
    au::perf::Config::get().setEnabled(false);
    auto& flow = au::flow::XFlow::get();
    (void)flow.setTracing(false);
    std::printf(
        "scope=host_synthetic_device_fifo hardware_gpu=unrun same_host_clock planned_deadlines_not_device_profiles "
        "stages=5 work_us=500\n");
    for (bool dag : {false, true}) {
        std::vector<std::uint64_t> gaps, frames;
        std::uint64_t              queries = 0;
        for (int i = 0; i < 3; ++i)
            if (scenario(dag, gaps, frames, queries))
                return 1;
        gaps.clear();
        frames.clear();
        queries = 0;
        for (int i = 0; i < 31; ++i)
            if (scenario(dag, gaps, frames, queries))
                return 2;
        std::sort(gaps.begin(), gaps.end());
        std::sort(frames.begin(), frames.end());
        std::printf(
            "mode=%s samples=31 gaps=%zu gap_p50_ns=%llu gap_p95_ns=%llu host_total_p50_ns=%llu host_total_p95_ns=%llu "
            "queries=%llu\n",
            dag ? "ordinary_task_dag" : "single_task_native_fifo_model", gaps.size(),
            static_cast<unsigned long long>(gaps[gaps.size() / 2]),
            static_cast<unsigned long long>(gaps[gaps.size() * 95 / 100]), static_cast<unsigned long long>(frames[15]),
            static_cast<unsigned long long>(frames[29]), static_cast<unsigned long long>(queries));
    }
    return flow.shutdown();
}
