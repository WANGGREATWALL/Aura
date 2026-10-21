#if ENABLE_TEST_XFLOW
#include "flow/xflow.h"
#include "flow/ready_queue.h"
#include "flow/testing.h"
#include "flow/task_errors.h"
#include "flow/completion.h"
#include "file/xfile.h"
#include "mm/registry.h"
#include "json/xjson.h"
#include "sys/flow_runtime.h"
#include "gtest/gtest.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int addValues(int a, int b, int& out) { out = a + b; return 0; }
struct Module {
    int mValue = 0;
    int apply(int n) { mValue += n; return 0; }
    int read(int& out) const { out = mValue; return 0; }
    static int staticWork(int& out) { out = 19; return 0; }
};
struct Functor { int operator()(int& out) && { out = 23; return 0; } };
int ordinaryTile(std::size_t, std::size_t count) { return count ? 0 : 1; }
int code(const au::flow::Task& task)
{
    au::flow::TaskResult result{};
    const int wait = au::flow::XFlow::get().waitFor(task, 10000, &result);
    return wait ? wait : result.mCode;
}
}

TEST(XFlow, CallableOwnershipAndResults)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    int a = 0, b = 0, c = 0, d = 0;
    Module module;
    EXPECT_EQ(code(flow.enqueue(addValues, 2, 3, std::ref(a))), 0);
    EXPECT_EQ(code(flow.enqueue(&Module::apply, std::ref(module), 7)), 0);
    EXPECT_EQ(code(flow.enqueue(&Module::read, std::cref(module), std::ref(b))), 0);
    EXPECT_EQ(code(flow.enqueue(&Module::staticWork, std::ref(c))), 0);
    EXPECT_EQ(code(flow.enqueue(Functor(), std::ref(d))), 0);
    EXPECT_EQ(a, 5); EXPECT_EQ(b, 7); EXPECT_EQ(c, 19); EXPECT_EQ(d, 23);
    std::unique_ptr<int> owned(new int(31));
    EXPECT_EQ(code(flow.enqueue([](std::unique_ptr<int> value) -> int { return *value == 31 ? 0 : 1; }, std::move(owned))), 0);
    EXPECT_FALSE(owned);
    au::flow::Task failure = flow.enqueue([]() -> int { return 17; });
    au::flow::TaskResult result{};
    EXPECT_EQ(flow.wait(failure, &result), 17);
    EXPECT_EQ(result.mCode, 17);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);
    EXPECT_EQ(result.mCauseTaskId, failure.id());
    au::flow::Task empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(flow.wait(empty), au::err::kErrorInvalidHandle);
}

TEST(XFlow, UnexpectedExceptionAndEqualBusinessCodeKeepTheirOrigins)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::TaskResult result{};
    au::flow::Task throwing = flow.enqueue([]() -> int { throw std::runtime_error("test"); });
    ASSERT_EQ(flow.wait(throwing, &result), au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mCode, au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Flow);
    au::flow::Task business = flow.enqueue([]() -> int { return au::flow::kErrorUnhandledException; });
    ASSERT_EQ(flow.wait(business, &result), au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);
    au::flow::Task tile = flow.enqueue([&]() -> int {
        return flow.parallelizeTiledTasks(16, 1, [](std::size_t, std::size_t) -> int {
            throw std::runtime_error("tile");
        });
    });
    ASSERT_EQ(flow.wait(tile, &result), au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mCode, au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Flow);
}

TEST(XFlow, CompletionConveniencesShareTaskErrorSemantics)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::Task empty, draft = flow.makeTask([]() -> int { return 17; });
    EXPECT_FALSE(empty.ready()); EXPECT_FALSE(draft.ready());
    EXPECT_EQ(empty.code(), au::err::kErrorInvalidHandle);
    EXPECT_EQ(empty.wait(), au::err::kErrorInvalidHandle);
    EXPECT_EQ(draft.code(), au::err::kErrorNotReady);
    EXPECT_EQ(draft.waitFor(0), au::err::kErrorNotReady);
    au::flow::TaskResult result{};
    ASSERT_EQ(flow.enqueue(draft).wait(&result), 17);
    EXPECT_TRUE(draft.ready()); EXPECT_EQ(draft.code(), 17);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);
    EXPECT_EQ(result.mCauseTaskId, draft.id());
    EXPECT_EQ(flow.wait(draft), 17);
    EXPECT_EQ(flow.waitFor(draft, 0), 17);
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
    au::flow::Task rejected = flow.enqueue([]() -> int { return 0; });
    EXPECT_TRUE(rejected.ready()); EXPECT_EQ(rejected.code(), au::err::kErrorNoMemory);
    EXPECT_EQ(rejected.wait(&result), au::err::kErrorNoMemory);
    EXPECT_EQ(result.mState, au::flow::TaskState::Rejected);
    std::atomic<bool> release{false};
    au::flow::Task pending = flow.enqueue([&]() -> int {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return 23;
    });
    result.mCode = 71;
    EXPECT_FALSE(pending.ready()); EXPECT_EQ(pending.code(), au::err::kErrorNotReady);
    EXPECT_EQ(pending.waitFor(0, &result), au::err::kErrorTimeout);
    EXPECT_EQ(result.mCode, 71); // No completed result was produced.
    release.store(true, std::memory_order_release);
    EXPECT_EQ(pending.wait(), 23);
    au::flow::Tasks group;
    EXPECT_FALSE(group.ready()); EXPECT_EQ(group.code(), au::err::kErrorNotReady);
    ASSERT_EQ(group.add(flow.makeTask([]() -> int { return 29; })), 0);
    ASSERT_EQ(flow.enqueue(group).submissionCode(), 0);
    EXPECT_EQ(group.wait(&result), 29); EXPECT_TRUE(group.ready());
    EXPECT_EQ(group.waitFor(0), 29); EXPECT_EQ(group.code(), 29);
    EXPECT_EQ(flow.wait(group), 29); EXPECT_EQ(flow.waitFor(group, 0), 29);
    EXPECT_EQ(result.mCode, 29);
}

TEST(XFlow, DiamondAndNestedGroups)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    int source = 0, left = 0, right = 0, answer = 0;
    au::flow::Task a = flow.makeTask([&]() -> int { source = 6; return 0; });
    au::flow::Task b = flow.makeTask([&]() -> int { left = source + 1; return 0; });
    au::flow::Task c = flow.makeTask([&]() -> int { right = source * 2; return 0; });
    au::flow::Task d = flow.makeTask([&]() -> int { answer = left + right; return 0; });
    au::flow::Tasks inner, graph;
    ASSERT_EQ(inner.add(b), 0); ASSERT_EQ(inner.add(c), 0);
    ASSERT_EQ(a.precede(inner), 0); ASSERT_EQ(inner.precede(d), 0);
    ASSERT_EQ(graph.add(a), 0); ASSERT_EQ(graph.add(std::move(inner)), 0); ASSERT_EQ(graph.add(d), 0);
    au::flow::Task all = flow.enqueue(graph);
    ASSERT_EQ(all.submissionCode(), 0);
    ASSERT_EQ(code(all), 0); EXPECT_EQ(answer, 19);
    EXPECT_EQ(code(b), 0);
    EXPECT_NE(flow.enqueue(graph).submissionCode(), 0);
    EXPECT_EQ(code(all), 0);
}

