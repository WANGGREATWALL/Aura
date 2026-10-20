#if ENABLE_TEST_XFLOW
#include "flow/xflow.h"
#include "flow/ready_queue.h"
#include "flow/testing.h"
#include "flow/completion.h"
#include "file/xfile.h"
#include "mm/registry.h"
#include "json/xjson.h"
#include "sys/flow_runtime.h"
#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
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
    EXPECT_EQ(flow.wait(failure, &result), 0);
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
    ASSERT_EQ(flow.wait(throwing, &result), 0);
    EXPECT_EQ(result.mCode, au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Flow);
    au::flow::Task business = flow.enqueue([]() -> int { return au::flow::kErrorUnhandledException; });
    ASSERT_EQ(flow.wait(business, &result), 0);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);
    au::flow::Task tile = flow.enqueue([&]() -> int {
        return flow.parallelizeTiledTasks(16, 1, [](std::size_t, std::size_t) -> int {
            throw std::runtime_error("tile");
        });
    });
    ASSERT_EQ(flow.wait(tile, &result), 0);
    EXPECT_EQ(result.mCode, au::flow::kErrorUnhandledException);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Flow);
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
    EXPECT_NE(trace.find("CPU"), std::string::npos);
    au::json::XJson parsed = au::json::XJson::parse(trace);
    ASSERT_TRUE(parsed.isValid());
    EXPECT_TRUE(parsed["traceEvents"].isArray());
    EXPECT_GT(parsed["traceEvents"].getArraySize(), 0u);
    EXPECT_EQ(au::file::removeFile("flow-test.dot"), 0);
    EXPECT_EQ(au::file::removeFile("flow-test.json"), 0);
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
        au::flow::detail::armExternalCallback(token);
        au::flow::detail::notifyExternal(token, {});
        au::flow::detail::notifyExternal(token, {}); // Duplicate notification with a live callback pin.
        au::flow::detail::leaveExternalCallback(token);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const bool earlyRelease = released.load(std::memory_order_relaxed) != 0;
        au::flow::detail::leaveExternalRegistrar(token);
        return earlyRelease ? 2 : 0;
    });
    EXPECT_EQ(code(task), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 1);
}

TEST(XFlow, ExternalCallbackExitAndFallbackErrors)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    std::atomic<int> released{0};
    std::atomic<bool> callbackEntered{false}, callbackInstalled{false}, releaseCallback{false};
    std::thread callback;
    au::flow::Task task = flow.enqueue([&]() -> int {
        std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
        au::flow::detail::ExternalToken* token = nullptr;
        const int prepared = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
        if (prepared) return prepared;
        (void)payload.release();
        au::flow::detail::armExternalCallback(token);
        callback = std::thread([&, token] {
            au::flow::detail::notifyExternal(token, {});
            callbackEntered.store(true, std::memory_order_release);
            while (!releaseCallback.load(std::memory_order_acquire)) std::this_thread::yield();
            au::flow::detail::leaveExternalCallback(token);
        });
        callbackInstalled.store(true, std::memory_order_release);
        au::flow::detail::leaveExternalRegistrar(token);
        return 0;
    });
    while (!callbackEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    while (!callbackInstalled.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_EQ(flow.waitFor(task, 2), au::err::kErrorTimeout);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 0);
    releaseCallback.store(true, std::memory_order_release);
    callback.join();
    EXPECT_EQ(code(task), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 1);

    std::atomic<FakeExternal*> pending{nullptr};
    au::flow::Task fallback = flow.enqueue([&]() -> int {
        std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
        payload->mStatus = {au::err::kErrorPlatformAPI, -7, au::flow::ErrorDomain::OpenCL};
        au::flow::detail::ExternalToken* token = nullptr;
        const int prepared = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
        if (prepared) return prepared;
        pending.store(payload.release(), std::memory_order_release);
        au::flow::detail::armExternalCallback(token);
        au::flow::detail::leaveExternalCallback(token); // Registration failed: native API will not call back.
        au::flow::detail::pollExternal(token);
        au::flow::detail::leaveExternalRegistrar(token);
        return 0;
    });
    FakeExternal* event = nullptr;
    while (!(event = pending.load(std::memory_order_acquire))) std::this_thread::yield();
    EXPECT_EQ(flow.waitFor(fallback, 2), au::err::kErrorTimeout);
    event->mDone.store(true, std::memory_order_release);
    au::flow::TaskResult result{};
    EXPECT_EQ(flow.waitFor(fallback, 10000, &result), 0);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(result.mNativeCode, -7);
    EXPECT_EQ(result.mCauseTaskId, fallback.id());
    EXPECT_EQ(released.load(std::memory_order_relaxed), 2);
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
    std::array<au::flow::detail::ExternalToken*, 192> tokens{};
    std::atomic<int> released{0};
    std::atomic<bool> prepared{false};
    au::flow::Task work = flow.enqueue([&]() -> int {
        for (auto& token : tokens) {
            std::unique_ptr<FakeExternal> payload(new FakeExternal()); payload->mReleased = &released;
            const int error = au::flow::detail::prepareExternal(fakeOps(), payload.get(), &token);
            if (error) return error;
            (void)payload.release();
            au::flow::detail::armExternalCallback(token);
            au::flow::detail::notifyExternal(token, {});
            au::flow::detail::leaveExternalRegistrar(token);
        }
        prepared.store(true, std::memory_order_release);
        return 0;
    });
    while (!prepared.load(std::memory_order_acquire)) std::this_thread::yield();
    EXPECT_EQ(flow.waitFor(work, 2), au::err::kErrorTimeout);
    EXPECT_EQ(released.load(std::memory_order_relaxed), 0);
    for (auto* token : tokens) au::flow::detail::leaveExternalCallback(token);
    EXPECT_EQ(code(work), 0);
    EXPECT_EQ(released.load(std::memory_order_relaxed), static_cast<int>(tokens.size()));
}

TEST(XFlow, AffinityAppliesBeforeTheNextCallableAndCanBeQueried)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    ASSERT_EQ(code(flow.enqueue([]() -> int { return 0; })), 0);
    std::size_t count = 0;
    const int query = flow.getWorkerAffinity(0, nullptr, 0, count);
    if (query == au::err::kErrorNotSupported) GTEST_SKIP() << "Explicit affinity is unsupported";
    ASSERT_EQ(query, au::err::kErrorBufferUnderflow);
    ASSERT_GT(count, 0u);
    std::vector<std::uint32_t> cpus(count);
    ASSERT_EQ(flow.getWorkerAffinity(0, cpus.data(), cpus.size(), count), 0);
    const std::uint32_t selected = cpus[0];
    const au::flow::AffinityPolicy policy{au::flow::AffinityMode::ExplicitCpuSet, &selected, 1};
    ASSERT_EQ(flow.setAffinity(policy), 0);
    EXPECT_EQ(code(flow.enqueue([selected]() -> int {
        std::vector<std::uint32_t> actual;
        const int ret = au::sys::flow::currentAffinity(actual);
        return ret ? ret : actual.size() == 1 && actual[0] == selected ? 0 : 1;
    })), 0);
    std::uint32_t actual = 0;
    EXPECT_EQ(flow.getWorkerAffinity(0, &actual, 1, count), 0);
    EXPECT_EQ(count, 1u); EXPECT_EQ(actual, selected);
    const au::flow::AffinityPolicy restore{au::flow::AffinityMode::SystemDefault, nullptr, 0};
    EXPECT_EQ(flow.setAffinity(restore), 0);
}
#endif
