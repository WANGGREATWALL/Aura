#include "flow/xflow.h"
#include "flow/completion.h"
#include "sys/flow_runtime.h"
#include "perf/xtimer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#endif
namespace {
std::size_t workers()
{
    std::array<au::flow::WorkerInfo, 8> info{};
    std::size_t count = 0;
    (void)au::flow::XFlow::get().getWorkers(info.data(), info.size(), count);
    return count;
}
void snapshot(const char* stage)
{
    std::printf("stage=%s worker_records=%zu", stage, workers());
#if defined(__linux__)
    for (const char* path : {"/proc/self/status", "/proc/self/smaps_rollup"}) {
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "r"), &std::fclose);
        if (!file) continue;
        char line[256];
        while (std::fgets(line, sizeof(line), file.get())) {
            unsigned long value = 0;
            if (std::sscanf(line, "Threads: %lu", &value) == 1) std::printf(" process_threads=%lu", value);
            if (std::sscanf(line, "Pss: %lu kB", &value) == 1) std::printf(" process_pss_kb=%lu", value);
        }
    }
#endif
    std::printf("\n");
}
void window(const char* stage, std::atomic<unsigned int>& queries)
{
    const auto initial = queries.load(std::memory_order_relaxed);
#if defined(__linux__)
    ::rusage before{}, after{};
    (void)::getrusage(RUSAGE_SELF, &before);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("window=%s ms=200 modeled_queries=%u", stage, queries.load(std::memory_order_relaxed) - initial);
#if defined(__linux__)
    (void)::getrusage(RUSAGE_SELF, &after);
    std::printf(" process_voluntary_context_switches=%ld process_involuntary_context_switches=%ld",
        after.ru_nvcsw - before.ru_nvcsw, after.ru_nivcsw - before.ru_nivcsw);
#endif
    std::printf("\n");
}
}
int main(int argc, char** argv)
{
    (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool changeBudget = argc < 2 || std::strcmp(argv[1], "--skip-budget-change") != 0;
    auto& flow = au::flow::XFlow::get();
    au::perf::Config::get().setEnabled(false);
    (void)flow.setTracing(false);
    std::printf("scope=host_cpu_modeled_external_event context_switches_are_not_wakeups gpu_hardware=unrun\n");
    std::atomic<bool> release{false};
    std::atomic<unsigned int> entered{0};
    std::vector<std::uint32_t> allowed;
    (void)au::sys::flow::allowedCpus(allowed);
    const auto warmCount = std::min<std::size_t>(3, allowed.size() > 1 ? allowed.size() - 1 : 0);
    std::array<au::flow::Task, 3> warm;
    for (std::size_t i = 0; i < warmCount; ++i) warm[i] = flow.enqueue([&]() -> int {
        entered.fetch_add(1, std::memory_order_relaxed);
        while (!release.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(100));
        return 0;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (entered.load(std::memory_order_relaxed) < warmCount && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    struct Event { std::atomic<bool> mDone{false}; std::atomic<unsigned int> mQueries{0}; } event;
    au::flow::detail::ExternalOps ops{};
    ops.mPoll = [](void* payload, bool* done, au::flow::detail::ExternalStatus*) noexcept -> int {
        auto& value = *static_cast<Event*>(payload);
        value.mQueries.fetch_add(1, std::memory_order_relaxed);
        *done = value.mDone.load(std::memory_order_acquire);
        return 0;
    };
    ops.mRelease = [](void*) noexcept {};
    // Warm workers return to dispatch while an event is pending: this exercises
    // the old multiple-timer behavior instead of leaving all followers asleep.
    auto pending = flow.enqueue([&]() -> int {
        au::flow::detail::ExternalToken* token = nullptr;
        const int code = au::flow::detail::prepareExternal(ops, &event, &token);
        if (code) return code;
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        return 0;
    });
    struct Drain {
        Event* mEvent;
        std::atomic<bool>* mRelease;
        std::array<au::flow::Task, 3>* mWarm;
        au::flow::Task* mPending;
        ~Drain()
        {
            mRelease->store(true, std::memory_order_release);
            mEvent->mDone.store(true, std::memory_order_release);
            for (const auto& task : *mWarm) if (task.valid()) (void)task.wait();
            (void)mPending->wait();
        }
    } drain{&event, &release, &warm, &pending};
    const auto queryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!event.mQueries.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < queryDeadline)
        std::this_thread::yield();
    release.store(true, std::memory_order_release);
    for (const auto& task : warm) if (task.valid() && task.waitFor(5000)) return 1;
    if (!event.mQueries.load(std::memory_order_relaxed)) return 6;
    window("external_pending", event.mQueries);
    std::vector<std::uint32_t> cpus;
    if (changeBudget && !au::sys::flow::affinitySupported() && !au::sys::flow::allowedCpus(cpus) && !cpus.empty()) {
        const au::flow::AffinityPolicy one{au::flow::AffinityMode::ExplicitCpuSet, cpus.data(), 1};
        if (flow.setAffinity(one)) return 2;
        window("budget_one_pending", event.mQueries);
    }
    const auto readyAt = std::chrono::steady_clock::now();
    event.mDone.store(true, std::memory_order_release);
    const int completion = pending.waitFor(5000);
    const bool stalled = completion == au::err::kErrorTimeout;
    if (stalled) {
        std::printf("modeled_completion_timeout=1 recovery=real_cpu_task\n");
        auto pulse = flow.enqueue([]() -> int { return 0; });
        if (pulse.waitFor(5000) || pending.waitFor(5000)) return 3;
    } else if (completion) return 3;
    std::printf("modeled_completion_valid=%d\n", stalled ? 0 : 1);
    std::printf("modeled_ready_to_task_ns=%lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - readyAt).count()));
    const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
    if (flow.setAffinity(restore)) return 4;
    snapshot("before_idle");
    window("idle", event.mQueries);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    snapshot("after_idle_2300ms");
    const auto start = std::chrono::steady_clock::now();
    if (flow.enqueue([]() -> int { return 0; }).waitFor(5000)) return 5;
    std::printf("restart_task_ns=%lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()));
    const int closed = flow.shutdown();
    return stalled ? 7 : closed;
}