TEST(XFlow, ConvenienceBuildersKeepErrorsAndDependencies)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    int source = 0, output = 0;
    au::flow::Task a = flow.makeTask([&]() -> int { source = 7; return 0; });
    au::flow::Task b = flow.makeTask([&]() -> int { output = source * 2; return 0; });
    ASSERT_EQ(a.precede(b), 0);
    au::flow::Tasks graph{a, b};
    EXPECT_EQ(flow.enqueue(graph).wait(), 0); EXPECT_EQ(output, 14);
    std::unique_ptr<int> owned(new int(14));
    EXPECT_EQ(flow.enqueueAfter(graph, [&output](std::unique_ptr<int> value) -> int {
        return output == *value ? 0 : 1;
    }, std::move(owned)).wait(), 0);
    EXPECT_FALSE(owned);

    std::atomic<bool> release{false}; std::atomic<int> calls{0};
    au::flow::Task before = flow.enqueue([&]() -> int {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return 0;
    });
    au::flow::Task after = flow.enqueueAfter(before, [&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; });
    EXPECT_EQ(flow.waitFor(after, 0), au::err::kErrorTimeout);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(after.wait(), 0); EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
    auto body = [&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; };
    au::flow::Task invalid, draft = flow.makeTask([]() -> int { return 0; });
    EXPECT_EQ(flow.enqueueAfter(invalid, body).wait(), au::err::kErrorInvalidHandle);
    EXPECT_EQ(flow.enqueueAfter(draft, body).wait(), au::err::kErrorStateInvalid);
    au::flow::Task failure = flow.enqueue([]() -> int { return 31; });
    au::flow::Task skipped = flow.enqueueAfter(failure, body);
    au::flow::TaskResult result{};
    EXPECT_EQ(flow.wait(skipped, &result), 31);
    EXPECT_EQ(result.mState, au::flow::TaskState::Skipped); EXPECT_EQ(result.mCauseTaskId, failure.id());
    au::flow::testing::failAfter(au::flow::testing::Failure::Edge, 0);
    EXPECT_EQ(flow.enqueueAfter(before, body).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);

    au::flow::Task untouched = flow.makeTask(body);
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
    au::flow::Task rejected = flow.makeTask(body);
    EXPECT_EQ(flow.wait(rejected, &result), au::err::kErrorNoMemory);
    au::flow::Tasks bad{untouched, rejected};
    EXPECT_EQ(flow.enqueue(bad).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(untouched.state(), au::flow::TaskState::Rejected);
    au::flow::Task duplicate = flow.makeTask(body);
    au::flow::Tasks repeated{duplicate, duplicate};
    EXPECT_EQ(flow.enqueue(repeated).wait(), au::err::kErrorStateInvalid);
    au::flow::Tasks owner{draft}, conflicting{draft};
    EXPECT_EQ(flow.enqueue(conflicting).wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(flow.enqueue(owner).wait(), 0); // A failed competing builder does not change the owner.
    au::flow::Tasks alreadySubmitted{before};
    EXPECT_EQ(flow.enqueue(alreadySubmitted).wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(before.wait(), 0);
    EXPECT_EQ(flow.enqueueAfter(rejected, body).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(XFlow, DynamicFanInAndTemporaryGroupsPreserveCompletion)
{
    auto& flow = au::flow::XFlow::get();
    std::atomic<bool> release{false};
    int left = 0, right = 0, result = 0;
    auto a = flow.makeTask([&]() -> int {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        left = 11; return 0;
    });
    auto b = flow.makeTask([&]() -> int {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        right = 13; return 0;
    });
    auto group = flow.enqueue(au::flow::Tasks{a, b}); // Temporary builder must not abandon accepted members.
    Module module;
    auto tail = flow.enqueueAfter({a, b, a}, &Module::apply, std::ref(module), 7);
    auto checked = flow.enqueueAfter({tail, group}, [&]() -> int {
        result = left + right + module.mValue; return 0;
    });
    EXPECT_EQ(checked.waitFor(0), au::err::kErrorTimeout);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(code(checked), 0); EXPECT_EQ(result, 31);
    EXPECT_EQ(code(group), 0); EXPECT_EQ(code(tail), 0);
    EXPECT_EQ(code(flow.enqueueAfter({}, []() -> int { return 0; })), 0);

    std::atomic<int> calls{0};
    auto body = [&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; };
    au::flow::Task invalid, draft = flow.makeTask(body);
    EXPECT_EQ(code(flow.enqueueAfter({a, invalid, b}, body)), au::err::kErrorInvalidHandle);
    EXPECT_EQ(code(flow.enqueueAfter({a, draft}, body)), au::err::kErrorStateInvalid);
    auto failed = flow.enqueue([]() -> int { return 43; });
    auto skipped = flow.enqueueAfter({a, failed, b}, body);
    au::flow::TaskResult error{};
    EXPECT_EQ(skipped.wait(&error), 43);
    EXPECT_EQ(error.mState, au::flow::TaskState::Skipped); EXPECT_EQ(error.mCauseTaskId, failed.id());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(code(flow.enqueue(au::flow::Tasks{flow.makeTask(body), invalid})), au::err::kErrorInvalidHandle);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
}

TEST(XFlow, BuilderFailuresKeepFirstCauseAndNeverChangeAcceptedResults)
{
    auto& flow = au::flow::XFlow::get();
    std::atomic<int> calls{0};
    auto body = [&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; };
    au::flow::Task invalid, task = flow.makeTask(body);
    EXPECT_EQ(task.setName(nullptr), au::err::kErrorNullPointer);
    EXPECT_EQ(invalid.precede(task), au::err::kErrorInvalidHandle); // Each operation still reports its own error.
    EXPECT_EQ(task.precede(task), au::flow::kErrorCycleDetected);
    EXPECT_EQ(code(flow.enqueue(task)), au::err::kErrorNullPointer);
    au::flow::Tasks group;
    EXPECT_EQ(group.handle().setName(nullptr), au::err::kErrorNullPointer);
    EXPECT_EQ(group.add(invalid), au::err::kErrorInvalidHandle);
    EXPECT_EQ(code(flow.enqueue(group)), au::err::kErrorNullPointer);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);

    au::flow::Tasks accepted{flow.makeTask(body)};
    ASSERT_EQ(code(flow.enqueue(accepted)), 0);
    EXPECT_EQ(accepted.add(invalid), au::err::kErrorInvalidHandle);
    EXPECT_EQ(accepted.handle().setName(nullptr), au::err::kErrorStateInvalid);
    EXPECT_EQ(accepted.code(), 0); EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(XFlow, LinearContinuationYieldsToQueuedWorkAndPropagatesFailure)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    std::atomic<bool> entered{false}, release{false};
    std::atomic<unsigned int> calls{0};
    au::flow::Tasks graph;
    std::vector<au::flow::Task> chain;
    chain.reserve(64);
    for (unsigned int i = 0; i < 64; ++i) {
        chain.emplace_back(flow.makeTask([&, i]() -> int {
            if (i == 8) {
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            }
            if (calls.fetch_add(1, std::memory_order_relaxed) != i) return 1;
            return i == 43 ? 37 : 0;
        }));
        ASSERT_EQ(graph.add(chain.back()), 0);
        if (i) ASSERT_EQ(chain[i - 1].precede(chain[i]), 0);
    }
    const au::flow::Task joined = flow.enqueue(graph);
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    unsigned int observed = 0;
    const au::flow::Task competitor = flow.enqueue([&]() -> int {
        observed = calls.load(std::memory_order_relaxed); return 0;
    });
    release.store(true, std::memory_order_release);
    EXPECT_EQ(joined.wait(), 37); EXPECT_EQ(competitor.wait(), 0);
    EXPECT_EQ(observed, 9u); EXPECT_EQ(calls.load(std::memory_order_relaxed), 44u);
    EXPECT_EQ(chain.back().state(), au::flow::TaskState::Skipped);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XFlow, RejectionAndDependencyFailure)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<int> called{0};
    au::flow::Task a = flow.makeTask([]() -> int { return 41; });
    au::flow::Task b = flow.makeTask([&]() -> int { called.fetch_add(1, std::memory_order_relaxed); return 0; });
    ASSERT_EQ(a.precede(b), 0);
    au::flow::Tasks graph;
    ASSERT_EQ(graph.add(a), 0); ASSERT_EQ(graph.add(b), 0);
    EXPECT_EQ(code(flow.enqueue(graph)), 41);
    EXPECT_EQ(b.state(), au::flow::TaskState::Skipped);
    EXPECT_EQ(called.load(std::memory_order_relaxed), 0);
    au::flow::Task x = flow.makeTask([]() -> int { return 0; });
    au::flow::Task y = flow.makeTask([]() -> int { return 0; });
    ASSERT_EQ(x.precede(y), 0);
    EXPECT_EQ(y.precede(x), au::flow::kErrorCycleDetected);
    au::flow::Tasks bad;
    ASSERT_EQ(bad.add(x), 0); ASSERT_EQ(bad.add(y), 0);
    EXPECT_EQ(flow.enqueue(bad).submissionCode(), au::flow::kErrorCycleDetected);
    EXPECT_EQ(x.state(), au::flow::TaskState::Rejected);
    au::flow::Task abandoned;
    { au::flow::Tasks draft; abandoned = flow.makeTask([]() -> int { return 0; }); ASSERT_EQ(draft.add(abandoned), 0); }
    EXPECT_EQ(code(abandoned), au::err::kErrorAborted);
}

TEST(XFlow, DynamicRegistrationRaces)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    for (int round = 0; round < 100; ++round) {
        std::atomic<int> seen{0};
        au::flow::Task before = flow.enqueue([]() -> int { return 0; });
        std::array<std::thread, 4> producers;
        for (auto& producer : producers) producer = std::thread([&]() {
            for (int i = 0; i < 16; ++i) {
                au::flow::Task after = flow.makeTask([&]() -> int { seen.fetch_add(1, std::memory_order_relaxed); return 0; });
                EXPECT_EQ(before.precede(after), 0);
                EXPECT_EQ(flow.enqueue(after).submissionCode(), 0);
            }
        });
        for (auto& producer : producers) producer.join();
        ASSERT_EQ(flow.waitAll(), 0);
        EXPECT_EQ(seen.load(std::memory_order_relaxed), 64);
    }
}

TEST(XFlow, StructuredCompletionAndCooperativeWait)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<int> sum{0};
    std::array<au::flow::Task, 8> parents;
    for (auto& parent : parents) parent = flow.enqueue([&]() -> int {
        au::flow::Task child = flow.enqueue([&]() -> int {
            (void)flow.enqueue([&]() -> int { sum.fetch_add(1, std::memory_order_relaxed); return 0; });
            return 0;
        });
        return code(child);
    });
    for (const auto& parent : parents) EXPECT_EQ(code(parent), 0);
    EXPECT_EQ(sum.load(std::memory_order_relaxed), 8);
    au::flow::Task parent = flow.enqueue([&]() -> int {
        (void)flow.enqueue([]() -> int { return 53; }); return 0;
    });
    EXPECT_EQ(code(parent), 53);
    EXPECT_EQ(code(flow.enqueue([&]() -> int {
        return flow.waitAll() == au::flow::kErrorWouldDeadlock ? 0 : 1;
    })), 0);
}

