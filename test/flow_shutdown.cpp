#include "flow/xflow.h"
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

// Separate process because shutdown is irreversible for each algorithm .so.
int main()
{
    auto& flow = au::flow::XFlow::get();
    std::atomic<bool> entered{false}, release{false};
    std::atomic<unsigned int> accepted{0}, rejected{0}, executed{0}, children{0};
    const auto root = flow.enqueue([&]() -> int {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto child = flow.enqueue([&]() -> int { children.fetch_add(1, std::memory_order_relaxed); return 0; });
        if (child.submissionCode()) return child.submissionCode();
        return flow.parallelizeTiledTasks(37, 2, [&](std::size_t, std::size_t count) {
            children.fetch_add(static_cast<unsigned int>(count), std::memory_order_relaxed);
        });
    });
    if (root.submissionCode()) return 1;
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    int closed = 1;
    std::thread closing([&] { closed = flow.shutdown(); });
    std::array<std::thread, 4> submitters;
    for (auto& thread : submitters) thread = std::thread([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto task = flow.enqueue([&]() -> int { executed.fetch_add(1, std::memory_order_relaxed); return 0; });
            if (task.submissionCode()) {
                if (task.submissionCode() == au::err::kErrorStateInvalid) rejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (auto& thread : submitters) thread.join();
    release.store(true, std::memory_order_release);
    closing.join();
    if (closed || rejected.load(std::memory_order_relaxed) != 4 ||
        accepted.load(std::memory_order_relaxed) != executed.load(std::memory_order_relaxed)) return 2;
    au::flow::TaskResult result{};
    if (flow.wait(root, &result) || result.mCode || children.load(std::memory_order_relaxed) != 38) return 3;
    return flow.shutdown();
}