TEST(XFlow, TiledNestedAndSaturatedWorkers)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<std::size_t> total{0};
    std::array<au::flow::Task, 8> parents;
    for (auto& parent : parents) parent = flow.enqueue([&]() -> int {
        return flow.parallelizeTiledTasks(1003, 17, [&](std::size_t, std::size_t count) -> int {
            return flow.parallelizeTiledTasks(count, 3, [&](std::size_t, std::size_t n) {
                total.fetch_add(n, std::memory_order_relaxed);
            });
        });
    });
    for (const auto& parent : parents) ASSERT_EQ(code(parent), 0);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 8u * 1003u);
    EXPECT_EQ(flow.parallelizeTiledTasks(0, 0, [](std::size_t, std::size_t) {}), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.parallelizeTiledTasks(0, 1, [](std::size_t, std::size_t) {}), 0);
    EXPECT_EQ(flow.parallelizeTiledTasks(SIZE_MAX, SIZE_MAX, [](std::size_t offset, std::size_t count) -> int {
        return offset == 0 && count == SIZE_MAX ? 0 : 1;
    }), 0);
    std::size_t checked = 0;
    XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(1, 1)
        checked = _i + _ti;
    XTHREAD_PARALLELIZE_END
    EXPECT_EQ(checked, 1u);
}

TEST(XFlow, SynchronousTilesBorrowNoncopyableCallbacksAndFunctions)
{
    struct Counter {
        std::atomic<std::size_t>& mTotal;
        explicit Counter(std::atomic<std::size_t>& total) noexcept : mTotal(total) {}
        Counter(const Counter&) = delete;
        Counter& operator=(const Counter&) = delete;
        Counter(Counter&&) = delete;
        Counter& operator=(Counter&&) = delete;
        int operator()(std::size_t, std::size_t count) &
        { mTotal.fetch_add(count, std::memory_order_relaxed); return 0; }
    };
    auto& flow = au::flow::XFlow::get();
    std::atomic<std::size_t> total{0};
    Counter callback(total);
    EXPECT_EQ(flow.parallelizeTiledTasks(1003, 17, callback), 0); // Original object, with no std::ref or copy.
    EXPECT_EQ(total.load(std::memory_order_relaxed), 1003u);
    EXPECT_EQ(flow.parallelizeTiledTasks(101, 3, Counter(total)), 0); // Temporary need not be movable either.
    const auto constant = [&total](std::size_t, std::size_t count) {
        total.fetch_add(count, std::memory_order_relaxed);
    };
    EXPECT_EQ(flow.parallelizeTiledTasks(29, 7, constant), 0);
    EXPECT_EQ(flow.parallelizeTiledTasks(13, 3, std::ref(callback)), 0);
    EXPECT_EQ(flow.parallelizeTiledTasks(11, 3, std::cref(constant)), 0);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 1157u);
    EXPECT_EQ(flow.parallelizeTiledTasks(19, 3, ordinaryTile), 0);
    auto function = &ordinaryTile;
    EXPECT_EQ(flow.parallelizeTiledTasks(19, 3, function), 0);
    EXPECT_EQ(flow.parallelizeTiledTasks(19, 3, &ordinaryTile), 0);
}

TEST(XFlow, ExternalTiledCallerFallsBackAndJoinsHelpers)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(4), 0);
    const auto caller = std::this_thread::get_id();
    std::atomic<std::size_t> covered{0}, callerTiles{0};
    std::atomic<bool> callerEntered{false};
    EXPECT_EQ(flow.parallelizeTiledTasks(1003, 17, [&](std::size_t, std::size_t count) -> int {
        if (std::this_thread::get_id() == caller) {
            callerTiles.fetch_add(1, std::memory_order_relaxed);
            callerEntered.store(true, std::memory_order_release);
        } else {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!callerEntered.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() > deadline) return 1;
                std::this_thread::yield();
            }
        }
        covered.fetch_add(count, std::memory_order_relaxed);
        return 0;
    }), 0);
    EXPECT_GT(callerTiles.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(covered.load(std::memory_order_relaxed), 1003u);
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0); // First helper fails; no root allocation.
    covered.store(0, std::memory_order_relaxed);
    EXPECT_EQ(flow.parallelizeTiledTasks(257, 3, [&](std::size_t, std::size_t count) -> int {
        if (std::this_thread::get_id() != caller) return 2;
        covered.fetch_add(count, std::memory_order_relaxed); return 0;
    }), 0);
    EXPECT_EQ(covered.load(std::memory_order_relaxed), 257u);
    EXPECT_EQ(flow.enqueue([&]() -> int {
        au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
        return flow.parallelizeTiledTasks(19, 1, [](std::size_t, std::size_t) {});
    }).wait(), 0); // Optional helper failure must not poison a managed parent either.
    EXPECT_EQ(flow.parallelizeTiledTasks(19, 1, [](std::size_t, std::size_t) -> int {
        throw std::runtime_error("external tile");
    }), au::flow::kErrorUnhandledException);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XFlow, SnapshotExcludesExternalSynchronousJob)
{
    auto& flow = au::flow::XFlow::get();
    std::atomic<bool> entered{false}, release{false}, drained{false};
    int tileCode = 1, waitCode = 1;
    std::thread caller([&] {
        tileCode = flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        });
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread waiter([&] { waitCode = flow.waitAll(); drained.store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!drained.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_TRUE(drained.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    caller.join();
    waiter.join();
    EXPECT_EQ(tileCode, 0);
    EXPECT_EQ(waitCode, 0);
}

TEST(XFlow, PureTilesRejectPublicationAndKeepDraftEditingAndNestedLoops)
{
    auto& flow = au::flow::XFlow::get();
    std::atomic<unsigned int> ran{0};
    const auto attempt = [&](bool nested) {
        auto draft = flow.makeTask([&]() -> int { ran.fetch_add(1, std::memory_order_relaxed); return 0; });
        auto after = flow.makeTask([]() -> int { return 0; });
        EXPECT_EQ(draft.precede(after), 0); // Building an unpublished graph remains legal.
        EXPECT_EQ(draft.state(), au::flow::TaskState::Draft);
        if (nested) {
            EXPECT_EQ(flow.parallelizeTiledTasks(3, 1, [&](std::size_t, std::size_t) {
                (void)flow.enqueue(draft); // Void callback deliberately ignores the rejection.
            }), au::err::kErrorStateInvalid);
        } else {
            const auto rejected = flow.enqueue(draft);
            EXPECT_EQ(rejected.submissionCode(), au::err::kErrorStateInvalid);
        }
        EXPECT_EQ(draft.state(), au::flow::TaskState::Draft);
    };
    for (bool nested : {false, true}) {
        EXPECT_EQ(flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) { attempt(nested); }),
                  au::err::kErrorStateInvalid);
        const auto parent = flow.enqueue([&]() -> int {
            (void)flow.parallelizeTiledTasks(53, 1, [&](std::size_t, std::size_t) { attempt(nested); });
            return 0; // Ignoring a loop error must not erase a structured task failure.
        });
        EXPECT_EQ(parent.wait(), au::err::kErrorStateInvalid);
    }
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0u);
    std::atomic<std::size_t> rows{0};
    EXPECT_EQ(flow.parallelizeTiledTasks(23, 3, [&](std::size_t, std::size_t count) {
        EXPECT_EQ(flow.parallelizeTiledTasks(count, 1, [&](std::size_t, std::size_t n) {
            rows.fetch_add(n, std::memory_order_relaxed);
        }), 0);
    }), 0);
    EXPECT_EQ(rows.load(std::memory_order_relaxed), 23u);
}

TEST(XFlow, PureTilesRejectExternalWorkAndPreserveErrorOrigins)
{
    auto& flow = au::flow::XFlow::get();
    au::flow::detail::ExternalOps ops{};
    ops.mRelease = [](void*) noexcept {};
    const auto rejected = [&] {
        return flow.parallelizeTiledTasks(31, 1, [&](std::size_t, std::size_t) {
            au::flow::detail::ExternalToken* token = nullptr;
            EXPECT_EQ(au::flow::detail::prepareExternal(ops, nullptr, &token), au::err::kErrorStateInvalid);
            EXPECT_EQ(token, nullptr);
        });
    };
    EXPECT_EQ(rejected(), au::err::kErrorStateInvalid);
    EXPECT_EQ(flow.enqueue(rejected).wait(), au::err::kErrorStateInvalid);
    for (bool throws : {false, true}) {
        auto task = flow.enqueue([&]() -> int {
            return flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) -> int {
                if (throws) throw std::runtime_error("tile origin");
                return au::flow::kErrorUnhandledException;
            });
        });
        au::flow::TaskResult result{};
        EXPECT_EQ(task.wait(&result), au::flow::kErrorUnhandledException);
        EXPECT_EQ(result.mDomain, throws ? au::flow::ErrorDomain::Flow : au::flow::ErrorDomain::Callable);
    }
    auto provenance = flow.enqueue([&]() -> int {
        return flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) -> int {
            au::flow::detail::ExternalStatus status{};
            status.mCode = 71; status.mNativeCode = -99;
            status.mDomain = au::flow::ErrorDomain::OpenCL; status.mCauseTaskId = 1234;
            au::flow::detail::recordExternalError(status);
            return 71;
        });
    });
    au::flow::TaskResult result{};
    EXPECT_EQ(provenance.wait(&result), 71);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(result.mNativeCode, -99);
    EXPECT_EQ(result.mCauseTaskId, 1234u);
}

TEST(XFlow, TileCallerAndHelperSharePublicationRestrictionsAndErrorOrdering)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(2), 0);
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> callerEntered{false}, helperEntered{false};
    EXPECT_EQ(flow.parallelizeTiledTasks(2, 1, [&](std::size_t, std::size_t) -> int {
        (std::this_thread::get_id() == caller ? callerEntered : helperEntered).store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!callerEntered.load(std::memory_order_acquire) || !helperEntered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) return 99;
            std::this_thread::yield();
        }
        (void)flow.enqueue([]() -> int { return 0; });
        EXPECT_EQ(flow.waitAll(), au::flow::kErrorWouldDeadlock);
        EXPECT_EQ(flow.shutdown(), au::flow::kErrorWouldDeadlock);
        return 0;
    }), au::err::kErrorStateInvalid);
    EXPECT_TRUE(callerEntered.load(std::memory_order_acquire));
    EXPECT_TRUE(helperEntered.load(std::memory_order_acquire));
    std::atomic<bool> higherOffsetEntered{false};
    EXPECT_EQ(flow.parallelizeTiledTasks(2, 1, [&](std::size_t offset, std::size_t) -> int {
        if (offset == 1) {
            higherOffsetEntered.store(true, std::memory_order_release);
            return 202;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!higherOffsetEntered.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) return 99;
            std::this_thread::yield();
        }
        return 101; // Lowest executed failing offset wins, regardless of finish order.
    }), 101);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XFlow, TiledHelpersCannotWaitForTheirStructuredAncestor)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(2), 0);
    std::atomic<unsigned int> entered{0};
    au::flow::Task parent;
    parent = flow.makeTask([&]() -> int {
        return flow.parallelizeTiledTasks(2, 1, [&](std::size_t, std::size_t) -> int {
            entered.fetch_add(1, std::memory_order_acq_rel);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (entered.load(std::memory_order_acquire) != 2) {
                if (std::chrono::steady_clock::now() > deadline) return 99;
                std::this_thread::yield();
            }
            return parent.wait(); // Both the parent caller and a live helper reject this cycle.
        });
    });
    EXPECT_EQ(flow.enqueue(parent).wait(), au::flow::kErrorWouldDeadlock);
    EXPECT_EQ(entered.load(std::memory_order_relaxed), 2u);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XFlow, TileContextSurvivesCooperativeWaitAndDeepSerialFallback)
{
    auto& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    au::flow::Task existing;
    std::atomic<bool> entered{false}, release{false}, childRan{false};
    auto parent = flow.enqueue([&]() -> int {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) -> int {
            return existing.wait(); // The unrelated task executes on this same cooperative stack.
        });
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    existing = flow.enqueue([&]() -> int {
        return flow.enqueue([&]() -> int { childRan.store(true, std::memory_order_relaxed); return 0; }).wait();
    });
    release.store(true, std::memory_order_release);
    EXPECT_EQ(parent.wait(), 0);
    EXPECT_TRUE(childRan.load(std::memory_order_relaxed));
    std::function<int(unsigned int)> descend;
    descend = [&](unsigned int depth) -> int {
        if (depth < 30) return flow.enqueue([&, depth]() -> int { return descend(depth + 1); }).wait();
        return flow.parallelizeTiledTasks(9, 1, [&](std::size_t, std::size_t) {
            (void)flow.enqueue([]() -> int { return 0; });
        }); // At depth 31 this uses the same guarded callback path, without helpers.
    };
    EXPECT_EQ(flow.enqueue([&]() -> int { return descend(0); }).wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(au::flow::testing::setBudget(budget), 0);
}

TEST(XFlow, OverflowQueueAndStableRecordReuse)
{
    au::flow::detail::ReadyQueue queue;
    std::array<au::flow::detail::ReadyLink, 2048> links;
    std::array<unsigned int, 2048> seen{};
    std::array<std::thread, 4> producers;
    for (std::size_t t = 0; t < producers.size(); ++t) producers[t] = std::thread([&, t] {
        for (std::size_t i = t; i < links.size(); i += producers.size()) {
            links[i].mValue = &seen[i]; queue.push(&links[i]);
        }
    });
    for (auto& producer : producers) producer.join();
    for (std::size_t i = 0; i < links.size(); ++i) {
        au::flow::detail::ReadyLink* item = nullptr;
        ASSERT_TRUE(queue.pop(item, i % 2 != 0)); ++*static_cast<unsigned int*>(item->mValue);
    }
    for (unsigned int count : seen) EXPECT_EQ(count, 1u);
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<unsigned int> sum{0};
    au::flow::Tasks wide;
    for (unsigned int i = 0; i < 2048; ++i) ASSERT_EQ(wide.add(flow.makeTask([&]() -> int {
        sum.fetch_add(1, std::memory_order_relaxed); return 0;
    })), 0);
    ASSERT_EQ(code(flow.enqueue(wide)), 0);
    EXPECT_EQ(sum.load(std::memory_order_relaxed), 2048u);
}

TEST(XFlow, TimeoutAndDraftQuery)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::Task draft = flow.makeTask([]() -> int { return 0; });
    au::flow::TaskResult result{};
    EXPECT_EQ(draft.result(result), au::err::kErrorNotReady);
    EXPECT_EQ(flow.waitFor(draft, 0), au::err::kErrorNotReady);
    std::atomic<bool> release{false};
    au::flow::Task work = flow.enqueue([&]() -> int {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        return 0;
    });
    EXPECT_EQ(flow.waitFor(work, 1), au::err::kErrorTimeout);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(code(work), 0);
    EXPECT_EQ(code(flow.enqueue(draft)), 0);
    au::flow::WorkerInfo workers[8] = {};
    std::size_t count = 0;
    EXPECT_EQ(flow.getWorkers(workers, 8, count), 0);
    EXPECT_GE(count, 1u); EXPECT_LE(count, 8u);
}

TEST(XFlow, AllocationRollbackAndParentSubmissionFailure)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 1);
    au::flow::Tasks missingEntry; // Roll back the group owner when its entry allocation fails.
    EXPECT_EQ(code(flow.enqueue(std::move(missingEntry))), au::err::kErrorNoMemory);
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
    au::flow::Task rejected = flow.enqueue([]() -> int { return 0; });
    EXPECT_EQ(rejected.submissionCode(), au::err::kErrorNoMemory);
    EXPECT_EQ(code(rejected), au::err::kErrorNoMemory);
    au::flow::Tasks incomplete;
    EXPECT_EQ(incomplete.add(rejected), au::err::kErrorNoMemory);
    EXPECT_EQ(flow.enqueue(incomplete).submissionCode(), au::err::kErrorNoMemory);
    au::flow::Task parent = flow.enqueue([&]() -> int {
        au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
        (void)flow.enqueue([]() -> int { return 0; });
        return 0;
    });
    EXPECT_EQ(code(parent), au::err::kErrorNoMemory);
    std::atomic<int> calls{0};
    au::flow::Task a = flow.makeTask([&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; });
    au::flow::Task b = flow.makeTask([&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; });
    au::flow::Tasks graph;
    ASSERT_EQ(graph.add(a), 0); ASSERT_EQ(graph.add(b), 0); ASSERT_EQ(a.precede(b), 0);
    au::flow::testing::failAfter(au::flow::testing::Failure::Edge, 0);
    EXPECT_EQ(flow.enqueue(graph).submissionCode(), au::err::kErrorNoMemory);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(a.state(), au::flow::TaskState::Rejected);
    EXPECT_EQ(b.state(), au::flow::TaskState::Rejected);
}

TEST(XFlow, GroupEntryFailureSkipsItsMembers)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::Task before = flow.enqueue([]() -> int { return 61; });
    int called = 0;
    au::flow::Tasks group;
    ASSERT_EQ(group.add(flow.makeTask([&]() -> int { ++called; return 0; })), 0);
    ASSERT_EQ(before.precede(group), 0);
    au::flow::Task all = flow.enqueue(group);
    EXPECT_EQ(code(all), 61);
    EXPECT_EQ(all.state(), au::flow::TaskState::Skipped);
    EXPECT_EQ(called, 0);
}

TEST(XFlow, SelfWaitAndStructuredCycleAreRejected)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::flow::Task self;
    self = flow.makeTask([&]() -> int { return flow.wait(self) == au::flow::kErrorWouldDeadlock ? 0 : 1; });
    EXPECT_EQ(code(flow.enqueue(self)), 0);
    au::flow::Task parent;
    parent = flow.makeTask([&]() -> int {
        au::flow::Task child = flow.makeTask([]() -> int { return 0; });
        if (parent.precede(child)) return 1;
        (void)flow.enqueue(child); // The failed submission is latched even if the body returns success.
        return 0;
    });
    EXPECT_EQ(code(flow.enqueue(parent)), au::flow::kErrorCycleDetected);
}

TEST(XFlow, IndirectWaitThroughAStructuredChildDoesNotDeadlock)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<bool> start{false};
    std::atomic<int> detected{0};
    au::flow::Task parent, other;
    parent = flow.makeTask([&]() -> int {
        (void)flow.enqueue([&]() -> int {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            const int wait = flow.wait(other);
            if (wait == au::flow::kErrorWouldDeadlock) detected.fetch_add(1, std::memory_order_relaxed);
            return wait && wait != au::flow::kErrorWouldDeadlock ? wait : 0;
        });
        return 0;
    });
    ASSERT_EQ(flow.enqueue(parent).submissionCode(), 0);
    other = flow.enqueue([&]() -> int {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        const int wait = flow.wait(parent);
        if (wait == au::flow::kErrorWouldDeadlock) detected.fetch_add(1, std::memory_order_relaxed);
        return wait && wait != au::flow::kErrorWouldDeadlock ? wait : 0;
    });
    start.store(true, std::memory_order_release);
    EXPECT_EQ(code(parent), 0); EXPECT_EQ(code(other), 0);
    EXPECT_GE(detected.load(std::memory_order_relaxed), 1);
}

TEST(XFlow, SnapshotIncludesFutureDescendantsAndExcludesLaterRoots)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    struct Context {
        std::atomic<bool> mSnapshot{false}, mContinue{false}, mReleaseParent{false};
        std::atomic<bool> mChildStarted{false}, mReleaseChild{false}, mReleaseLater{false}, mDone{false};
    } context;
    au::flow::Task before = flow.enqueue([&]() -> int {
        while (!context.mReleaseParent.load(std::memory_order_acquire)) std::this_thread::yield();
        (void)flow.enqueue([&]() -> int {
            context.mChildStarted.store(true, std::memory_order_release);
            while (!context.mReleaseChild.load(std::memory_order_acquire)) std::this_thread::yield();
            return 0;
        });
        return 0;
    });
    au::flow::testing::SnapshotHook hook{[](void* payload) {
        Context& c = *static_cast<Context*>(payload);
        c.mSnapshot.store(true, std::memory_order_release);
        while (!c.mContinue.load(std::memory_order_acquire)) std::this_thread::yield();
    }, &context};
    au::flow::testing::snapshotHook(&hook);
    std::thread waiter([&] { EXPECT_EQ(flow.waitAll(), 0); context.mDone.store(true, std::memory_order_release); });
    while (!context.mSnapshot.load(std::memory_order_acquire)) std::this_thread::yield();
    au::flow::Task later = flow.makeTask([&]() -> int {
        while (!context.mReleaseLater.load(std::memory_order_acquire)) std::this_thread::yield();
        return 0;
    });
    EXPECT_EQ(before.precede(later), 0); EXPECT_EQ(flow.enqueue(later).submissionCode(), 0);
    context.mContinue.store(true, std::memory_order_release);
    context.mReleaseParent.store(true, std::memory_order_release);
    while (!context.mChildStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_FALSE(context.mDone.load(std::memory_order_acquire));
    context.mReleaseChild.store(true, std::memory_order_release);
    waiter.join();
    EXPECT_TRUE(context.mDone.load(std::memory_order_acquire));
    EXPECT_NE(later.state(), au::flow::TaskState::Succeeded);
    context.mReleaseLater.store(true, std::memory_order_release);
    au::flow::testing::snapshotHook(nullptr);
    EXPECT_EQ(code(later), 0);
}

TEST(XFlow, SingleWorkerAndAllParticipantsMakeProgress)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    EXPECT_EQ(flow.waitAll(), 0);
    const std::size_t budget = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    EXPECT_EQ(code(flow.enqueue([&]() -> int {
        au::flow::Task child = flow.enqueue([]() -> int { return 0; });
        if (code(child)) return 1;
        return flow.parallelizeTiledTasks(129, 7, [](std::size_t, std::size_t) {});
    })), 0);
    EXPECT_EQ(flow.waitAll(), 0);
    ASSERT_EQ(au::flow::testing::setBudget(budget), 0);
    std::atomic<std::size_t> arrived{0}, covered{0};
    std::vector<au::flow::Task> parents;
    parents.reserve(budget);
    for (std::size_t i = 0; i < budget; ++i) parents.emplace_back(flow.enqueue([&]() -> int {
        arrived.fetch_add(1, std::memory_order_acq_rel);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (arrived.load(std::memory_order_acquire) != budget) {
            if (std::chrono::steady_clock::now() > until) return 1;
            std::this_thread::yield();
        }
        return flow.parallelizeTiledTasks(111, 3, [&](std::size_t, std::size_t n) {
            covered.fetch_add(n, std::memory_order_relaxed);
        });
    }));
    for (const auto& parent : parents) EXPECT_EQ(code(parent), 0);
    EXPECT_EQ(covered.load(std::memory_order_relaxed), budget * 111);
}

TEST(XFlow, DotAndTraceSnapshots)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    ASSERT_EQ(flow.setTracing(true), 0);
    au::flow::Task work = flow.makeTask([]() -> int { return 0; });
    ASSERT_EQ(work.setName("quoted\\\"name"), 0);
    EXPECT_EQ(code(flow.enqueue(work)), 0);
    EXPECT_EQ(flow.setTracing(false), 0);
    ASSERT_EQ(flow.dumpDot(work, "flow-test.dot"), 0);
    ASSERT_EQ(flow.dumpTrace("flow-test.json"), 0);
    std::string dot, trace;
    ASSERT_EQ(au::file::read("flow-test.dot", dot), 0);
    ASSERT_EQ(au::file::read("flow-test.json", trace), 0);
    EXPECT_NE(dot.find("digraph AuraFlow"), std::string::npos);
    EXPECT_NE(trace.find("traceEvents"), std::string::npos);
    EXPECT_NE(trace.find("callable wall"), std::string::npos);
    au::json::XJson parsed = au::json::XJson::parse(trace);
    ASSERT_TRUE(parsed.isValid());
    EXPECT_TRUE(parsed["traceEvents"].isArray());
    EXPECT_GT(parsed["traceEvents"].getArraySize(), 0u);
    EXPECT_EQ(au::file::removeFile("flow-test.dot"), 0);
    EXPECT_EQ(au::file::removeFile("flow-test.json"), 0);
}

TEST(XFlow, CapturedStructuredChildrenAndNestedClustersRemainExportable)
{
    auto& flow = au::flow::XFlow::get(); ASSERT_EQ(flow.setTracing(true), 0);
    au::flow::Tasks outer, inner;
    ASSERT_EQ(outer.setName("outer"), 0); ASSERT_EQ(inner.setName("inner"), 0);
    auto parent = flow.makeTask([&]() -> int {
        auto child = flow.makeTask([]() -> int { return 0; });
        std::string name = "dynamic \"child\"\nline";
        for (unsigned int i = 0; i < 40; ++i) name += u8"界";
        const int named = child.setName(name.c_str()); if (named) return named;
        const auto submitted = flow.enqueue(child); if (submitted.submissionCode()) return submitted.submissionCode();
        return flow.wait(submitted);
    });
    ASSERT_EQ(parent.setName("parent \"label\""), 0); ASSERT_EQ(inner.add(parent), 0); ASSERT_EQ(outer.add(std::move(inner)), 0);
    auto group = flow.enqueue(outer); EXPECT_EQ(code(group), 0);
    EXPECT_EQ(flow.setTracing(false), 0);
    ASSERT_EQ(flow.dumpDot(outer, "flow-nested.dot"), 0); ASSERT_EQ(flow.dumpTrace("flow-nested.json"), 0);
    std::string dot, trace; ASSERT_EQ(au::file::read("flow-nested.dot", dot), 0); ASSERT_EQ(au::file::read("flow-nested.json", trace), 0);
    EXPECT_NE(dot.find("subgraph cluster_" + std::to_string(group.id())), std::string::npos);
    EXPECT_NE(dot.find("label=\"inner\""), std::string::npos); EXPECT_NE(dot.find("structured child"), std::string::npos);
    EXPECT_NE(dot.find("dynamic \\\"child\\\"\\nline"), std::string::npos);
    const auto json = au::json::XJson::parse(trace); ASSERT_TRUE(json.isValid());
    const auto events = json["traceEvents"];
    int begins = 0, ends = 0; bool sawName = false, sawTruncation = false;
    for (std::size_t i = 0; i < events.getArraySize(); ++i) {
        const auto event = events[i];
        const auto label = event["args"]["taskName"].getString();
        if (label.find("dynamic \"child\"\nline") == 0) {
            EXPECT_TRUE(event["args"]["taskNameTruncated"].getBool());
            EXPECT_GE(label.size(), 3u); EXPECT_EQ(label.substr(label.size() - 3), u8"界"); sawTruncation = true;
        }
        if (event["args"]["task"].getString() != std::to_string(parent.id())) continue;
        if (event["args"]["taskName"].getString() == "parent \"label\"") sawName = true;
        if (event["name"].getString() == "CPU slice") {
            if (event["ph"].getString() == "B") ++begins;
            if (event["ph"].getString() == "E") ++ends;
        }
    }
    EXPECT_TRUE(sawName); EXPECT_TRUE(sawTruncation); EXPECT_GE(begins, 2); EXPECT_EQ(begins, ends); // Explicit wait splits the parent slice even if child already finished.
    EXPECT_EQ(au::file::removeFile("flow-nested.dot"), 0); EXPECT_EQ(au::file::removeFile("flow-nested.json"), 0);
}

namespace {
struct FakeExternal {
    std::atomic<bool> mDone{false};
    std::atomic<int>* mReleased;
    au::flow::detail::ExternalStatus mStatus;
    au::mm::detail::MemoryLease mLease;
    static int poll(void* payload, bool* done, au::flow::detail::ExternalStatus* status) noexcept
    {
        FakeExternal& value = *static_cast<FakeExternal*>(payload);
        *done = value.mDone.load(std::memory_order_acquire);
        if (*done) *status = value.mStatus;
        return 0;
    }
    static au::flow::detail::ExternalStatus finalize(void*) noexcept { return {}; }
    static void release(void* payload) noexcept
    {
        std::unique_ptr<FakeExternal> owner(static_cast<FakeExternal*>(payload));
        owner->mReleased->fetch_add(1, std::memory_order_relaxed);
    }
};
au::flow::detail::ExternalOps fakeOps()
{ return {&FakeExternal::poll, &FakeExternal::finalize, &FakeExternal::release}; }
}

TEST(XFlow, ImmediateExternalCompletionAndRegistrarPin)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<int> released{0};
    au::flow::Task task = flow.enqueue([&]() -> int {
        std::unique_ptr<FakeExternal> payload(new FakeExternal());
        payload->mReleased = &released;
        au::flow::detail::ExternalToken* token = nullptr;
        const int prepared = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
        if (prepared || !token) return prepared ? prepared : 1;
        (void)payload.release();
        au::flow::detail::notifyExternal(token, {});
        au::flow::detail::notifyExternal(token, {}); // Duplicate notification while the registrar owns the token.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const bool earlyRelease = released.load(std::memory_order_relaxed) != 0;
        au::flow::detail::leaveExternalRegistrar(token);
        return earlyRelease ? 2 : 0;
    });
    EXPECT_EQ(code(task), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 1);
}

TEST(XFlow, PollingExternalCompletionAndNativeErrors)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<int> released{0};
    std::atomic<FakeExternal*> pending{nullptr};
    au::flow::Task fallback = flow.enqueue([&]() -> int {
        std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
        payload->mStatus = {au::err::kErrorPlatformAPI, -7, au::flow::ErrorDomain::OpenCL};
        au::flow::detail::ExternalToken* token = nullptr;
        const int prepared = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
        if (prepared) return prepared;
        pending.store(payload.release(), std::memory_order_release);
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        return 0;
    });
    FakeExternal* event = nullptr;
    while (!(event = pending.load(std::memory_order_acquire))) std::this_thread::yield();
    EXPECT_EQ(flow.waitFor(fallback, 2), au::err::kErrorTimeout);
    event->mDone.store(true, std::memory_order_release);
    au::flow::TaskResult result{};
    EXPECT_EQ(flow.waitFor(fallback, 10000, &result), au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(result.mNativeCode, -7);
    EXPECT_EQ(result.mCauseTaskId, fallback.id());
    EXPECT_EQ(released.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(au::flow::detail::currentTaskId(), 0u);
    au::flow::Task immediate = flow.enqueue([]() -> int {
        EXPECT_NE(au::flow::detail::currentTaskId(), 0u);
        au::flow::detail::recordExternalError({au::err::kErrorPlatformAPI, -5, au::flow::ErrorDomain::OpenCL});
        return au::err::kErrorPlatformAPI;
    });
    EXPECT_EQ(flow.waitFor(immediate, 10000, &result), au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mNativeCode, -5);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL); EXPECT_EQ(result.mCauseTaskId, immediate.id());
}

TEST(XFlow, BodyFailureStillWaitsForExternalMemoryLease)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    au::mm::MemBlock block;
    const int allocation = au::mm::alloc(256, au::mm::MemType::Pss, block);
    if (allocation == au::err::kErrorNotSupported) GTEST_SKIP() << "Native Pss allocation is unavailable on this platform";
    ASSERT_EQ(allocation, 0);
    au::mm::detail::MemoryLease observation;
    ASSERT_EQ(au::mm::Registry::instance().acquire(block.ptr, block.size, observation), 0);
    std::shared_ptr<au::mm::detail::ReleaseStatus> report = observation.releaseStatus();
    observation.reset(); // The report observes cleanup without owning the storage.
    std::atomic<int> released{0};
    std::atomic<FakeExternal*> pending{nullptr};
    au::flow::Task work = flow.enqueue([&]() -> int {
        std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
        const int lease = au::mm::Registry::instance().acquire(block.ptr, block.size, payload->mLease);
        if (lease) return lease;
        au::flow::detail::ExternalToken* token = nullptr;
        const int prepare = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
        if (prepare) return prepare;
        pending.store(payload.release(), std::memory_order_release);
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        return 71; // A later native submit failed after some work was already launched.
    });
    FakeExternal* event = nullptr;
    while (!(event = pending.load(std::memory_order_acquire))) std::this_thread::yield();
    EXPECT_EQ(au::mm::free(block.ptr), 0);
    EXPECT_FALSE(report->mDone.load(std::memory_order_acquire));
    EXPECT_EQ(flow.waitFor(work, 2), au::err::kErrorTimeout);
    std::memset(event->mLease.block().ptr, 0x7c, event->mLease.block().size);
    event->mDone.store(true, std::memory_order_release);
    EXPECT_EQ(code(work), 71);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(report->mDone.load(std::memory_order_acquire));
}

TEST(XFlow, ExternalRecordsGrowAndDrainWithoutAThirdThreadPool)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::array<FakeExternal*, 192> events{};
    std::atomic<int> released{0};
    std::atomic<bool> prepared{false};
    au::flow::Task work = flow.enqueue([&]() -> int {
        for (auto& event : events) {
            std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
            au::flow::detail::ExternalToken* token = nullptr;
            const int error = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
            if (error) return error;
            event = payload.release();
            au::flow::detail::pollExternal(token);
            au::flow::detail::leaveExternalRegistrar(token);
        }
        prepared.store(true, std::memory_order_release);
        return 0;
    });
    while (!prepared.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_EQ(flow.waitFor(work, 2), au::err::kErrorTimeout);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 0);
    for (auto* event : events) event->mDone.store(true, std::memory_order_release);
    EXPECT_EQ(code(work), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), static_cast<int>(events.size()));
}

TEST(XFlow, AffinityAppliesBeforeTheNextCallableAndCanBeQueried)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::uint64_t thread = 0;
    ASSERT_EQ(code(flow.enqueue([&]() -> int { thread = au::sys::flow::threadId(); return 0; })), 0);
    const auto slotFor = [&](std::uint64_t native) -> std::uint32_t {
        std::array<au::flow::WorkerInfo, 8> info{};
        std::size_t slots = 0;
        if (flow.getWorkers(info.data(), info.size(), slots)) return 8;
        for (std::size_t i = 0; i < slots; ++i) if (info[i].mThreadId == native) return info[i].mWorkerId;
        return 8;
    };
    const auto firstSlot = slotFor(thread);
    ASSERT_LT(firstSlot, 8u);
    std::size_t count = 0;
    const int query = flow.getWorkerAffinity(firstSlot, nullptr, 0, count);
    if (query == au::err::kErrorNotSupported) GTEST_SKIP() << "Explicit affinity is unsupported";
    ASSERT_EQ(query, au::err::kErrorBufferUnderflow);
    ASSERT_GT(count, 0u);
    std::vector<std::uint32_t> cpus(count);
    ASSERT_EQ(flow.getWorkerAffinity(firstSlot, cpus.data(), cpus.size(), count), 0);
    const std::uint32_t selected = cpus[0];
    const au::flow::AffinityPolicy policy{au::flow::AffinityMode::ExplicitCpuSet, &selected, 1};
    ASSERT_EQ(flow.setAffinity(policy), 0);
    EXPECT_EQ(code(flow.enqueue([selected, &thread]() -> int {
        thread = au::sys::flow::threadId();
        std::vector<std::uint32_t> actual;
        const int ret = au::sys::flow::currentAffinity(actual);
        return ret ? ret : actual.size() == 1 && actual[0] == selected ? 0 : 1;
    })), 0);
    std::uint32_t actual = 0;
    EXPECT_EQ(flow.getWorkerAffinity(slotFor(thread), &actual, 1, count), 0);
    EXPECT_EQ(count, 1u); EXPECT_EQ(actual, selected);
    const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
    EXPECT_EQ(flow.setAffinity(restore), 0);
}

TEST(XFlow, CleanupCannotWaitOnItselfOrCreateNewWork)
{
    struct Payload { au::flow::Task mSelf; int* mWait; int* mSubmit; };
    int waited = 0, submitted = 0;
    auto destroy = [](Payload* payload) {
        *payload->mWait = au::flow::XFlow::get().wait(payload->mSelf);
        *payload->mSubmit = au::flow::XFlow::get().enqueue([]() -> int { return 0; }).submissionCode();
        delete payload;
    };
    std::unique_ptr<Payload, decltype(destroy)> capture(new Payload{{}, &waited, &submitted}, destroy);
    Payload* pointer = capture.get();
    au::flow::Task task = au::flow::XFlow::get().makeTask([value = std::move(capture)]() -> int { return 0; });
    pointer->mSelf = task;
    ASSERT_EQ(au::flow::XFlow::get().enqueue(task).submissionCode(), 0);
    EXPECT_EQ(code(task), au::flow::kErrorWouldDeadlock); // Ignored child acceptance failure still reported.
    EXPECT_EQ(waited, au::flow::kErrorWouldDeadlock);
    EXPECT_EQ(submitted, au::flow::kErrorWouldDeadlock);
}

TEST(XFlow, ElasticCallableStorageAlignmentAndConstructionRollback)
{
    struct alignas(128) LargeCallable {
        std::array<unsigned char, 16384> mData{};
        std::unique_ptr<int> mOwner;
        std::atomic<int>* mDestroyed;
        explicit LargeCallable(std::atomic<int>& count) : mOwner(new int(27)), mDestroyed(&count) { mData.back() = 91; }
        LargeCallable(LargeCallable&&) noexcept = default;
        ~LargeCallable() { if (mOwner) mDestroyed->fetch_add(1, std::memory_order_relaxed); }
        int operator()() &&
        {
            return reinterpret_cast<std::uintptr_t>(this) % 128 == 0 && mData.back() == 91 && *mOwner == 27
                ? 0 : au::err::kErrorAlignmentFault;
        }
    };
    std::atomic<int> destroyed{0};
    auto& flow = au::flow::XFlow::get();
    au::flow::Task task = flow.makeTask(LargeCallable(destroyed));
    au::flow::Task copy = task;
    au::flow::Task* same = &copy;
    copy = *same; copy = std::move(*same);
    EXPECT_EQ(copy.id(), task.id());
    au::flow::Task moved = std::move(copy);
    EXPECT_FALSE(copy.valid());
    copy = task; moved = std::move(copy); copy = au::flow::Task();
    EXPECT_EQ(moved.id(), task.id()); EXPECT_FALSE(copy.valid());
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(code(flow.enqueue(moved)), 0);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
    struct ThrowOnCopy {
        int mFailure;
        explicit ThrowOnCopy(int failure) : mFailure(failure) {}
        ThrowOnCopy(const ThrowOnCopy& other) : mFailure(other.mFailure)
        { if (mFailure == 1) throw std::bad_alloc(); if (mFailure == 2) throw std::runtime_error("copy"); }
        int operator()() && { return 0; }
    };
    ThrowOnCopy function(1);
    EXPECT_EQ(au::flow::XFlow::get().makeTask(function).submissionCode(), au::err::kErrorNoMemory);
    function.mFailure = 2;
    EXPECT_EQ(au::flow::XFlow::get().makeTask(function).submissionCode(), au::flow::kErrorUnhandledException);
    au::flow::testing::failAfter(au::flow::testing::Failure::Payload, 0);
    EXPECT_EQ(au::flow::XFlow::get().makeTask([]() -> int { return 0; }).submissionCode(), au::err::kErrorNoMemory);
    function.mFailure = 0;
    EXPECT_EQ(code(au::flow::XFlow::get().enqueue(function)), 0);
}

TEST(XFlow, StructuredReservationAndOrderOverflowDoNotPartiallyAccept)
{
    auto& flow = au::flow::XFlow::get();
    au::flow::testing::setOutstandingLimit(1);
    const auto parent = flow.enqueue([&]() -> int {
        const auto child = flow.enqueue([]() -> int { return 71; });
        if (child.submissionCode() != au::err::kErrorOverflow || child.state() != au::flow::TaskState::Rejected) return 1;
        au::flow::detail::ExternalToken* token = nullptr;
        const au::flow::detail::ExternalOps ops{nullptr, nullptr, [](void*) noexcept {}};
        const int external = au::flow::detail::prepareExternal(ops, nullptr, &token);
        return external == au::err::kErrorOverflow && !token ? 0 : 2;
    });
    const int status = code(parent);
    au::flow::testing::setOutstandingLimit(SIZE_MAX);
    EXPECT_EQ(status, au::err::kErrorOverflow); // Ignored failed child acceptance is latched on its parent.
    au::flow::testing::failAfter(au::flow::testing::Failure::Order, 0);
    au::flow::Tasks group; std::atomic<unsigned int> calls{0};
    ASSERT_EQ(group.add(flow.makeTask([&]() -> int { calls.fetch_add(1, std::memory_order_relaxed); return 0; })), 0);
    const auto rejected = flow.enqueue(group);
    EXPECT_EQ(rejected.submissionCode(), au::err::kErrorOverflow); EXPECT_EQ(calls.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(code(flow.enqueue([]() -> int { return 0; })), 0);
}

TEST(XFlow, CrossWaitAndDeepGraphUseBoundedStacks)
{
    auto& flow = au::flow::XFlow::get();
    std::array<au::flow::Task, 2> peers;
    std::atomic<unsigned int> entered{0}, cycles{0};
    for (std::size_t i = 0; i < peers.size(); ++i) peers[i] = flow.makeTask([&, i]() -> int {
        entered.fetch_add(1, std::memory_order_acq_rel);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (entered.load(std::memory_order_acquire) < peers.size()) {
            if (std::chrono::steady_clock::now() >= deadline) return au::err::kErrorTimeout;
            std::this_thread::yield();
        }
        const int wait = flow.waitFor(peers[1 - i], 2000);
        if (wait == au::flow::kErrorWouldDeadlock) cycles.fetch_add(1, std::memory_order_relaxed);
        return wait && wait != au::flow::kErrorWouldDeadlock ? wait : 0;
    });
    for (auto& peer : peers) ASSERT_EQ(flow.enqueue(peer).submissionCode(), 0);
    for (auto& peer : peers) EXPECT_EQ(code(peer), 0);
    EXPECT_GE(cycles.load(std::memory_order_relaxed), 1u);
    // Link in reverse construction order so draft validation remains linear
    // per insertion while acceptance/completion/reclamation cover a deep chain.
    au::flow::Tasks graph; std::vector<au::flow::Task> chain; chain.reserve(4096);
    for (unsigned int i = 0; i < 4096; ++i) chain.push_back(flow.makeTask([]() -> int { return 0; }));
    for (std::size_t i = chain.size() - 1; i; --i) ASSERT_EQ(chain[i - 1].precede(chain[i]), 0);
    for (const auto& node : chain) ASSERT_EQ(graph.add(node), 0);
    EXPECT_EQ(code(flow.enqueue(graph)), 0);
}

TEST(XFlow, CompletionSearchVisitsOneGraphAcrossCooperativeFrames)
{
    auto& flow = au::flow::XFlow::get();
    const auto previous = au::flow::testing::budget();
    ASSERT_EQ(au::flow::testing::setBudget(1), 0);
    struct Drain {
        std::atomic<bool> mDone{false};
        std::size_t mBudget;
        ~Drain() {
            mDone.store(true, std::memory_order_release);
            (void)au::flow::XFlow::get().waitAll();
            (void)au::flow::testing::setBudget(mBudget);
        }
    } drain{{false}, previous};
    std::atomic<unsigned int> entered{0};
    const au::flow::detail::ExternalOps ops{
        [](void* payload, bool* done, au::flow::detail::ExternalStatus*) noexcept -> int {
            *done = static_cast<Drain*>(payload)->mDone.load(std::memory_order_acquire);
            return 0;
        }, nullptr, [](void*) noexcept {}};
    std::array<au::flow::Task, 64> pending;
    for (auto& task : pending) task = flow.enqueue([&]() -> int {
        au::flow::detail::ExternalToken* token = nullptr;
        const int result = au::flow::detail::prepareExternal(ops, &drain, &token);
        if (result) return result;
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        entered.fetch_add(1, std::memory_order_release);
        return 0;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (entered.load(std::memory_order_acquire) != pending.size() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    ASSERT_EQ(entered.load(std::memory_order_acquire), pending.size());
    au::flow::testing::GraphStats observed;
    std::function<int(unsigned int)> nested;
    nested = [&](unsigned int depth) -> int {
        if (depth) return flow.enqueue([&, depth]() -> int { return nested(depth - 1); }).wait();
        (void)au::flow::testing::graphStats(true);
        const int result = flow.waitFor(pending[0], 0);
        observed = au::flow::testing::graphStats();
        return result == au::err::kErrorTimeout ? 0 : 1;
    };
    EXPECT_EQ(flow.enqueue([&]() -> int { return nested(6); }).wait(), 0);
    EXPECT_EQ(observed.mSearches, 1u);
    EXPECT_EQ(observed.mNodes, 1u);
    EXPECT_EQ(observed.mActiveScans, 0u); // Independent active roots do not participate.
}

TEST(XFlow, DraftTraversalScratchIsRestoredAfterEarlyCycleExitAndSlotReuse)
{
    auto& flow = au::flow::XFlow::get();
    for (unsigned int round = 0; round < 128; ++round) {
        std::array<au::flow::Task, 8> chain;
        for (auto& node : chain) node = flow.makeTask([]() -> int { return 0; });
        for (std::size_t i = 1; i < chain.size(); ++i) ASSERT_EQ(chain[i - 1].precede(chain[i]), 0);
        EXPECT_EQ(chain.back().precede(chain.front()), au::flow::kErrorCycleDetected);
        auto tail = flow.makeTask([]() -> int { return 0; });
        EXPECT_EQ(chain.back().precede(tail), 0);
        EXPECT_EQ(tail.precede(chain[6]), au::flow::kErrorCycleDetected);
        EXPECT_EQ(code(flow.enqueue([]() -> int { return 0; })), 0);
        // Nothing submits the rejected Draft chain. ASan/LSan also verifies
        // that a failed early search did not install an owning reference cycle.
    }
}

TEST(XFlow, InheritedResultPriorityIsIndependentOfArrivalOrder)
{
    const auto make = [](std::uint64_t cause, int code, au::flow::ErrorDomain domain, bool skipped) {
        auto result = au::flow::detail::taskResult(cause, code, domain);
        result.mNativeCode = -static_cast<int>(cause);
        if (skipped) result.mState = static_cast<std::uint32_t>(au::flow::TaskState::Skipped);
        return result;
    };
    const std::array<au_flow_result, 4> sources{{
        make(10, 77, au::flow::ErrorDomain::Flow, true),
        make(11, 88, au::flow::ErrorDomain::OpenCL, false),
        make(12, 99, au::flow::ErrorDomain::Memory, false),
        make(13, 66, au::flow::ErrorDomain::Flow, true)}};
    std::array<std::size_t, 4> order{{0, 1, 2, 3}};
    do {
        au::flow::detail::TaskErrors errors;
        for (const auto i : order) errors.inherit(sources[i], i);
        errors.body(0);
        const auto result = errors.complete(100);
        EXPECT_EQ(result.mCode, 88);
        EXPECT_EQ(result.mNativeCode, -11);
        EXPECT_EQ(result.mCauseTaskId, 11u);
        EXPECT_EQ(result.mDomain, static_cast<std::uint32_t>(au::flow::ErrorDomain::OpenCL));
        EXPECT_EQ(result.mState, static_cast<std::uint32_t>(au::flow::TaskState::Failed));
    } while (std::next_permutation(order.begin(), order.end()));
}

#endif
