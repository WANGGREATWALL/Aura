#if ENABLE_TEST_XFLOW
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flow/xflow.h"
#include "flow/xflow_core.h"
#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "sys/xsystem.h"

namespace {
template <class Predicate>
bool waitUntil(Predicate&& predicate)
{
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= limit)
            return false;
        std::this_thread::yield();
    }

    return true;
}

void expectCreated(std::initializer_list<au::flow::Task> tasks)
{
    auto&                       core = au::flow::detail::core();
    std::lock_guard<std::mutex> lock(core.mMutex);
    for (const auto& task : tasks)
        EXPECT_EQ(task.nativeHandle()->mNode->mState, au::flow::detail::TaskState::Created);
}

::testing::AssertionResult isFailedTask(const au::flow::Task& task, int expected)
{
    if (task.valid() || !task.done() || task.nativeHandle() || task.code() != expected)
        return ::testing::AssertionFailure()
               << "Expected a node-free failure " << expected << ", valid=" << task.valid() << ", done=" << task.done()
               << ", code=" << task.code();
    const int code = task.wait();
    return code == expected ? ::testing::AssertionSuccess()
                            : ::testing::AssertionFailure() << "wait() returned " << code << ", expected " << expected;
}

// Declare after borrowed state, before submitting work; TearDown runs after test locals are gone.
struct DrainTasksOnExit
{
    ~DrainTasksOnExit() { EXPECT_EQ(au::flow::Flow::get().waitAll(), 0); }
};

struct CaptureFlowLogs
{
    au::log::Config& mConfig    = au::log::Config::get();
    au::log::Level   mLevel     = mConfig.getLevel();
    bool             mShell     = mConfig.isShellPrintEnabled();
    bool             mCapturing = true;

    CaptureFlowLogs()
    {
        mConfig.setLevel(au::log::Level::Error);
        mConfig.setShellPrintEnabled(true);
        ::testing::internal::CaptureStdout();
    }

    std::string finish()
    {
        mCapturing = false;
        return ::testing::internal::GetCapturedStdout();
    }

    ~CaptureFlowLogs()
    {
        if (mCapturing)
            (void)finish();
        mConfig.setLevel(mLevel);
        mConfig.setShellPrintEnabled(mShell);
    }
};

bool exchangeClosingState(bool closing)
{
    auto&                       core = au::flow::detail::core();
    std::lock_guard<std::mutex> lock(core.mMutex);
    return std::exchange(core.mClosing, closing);
}

struct ScopedClosingState
{
    bool mPrevious;

    explicit ScopedClosingState(bool closing) : mPrevious(exchangeClosingState(closing)) {}

    ~ScopedClosingState() { exchangeClosingState(mPrevious); }
};

struct Member
{
    int add(int& value, int amount) const
    {
        value += amount;
        return 0;
    }
};

template <size_t Bytes, size_t Alignment = alignof(std::max_align_t)>
struct alignas(Alignment) PayloadProbe
{
    unsigned char      mBytes[Bytes]{};
    std::atomic<int>*  mCalls;
    std::atomic<int>*  mDestructors;
    std::atomic<bool>* mGate;
    bool               mOwns = true;

    PayloadProbe(std::atomic<int>& calls, std::atomic<int>& destructors, std::atomic<bool>* gate = nullptr)
        : mCalls(&calls), mDestructors(&destructors), mGate(gate)
    {
    }

    PayloadProbe(PayloadProbe&& other) noexcept
        : mCalls(other.mCalls), mDestructors(other.mDestructors), mGate(other.mGate)
    {
        other.mOwns = false;
    }

    PayloadProbe(const PayloadProbe&)            = delete;
    PayloadProbe& operator=(const PayloadProbe&) = delete;

    ~PayloadProbe()
    {
        if (mOwns)
            mDestructors->fetch_add(1, std::memory_order_relaxed);
    }

    int operator()()
    {
        mCalls->fetch_add(1, std::memory_order_release);
        return !mGate || waitUntil([&] { return mGate->load(std::memory_order_acquire); }) ? 0 : 1;
    }
};

template <size_t Bytes, bool NoMemory = false>
struct ThrowingPayload
{
    unsigned char mBytes[Bytes]{};
    int*          mDestructors;

    explicit ThrowingPayload(int& destructors) : mDestructors(&destructors) {}

    ThrowingPayload(const ThrowingPayload&)
    {
        if (NoMemory)
            throw std::bad_alloc();
        throw std::runtime_error("copy failed");
    }

    ~ThrowingPayload() { ++*mDestructors; }

    int operator()() { return 0; }
};

class XFlow : public ::testing::Test
{
protected:
    au::flow::Flow& mFlow = au::flow::Flow::get();

    void TearDown() override { EXPECT_EQ(mFlow.waitAll(), 0); }
};

// Task handles and payload ownership

TEST_F(XFlow, AsyncEnqueueAndWait)
{
    int value = 0;
    EXPECT_EQ(mFlow.enqueue([](int& v) { v = 4; }, std::ref(value)).wait(), 0);
    Member member;
    EXPECT_EQ(mFlow.enqueue(&Member::add, &member, std::ref(value), 3).wait(), 0);
    EXPECT_EQ(value, 7);
    std::unique_ptr<int> owned(new int(19));
    EXPECT_EQ(mFlow.enqueue([](std::unique_ptr<int> p) -> int { return *p == 19 ? 0 : 1; }, std::move(owned)).wait(),
              0);
    EXPECT_FALSE(owned);
    EXPECT_EQ(mFlow.enqueue([]() -> int { return 71; }).wait(), 71);
    EXPECT_EQ(mFlow.enqueue([]() -> int { throw std::runtime_error("unexpected"); }).wait(), au::err::kErrorUnknown);
}

TEST_F(XFlow, TaskHandleStatesAndOwnership)
{
    au::flow::Task invalid;
    EXPECT_FALSE(invalid.done());
    EXPECT_EQ(invalid.code(), au::err::kErrorInvalidParam);
    EXPECT_EQ(invalid.wait(), au::err::kErrorInvalidParam);
    auto draft = mFlow.create([] {});
    EXPECT_EQ(draft.wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(draft.code(), au::err::kErrorNotReady);
    EXPECT_FALSE(draft.done());
    auto copy = draft;
    draft     = std::move(draft);
    copy      = copy;
    EXPECT_EQ(mFlow.enqueue(draft).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(copy).wait(), 0);
    EXPECT_EQ(copy.wait(), 0);
    EXPECT_TRUE(copy.done());
    EXPECT_EQ(copy.code(), 0);
    EXPECT_EQ(mFlow.waitAll(), 0);
    EXPECT_EQ(mFlow.waitAll(), 0);
    std::weak_ptr<int> released;
    {
        auto owner               = std::make_shared<int>(7);
        released                 = owner;
        auto            detached = mFlow.create([owner] {});
        au::flow::Tasks group{detached};
        EXPECT_EQ(mFlow.enqueue(group).wait(), 0);
    }

    EXPECT_EQ(mFlow.waitAll(), 0);
    EXPECT_TRUE(released.expired());
}

TEST_F(XFlow, EnqueueOverloadResolution)
{
    std::atomic<int> calls{0};
    auto             task       = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    const auto       constant   = task;
    auto             completion = mFlow.enqueue(task);
    EXPECT_EQ(completion.nativeHandle(), task.nativeHandle());
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(constant).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(std::move(task)).wait(), 0);
    EXPECT_TRUE(task.valid());  // Enqueue copies a handle even when given an rvalue.
    EXPECT_EQ(mFlow.enqueue(mFlow.create([] {})).wait(), 0);
    EXPECT_EQ(mFlow.enqueue({}).wait(), 0);
    EXPECT_EQ(mFlow.enqueue({task}).nativeHandle(), task.nativeHandle());
    EXPECT_EQ(mFlow.enqueue({task, constant}).wait(), 0);
    au::flow::Tasks expression{task};
    const auto      view = expression;
    EXPECT_EQ(mFlow.enqueue(expression).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(view).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(std::move(expression)).wait(), 0);
    EXPECT_EQ(mFlow.enqueue([](int result) { return result; }, 13).wait(), 13);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST_F(XFlow, InlineAndLargePayloadLifetime)
{
    std::atomic<int> calls{0};
    std::atomic<int> destructors{0};
    auto             checkStorage = [&](auto payload, bool inlineStorage) {
        auto task = mFlow.create(std::move(payload));
        ASSERT_TRUE(task.valid());
        auto* handle = task.nativeHandle();
        EXPECT_EQ(handle->mNode.get(), &handle->mStorage);
        EXPECT_EQ(handle->mStorage.mPayload.get() == handle->mInlinePayload, inlineStorage);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(handle->mStorage.mPayload.get()) % alignof(decltype(payload)), 0u);
        EXPECT_EQ(mFlow.enqueue(task).wait(), 0);
    };
    checkStorage(PayloadProbe<1>(calls, destructors), true);
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 1);
    checkStorage(PayloadProbe<256>(calls, destructors), false);
    checkStorage(PayloadProbe<1, 128>(calls, destructors), false);

    {
        auto abandoned = mFlow.create(PayloadProbe<1>(calls, destructors));
        EXPECT_TRUE(abandoned.valid());
    }

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 4);

    // Dropping the last public handle must not free in-flight inline bytes.
    std::atomic<bool> release{false};
    auto              detached = mFlow.enqueue(PayloadProbe<1>(calls, destructors, &release));
    EXPECT_TRUE(waitUntil([&] { return calls.load(std::memory_order_acquire) == 4; }));
    detached = au::flow::Task();
    release.store(true, std::memory_order_release);
    EXPECT_EQ(mFlow.waitAll(), 0);
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 5);
}

TEST_F(XFlow, PayloadConstructionFailures)
{
    int failedDestructors = 0;
    {
        ThrowingPayload<1>   small(failedDestructors);
        ThrowingPayload<256> large(failedDestructors);
        auto                 failed = mFlow.create(small);
        EXPECT_TRUE(isFailedTask(failed, au::err::kErrorUnknown));
        EXPECT_EQ(mFlow.create(large).wait(), au::err::kErrorUnknown);
        ThrowingPayload<1, true> noMemory(failedDestructors);
        auto                     exhausted = mFlow.create(noMemory);
        EXPECT_TRUE(isFailedTask(exhausted, au::err::kErrorNoMemory));
        auto exhaustedCopy = exhausted;
        EXPECT_EQ(exhaustedCopy.wait(), au::err::kErrorNoMemory);
        auto afterError = mFlow.enqueueAfter(exhausted, [] {});
        EXPECT_TRUE(isFailedTask(afterError, au::err::kErrorNoMemory));
        EXPECT_EQ(failedDestructors, 0);  // Never destroy unconstructed storage.
    }

    EXPECT_EQ(failedDestructors, 3);

    void* storage = reinterpret_cast<void*>(1);
    EXPECT_EQ(au_flow_create_task_ex(nullptr, 1, 3, [](void*) { return 0; }, nullptr, &storage), nullptr);
    EXPECT_EQ(storage, nullptr);
    auto raw =
        au_flow_create_task_ex(nullptr, 64, alignof(std::max_align_t), [](void*) { return 0; }, nullptr, &storage);
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(au_flow_enqueue_task(raw), au::err::kErrorNotReady);
    au_flow_release_task(raw);
}

TEST_F(XFlow, PayloadCleanupKeepsTaskContext)
{
    // Payload cleanup remains task context, including when it submits work during closing.
    struct CleanupContext
    {
        au::flow::Task mChild;
        int            mReleaseCode = 0;
    } context;

    ScopedClosingState restore(false);
    DrainTasksOnExit   drain;

    au::flow::Task cleanup(au_flow_create_task(
        "cleanup-context",
        [](void*) -> int {
            exchangeClosingState(true);
            return 0;
        },
        &context,
        [](void* data) {
            auto& state        = *static_cast<CleanupContext*>(data);
            auto& scheduler    = au::flow::Flow::get();
            state.mReleaseCode = scheduler.releaseWorkers();
            state.mChild       = scheduler.enqueue([] {});

            exchangeClosingState(false);
        }));
    ASSERT_TRUE(cleanup.valid());
    EXPECT_EQ(mFlow.enqueue(cleanup).wait(), 0);
    EXPECT_EQ(context.mReleaseCode, au::err::kErrorFlowWouldDeadlock);
    EXPECT_TRUE(context.mChild.valid());
    EXPECT_EQ(context.mChild.wait(), 0);
}

TEST_F(XFlow, CompletedDependenciesDoNotRetainHistory)
{
    std::array<std::weak_ptr<au::flow::detail::Node>, 64> window;
    std::atomic<int>                                      calls{0};
    DrainTasksOnExit                                      drain;
    auto                                                  tail = mFlow.enqueue([] {});
    ASSERT_EQ(tail.wait(), 0);
    for (std::size_t i = 0; i < 100000; ++i) {
        auto next = mFlow.enqueueAfter(tail, [&, i] {
            calls.fetch_add(1, std::memory_order_relaxed);
            return i == 50000 ? -123 : 0;
        });
        ASSERT_EQ(next.wait(), i < 50000 ? 0 : -123) << "iteration=" << i;
        tail      = std::move(next);
        auto& old = window[i % window.size()];
        // Keep only a bounded weak window: make_shared coallocates the node and control block.
        if (i >= window.size())
            ASSERT_TRUE(waitUntil([&] { return old.expired(); })) << "iteration=" << i;
        old = tail.nativeHandle()->mNode;
    }
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 50001);  // Failed descendants were skipped too.
    ASSERT_EQ(mFlow.releaseWorkers(), 0);
    for (std::size_t i = 0; i < window.size(); ++i)
        EXPECT_EQ(window[i].expired(), i != (99999 % window.size()));
    tail = au::flow::Task();
    for (const auto& weak : window)
        EXPECT_TRUE(weak.expired());

    auto&             core = au::flow::detail::core();
    std::atomic<bool> destroyed{false};
    DrainTasksOnExit  cleanupDrain;
    auto              destroyUpstream = [&](au::flow::detail::Node* node) {
        // Reentering the scheduler would deadlock if the last owner died under its lock.
        {
            std::lock_guard<std::mutex> lock(core.mMutex);
        }
        delete node;
        destroyed.store(true, std::memory_order_release);
    };
    auto upstream  = au::flow::detail::NodePtr(new au::flow::detail::Node([](void*) { return 0; }), destroyUpstream);
    auto successor = mFlow.create([] {});
    {
        std::lock_guard<std::mutex> lock(core.mMutex);
        ASSERT_EQ(core.addDependency(upstream, successor.nativeHandle()->mNode), 0);
        ASSERT_EQ(core.submit(successor.nativeHandle()->mNode), 0);
    }
    upstream.reset();
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(waitUntil([&] { return destroyed.load(std::memory_order_acquire); }));
}

TEST_F(XFlow, FailedResultsDoNotOwnNodes)
{
    std::weak_ptr<int> lifetime;
    auto               owner = std::make_shared<int>(1);
    lifetime                 = owner;
    auto failure             = mFlow.enqueueAfter(au::flow::Task(), [owner] {});
    owner.reset();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_TRUE(isFailedTask(failure, au::err::kErrorInvalidParam));
    auto copy  = failure;
    auto moved = std::move(copy);
    EXPECT_EQ(moved.code(), au::err::kErrorInvalidParam);
    EXPECT_FALSE(copy.valid());
    EXPECT_FALSE(copy.done());

    auto businessFailure = mFlow.enqueue([] { return -123; });
    EXPECT_EQ(businessFailure.wait(), -123);
    EXPECT_TRUE(businessFailure.valid());
    EXPECT_TRUE(businessFailure.done());

    int                      destructors = 0;
    ThrowingPayload<1, true> payload(destructors);
    auto                     exhausted = mFlow.create(payload);
    std::atomic<int>         calls{0};
    auto                     next = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(exhausted.precede(next), au::err::kErrorNoMemory);
    EXPECT_EQ(next.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(mFlow.enqueue(next).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);

    auto clean = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(clean.precede(exhausted), au::err::kErrorNoMemory);
    {
        auto&                       core = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(core.mMutex);
        EXPECT_TRUE(clean.nativeHandle()->mNode->mSuccessors.empty());
    }
    EXPECT_EQ(mFlow.enqueue(clean).wait(), 0);
    EXPECT_EQ(exhausted.precede(businessFailure), au::err::kErrorNoMemory);
    EXPECT_EQ(businessFailure.code(), -123);  // Accepted results are never poisoned.
}

TEST_F(XFlow, TaskExceptionDiagnostics)
{
    CaptureFlowLogs capture;
    const int       standard =
        mFlow.enqueueNamed("std-task", []() -> int { throw std::runtime_error("task-detail"); }).wait();
    const int      memory  = mFlow.enqueueNamed("oom-task", []() -> int { throw std::bad_alloc(); }).wait();
    const int      unknown = mFlow.enqueueNamed("other-task", []() -> int { throw 7; }).wait();
    au::flow::Task cTask(
        au_flow_create_task("c-task", [](void*) -> int { throw std::runtime_error("c-detail"); }, nullptr, nullptr));
    const auto        submitted = mFlow.enqueue(cTask);
    const int         cCode     = cTask.wait();
    const std::string output    = capture.finish();
    EXPECT_EQ(standard, au::err::kErrorUnknown);
    EXPECT_EQ(memory, au::err::kErrorNoMemory);
    EXPECT_EQ(unknown, au::err::kErrorUnknown);
    EXPECT_EQ(submitted.wait(), au::err::kErrorUnknown);
    EXPECT_EQ(cCode, au::err::kErrorUnknown);
    for (const char* text : {"std-task", "task-detail", "oom-task", "other-task", "c-task", "c-detail"})
        EXPECT_NE(output.find(text), std::string::npos);
}

TEST_F(XFlow, OperationDiagnostics)
{
    CaptureFlowLogs capture;
    auto            predecessor = mFlow.createNamed("edge-predecessor", [] {});
    auto            successor   = mFlow.createNamed("edge-successor", [] {});
    EXPECT_EQ(predecessor.precede(successor), 0);
    EXPECT_EQ(successor.precede(predecessor), au::err::kErrorFlowCycleDetected);
    // The original build failure is not repeated by a later enqueue or wait.
    EXPECT_EQ(mFlow.enqueue(predecessor).code(), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(predecessor.wait(), au::err::kErrorFlowCycleDetected);

    void*          storage = nullptr;
    au::flow::Task raw(
        au_flow_create_task_ex("uncommitted-upstream", 1, 1, [](void*) { return 0; }, nullptr, &storage));
    ASSERT_TRUE(raw.valid());
    EXPECT_EQ(mFlow.enqueue(raw).code(), au::err::kErrorNotReady);
    auto downstream = mFlow.createNamed("downstream", [] {});
    EXPECT_EQ(raw.precede(downstream), 0);
    EXPECT_EQ(mFlow.enqueue(downstream).code(), au::err::kErrorNotReady);
    ::new (storage) unsigned char(0);
    EXPECT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), 0);
    EXPECT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), au::err::kErrorInvalidParam);
    EXPECT_EQ(mFlow.enqueue(downstream).wait(), 0);

    auto draft = mFlow.createNamed("draft-wait", [] {});
    EXPECT_EQ(draft.wait(), au::err::kErrorStateInvalid);
    au::flow::Task self;
    self = mFlow.createNamed("self-wait", [&] { return self.wait(); });
    EXPECT_EQ(mFlow.enqueue(self).wait(), au::err::kErrorFlowWouldDeadlock);
    mFlow.parallelFor(1, 1, [&](size_t, size_t) {
        EXPECT_EQ(mFlow.waitAll(), au::err::kErrorFlowWouldDeadlock);
        EXPECT_EQ(mFlow.releaseWorkers(), au::err::kErrorFlowWouldDeadlock);
    });

    {
        ScopedClosingState closing(true);
        EXPECT_EQ(mFlow.enqueueNamed("closing-submit", [] {}).code(), au::err::kErrorStateInvalid);
    }

    EXPECT_EQ(au_flow_create_task(nullptr, nullptr, nullptr, nullptr), nullptr);
    EXPECT_EQ(au_flow_create_task_ex(nullptr, 1, 3, [](void*) { return 0; }, nullptr, &storage), nullptr);
    EXPECT_EQ(storage, nullptr);
    au_flow_parallel_for(1, 1, nullptr, nullptr);

    const auto output = capture.finish();
    for (const char* text :
         {"add_dependency", "ErrorFlowCycleDetected", "edge-predecessor", "edge-successor", "payload not committed",
          "uncommitted-upstream", "already committed", "draft-wait", "dependency/runtime wait cycle", "self-wait",
          "wait_all", "release_workers", "runtime closing", "closing-submit", "create_task: null function",
          "create_task_ex: invalid arguments", "parallel_for: null callback"})
        EXPECT_NE(output.find(text), std::string::npos) << text;
    const auto first = output.find("flow add_dependency:");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(output.find("flow add_dependency:", first + 1), std::string::npos);
    const auto rootFailure = output.find("payload not committed");
    ASSERT_NE(rootFailure, std::string::npos);
    const auto upstreamFailure = output.find("payload not committed", rootFailure + 1);
    ASSERT_NE(upstreamFailure, std::string::npos);
    EXPECT_EQ(output.find("payload not committed", upstreamFailure + 1), std::string::npos);
    EXPECT_EQ(output.find("downstream"), std::string::npos);  // Log the failed upstream, not its submitted exit.
}

TEST_F(XFlow, NormalResultsStayQuiet)
{
    CaptureFlowLogs capture;
    auto            draft = mFlow.create([] {});
    EXPECT_EQ(draft.code(), au::err::kErrorNotReady);
    EXPECT_FALSE(draft.done());
    auto failed = mFlow.enqueueNamed("business-failure", [] { return -123; });
    EXPECT_EQ(failed.wait(), -123);
    EXPECT_EQ(failed.code(), -123);
    EXPECT_EQ(mFlow.enqueueAfter(failed, [] { ADD_FAILURE(); }).wait(), -123);
    au_flow_parallel_for(0, 0, nullptr, nullptr);
    mFlow.parallelFor(1, 0, [&](size_t, size_t) { mFlow.parallelFor(1, 1, [](size_t, size_t) {}); });
    EXPECT_TRUE(capture.finish().empty());
}

#if GTEST_HAS_DEATH_TEST
TEST_F(XFlow, TileExceptionDiagnostics)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            CaptureFlowLogs capture;
            std::set_terminate([] {
                const auto output = ::testing::internal::GetCapturedStdout();
                std::fputs(output.c_str(), stderr);
                std::_Exit(71);
            });
            mFlow.parallelFor(1, 1, [](size_t, size_t) { throw std::runtime_error("tile-detail"); });
        },
        ::testing::ExitedWithCode(71), "parallel_for callback: range=1, tile=1, index=0, worker=0.*tile-detail");
}
#endif

// Expressions and atomic submission

TEST_F(XFlow, DependencyChainAndDiamondDAG)
{
    std::atomic<int> stage{0};
    auto             a      = mFlow.create([&] { stage.store(1, std::memory_order_relaxed); });
    auto             branch = [&] {
        EXPECT_GE(stage.load(std::memory_order_relaxed), 1);
        stage.fetch_add(1, std::memory_order_relaxed);
    };
    auto b     = mFlow.create(branch);
    auto c     = mFlow.create(branch);
    auto d     = mFlow.create([&] { EXPECT_EQ(stage.load(std::memory_order_relaxed), 3); });
    auto graph = a >> (b | c) >> d;
    EXPECT_EQ(a.precede(b), 0);  // Duplicate edges are idempotent.
    ASSERT_EQ(mFlow.enqueue(graph).wait(), 0);
    ASSERT_EQ(d.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(graph).wait(), 0);
    EXPECT_EQ(stage.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, ExpressionsNamedTasksAndEmptyIdentity)
{
    std::atomic<int> count{0};
    auto             a      = mFlow.createNamed("prepare", [&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             branch = [&] {
        EXPECT_GE(count.load(std::memory_order_relaxed), 1);
        count.fetch_add(1, std::memory_order_relaxed);
    };
    auto            b = mFlow.createNamed("left", branch);
    auto            c = mFlow.createNamed("right", branch);
    auto            d = mFlow.createNamed("finish", [&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 3); });
    au::flow::Tasks empty;
    auto            graph = a >> (empty | b | c) >> empty >> d;
    auto            copy  = graph;
    graph                 = std::move(graph);
    copy                  = copy;
    auto completion       = mFlow.enqueue(graph);
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(completion.nativeHandle(), d.nativeHandle());
    EXPECT_EQ(mFlow.enqueue(copy).wait(), 0);
    EXPECT_EQ(mFlow.enqueueAfter(copy, [] {}).wait(), 0);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 3);
    auto emptyCompletion = mFlow.enqueue(empty);
    EXPECT_TRUE(emptyCompletion.valid());
    EXPECT_EQ(emptyCompletion.wait(), 0);
    EXPECT_EQ(mFlow.enqueueAfter(empty, [] {}).wait(), 0);

    auto single = mFlow.create([] {});
    EXPECT_EQ(mFlow.enqueue(empty >> single >> empty).nativeHandle(), single.nativeHandle());
    EXPECT_EQ(single.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks{empty, single, empty}).nativeHandle(), single.nativeHandle());
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks{empty, empty}).wait(), 0);
    auto failed = mFlow.enqueue(au::flow::Task() | empty);
    EXPECT_TRUE(failed.done());
    EXPECT_EQ(failed.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(mFlow.enqueue(empty >> au::flow::Task()).wait(), au::err::kErrorInvalidParam);
    auto            untouched = mFlow.create([] {});
    au::flow::Tasks rejected{au::flow::Task(), untouched};
    EXPECT_EQ(mFlow.enqueue(rejected).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(mFlow.enqueue(rejected).code(), au::err::kErrorInvalidParam);
    expectCreated({untouched});
}

TEST_F(XFlow, UnparenthesizedOperatorsFormIndependentChains)
{
    std::atomic<int>  entered{0};
    std::atomic<int>  finished{0};
    std::atomic<int>  dependents{0};
    std::atomic<bool> release{false};
    auto              root = [&] {
        entered.fetch_add(1, std::memory_order_release);
        const bool ready = waitUntil([&] { return release.load(std::memory_order_acquire); });
        finished.fetch_add(1, std::memory_order_release);
        return ready ? 0 : 1;
    };
    auto successor = [&] {
        EXPECT_TRUE(release.load(std::memory_order_acquire));
        dependents.fetch_add(1, std::memory_order_relaxed);
    };
    auto       a             = mFlow.create(root);
    auto       b             = mFlow.create(successor);
    auto       c             = mFlow.create(root);
    auto       d             = mFlow.create(successor);
    auto       completion    = mFlow.enqueue(a >> b | c >> d);  // Exactly (a >> b) | (c >> d).
    const bool parallelRoots = waitUntil([&] { return entered.load(std::memory_order_acquire) == 2; });
    release.store(true, std::memory_order_release);
    EXPECT_TRUE(parallelRoots);
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(finished.load(std::memory_order_acquire), 2);
    EXPECT_EQ(dependents.load(std::memory_order_relaxed), 2);
}

TEST_F(XFlow, NestedExpressionsAndLocalDependencies)
{
    std::atomic<unsigned> completed{0};
    auto                  make = [&](unsigned index, unsigned dependencies) {
        return mFlow.create([&, index, dependencies] {
            EXPECT_EQ(completed.load(std::memory_order_acquire) & dependencies, dependencies);
            completed.fetch_or(1u << index, std::memory_order_acq_rel);
        });
    };
    auto a     = make(0, 0);
    auto b     = make(1, 1);
    auto c     = make(2, 1);
    auto d     = make(3, 1);
    auto e     = make(4, 1u << 3);
    auto f     = make(5, 1);
    auto g     = make(6, 1);
    auto h     = make(7, 1u << 6);
    auto i     = make(8, 1u << 7);
    auto j     = make(9, 511);
    auto k     = make(10, 1u << 9);
    auto l     = make(11, 511);
    auto graph = a >> (b | c | ((d >> e) | f | (g >> h >> i))) >> ((j >> k) | l);
    EXPECT_EQ(mFlow.enqueue(graph).wait(), 0);
    EXPECT_EQ(completed.load(std::memory_order_relaxed), 4095u);

    std::atomic<int> stage{0};
    auto             first  = mFlow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 0); });
    auto             second = mFlow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 1); });
    auto             third  = mFlow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 2); });
    (void)(first >> second >> third);
    // Listing existing nodes adds a join, without replacing their local dependency edges.
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks{first, second, third}).wait(), 0);
    EXPECT_EQ(stage.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, FlatParallelRanges)
{
    std::atomic<int>            calls{0};
    std::vector<au::flow::Task> tasks;
    for (int i = 0; i < 64; ++i)
        tasks.push_back(mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); }));

    // Copies share a cursor: attempting a preliminary traversal would consume this input range.
    struct SinglePassBranches
    {
        const std::vector<au::flow::Task>* mTasks;
        std::size_t*                       mPosition;
        bool                               mEnd;

        const au::flow::Task& operator*() const { return (*mTasks)[*mPosition]; }

        SinglePassBranches& operator++()
        {
            ++*mPosition;
            return *this;
        }

        bool operator!=(const SinglePassBranches&) const { return !mEnd && *mPosition < mTasks->size(); }
    };

    std::size_t position = 0;
    auto        flat     = au::flow::Tasks::parallel(SinglePassBranches{&tasks, &position, false},
                                                     SinglePassBranches{&tasks, &position, true});
    EXPECT_EQ(position, tasks.size());
    EXPECT_EQ(mFlow.enqueue(flat).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 64);
    EXPECT_EQ(mFlow.enqueue(flat).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 64);
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks::parallel(tasks.begin(), tasks.begin())).wait(), 0);
    auto single = mFlow.enqueue(au::flow::Tasks::parallel(tasks.begin(), tasks.begin() + 1));
    EXPECT_EQ(single.nativeHandle(), tasks[0].nativeHandle());
    EXPECT_EQ(single.wait(), 0);
}

TEST_F(XFlow, SharedNodesAndDiscardedExpressions)
{
    std::atomic<int>      calls{0};
    std::atomic<unsigned> flags{0};
    auto                  a      = mFlow.create([&] { flags.fetch_or(1, std::memory_order_relaxed); });
    auto                  b      = mFlow.create([&] { flags.fetch_or(2, std::memory_order_relaxed); });
    auto                  left   = mFlow.create([&] { EXPECT_EQ(flags.load(std::memory_order_relaxed), 3u); });
    auto                  right  = mFlow.create([&] { EXPECT_EQ(flags.load(std::memory_order_relaxed), 3u); });
    auto                  shared = left | right;
    auto                  first  = a >> shared;
    auto                  second = b >> shared;  // Shared nodes accumulate both prefixes.
    EXPECT_EQ(mFlow.enqueue(first).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(second).wait(), 0);
    EXPECT_TRUE(b.done());

    auto            common = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    au::flow::Tasks firstJoin{common, mFlow.create([] {})};
    au::flow::Tasks secondJoin{common, mFlow.create([] {})};
    EXPECT_EQ(mFlow.enqueue(common).wait(), 0);  // Expressions do not reserve or own exclusive membership.
    EXPECT_EQ(mFlow.enqueue(firstJoin).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(secondJoin).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);  // Both joins share the same execution.

    auto abandoned = mFlow.create([] {});
    auto untouched = mFlow.create([] {});
    {
        auto discarded = abandoned | untouched;
    }
    EXPECT_EQ(mFlow.enqueue(abandoned).wait(), 0);
    expectCreated({untouched});  // A discarded parallel join did not add an entry before either branch.

    auto head = mFlow.create([] {});
    auto tail = mFlow.create([] {});
    {
        auto discarded = head >> tail;
    }
    EXPECT_EQ(mFlow.enqueue(tail).wait(), 0);
    EXPECT_TRUE(head.done());  // Eager serial edges survive destruction of their expression.

    auto                         duplicate  = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    auto                         serialTail = mFlow.create([] {});
    std::vector<au::flow::Tasks> fragments{duplicate >> serialTail, duplicate, au::flow::Tasks{}};
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks::parallel(fragments.begin(), fragments.end())).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 2);
}

TEST_F(XFlow, UpstreamClosureAndExpressionSubmission)
{
    std::atomic<int> roots{0};
    std::atomic<int> branches{0};
    auto             root   = mFlow.create([&] { roots.fetch_add(1, std::memory_order_relaxed); });
    auto             branch = [&] {
        EXPECT_EQ(roots.load(std::memory_order_relaxed), 1);
        branches.fetch_add(1, std::memory_order_relaxed);
    };
    auto left  = mFlow.create(branch);
    auto right = mFlow.create(branch);
    auto tail  = mFlow.create([&] { EXPECT_EQ(branches.load(std::memory_order_relaxed), 2); });
    (void)(root >> left >> tail);
    auto        chain = root >> right >> tail;
    const auto& view  = chain;
    ASSERT_EQ(mFlow.enqueue(view).wait(), 0);
    EXPECT_EQ(mFlow.enqueue(chain).nativeHandle(), tail.nativeHandle());
    EXPECT_EQ(tail.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(chain).wait(), 0);
    EXPECT_EQ(roots.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(branches.load(std::memory_order_relaxed), 2);

    auto a = mFlow.create([] {});
    auto b = mFlow.create([] {});
    EXPECT_EQ(mFlow.enqueue(a >> b).wait(), 0);  // Temporary expression must not select enqueue(callable).
    EXPECT_EQ(b.wait(), 0);
    EXPECT_TRUE(a.done());
    auto draft     = mFlow.create([] {});
    auto successor = mFlow.enqueueAfter({draft, draft}, [] {});
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(draft.done());
}

TEST_F(XFlow, MixedStateExpressionSubmission)
{
    auto done = mFlow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto              running = mFlow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return waitUntil([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire); }));
    auto pending           = mFlow.create([] {});
    auto pendingCompletion = mFlow.enqueue(running >> pending);
    EXPECT_EQ(pendingCompletion.code(), au::err::kErrorNotReady);
    auto draft     = mFlow.create([] {});
    auto tail      = mFlow.create([] {});
    auto untouched = mFlow.create([] {});
    EXPECT_EQ(done.precede(draft), 0);
    EXPECT_EQ(running.precede(draft), 0);
    EXPECT_EQ(pending.precede(draft), 0);
    auto tailCompletion = mFlow.enqueue(draft >> tail);
    EXPECT_EQ(tailCompletion.code(), au::err::kErrorNotReady);
    for (const auto& target : {done, running, pending}) {
        auto invalid = untouched >> target;
        EXPECT_EQ(mFlow.enqueue(invalid).wait(), au::err::kErrorStateInvalid);
    }
    expectCreated({untouched});
    EXPECT_FALSE(pending.done());
    release.store(true, std::memory_order_release);
    EXPECT_EQ(tail.wait(), 0);
    EXPECT_EQ(running.code(), 0);
    EXPECT_EQ(pending.code(), 0);
    EXPECT_EQ(done.code(), 0);
}

TEST_F(XFlow, ParallelAcceptedPredecessorsAndBranchErrors)
{
    auto done = mFlow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto              running = mFlow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return waitUntil([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire); }));
    std::atomic<int> count{0};
    auto             pending    = mFlow.enqueueAfter(running, [&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             consumer   = mFlow.create([&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 1); });
    auto             branches   = done | running | pending;
    auto             completion = mFlow.enqueue(branches >> consumer);
    EXPECT_FALSE(completion.done());
    auto incoming = mFlow.create([] {});
    auto rejected = mFlow.enqueue(incoming >> branches);
    EXPECT_EQ(rejected.wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(rejected.code(), au::err::kErrorStateInvalid);
    expectCreated({incoming});
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(pending.wait(), 0);

    auto failed = mFlow.enqueue([] { return -123; });
    ASSERT_EQ(failed.wait(), -123);
    auto sibling = mFlow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto skipped = mFlow.create([&] { ADD_FAILURE() << "Failed join must skip its successor"; });
    EXPECT_EQ(mFlow.enqueue((failed | sibling) >> skipped).wait(), -123);
    EXPECT_EQ(sibling.wait(), 0);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 2);

    auto notReadyResult = mFlow.enqueue([] { return au::err::kErrorNotReady; });
    ASSERT_EQ(notReadyResult.wait(), au::err::kErrorNotReady);
    auto independent = mFlow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(mFlow.enqueue(notReadyResult | independent).wait(), au::err::kErrorNotReady);
    EXPECT_EQ(independent.wait(), 0);  // A completed business error is not an expression build error.
    EXPECT_EQ(count.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, ExpressionCreationAndSubmissionErrors)
{
    int                      destructors = 0;
    ThrowingPayload<1, true> payload(destructors);
    auto                     failed    = mFlow.create(payload);
    auto                     untouched = mFlow.create([] {});
    au::flow::Tasks          empty;
    EXPECT_EQ(mFlow.enqueue(untouched >> failed).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(mFlow.enqueue(empty | failed).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(mFlow.enqueue(failed >> empty).code(), au::err::kErrorNoMemory);
    EXPECT_EQ(mFlow.enqueueAfter(failed | untouched, [] {}).wait(), au::err::kErrorNoMemory);
    expectCreated({untouched});

    void*          storage = nullptr;
    au::flow::Task raw(au_flow_create_task_ex(nullptr, 1, 1, [](void*) { return 0; }, nullptr, &storage));
    ASSERT_TRUE(raw.valid());
    auto rejected = mFlow.enqueue(raw);
    EXPECT_TRUE(isFailedTask(rejected, au::err::kErrorNotReady));
    EXPECT_FALSE(raw.done());
    ::new (storage) unsigned char(0);
    ASSERT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), 0);
    EXPECT_EQ(mFlow.enqueue(raw).wait(), 0);
    EXPECT_EQ(raw.code(), 0);  // The submission error belonged to the returned handle, not this original.
    EXPECT_EQ(rejected.wait(), au::err::kErrorNotReady);
    EXPECT_EQ(mFlow.enqueue(rejected | untouched).wait(), au::err::kErrorNotReady);
    expectCreated({untouched});

    auto head   = mFlow.create([] {});
    auto middle = mFlow.create([] {});
    auto done   = mFlow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(head >> middle >> done).wait(), au::err::kErrorStateInvalid);
    expectCreated({head, middle});
    EXPECT_EQ(mFlow.enqueue(middle).wait(), 0);  // Successful eager edges survived the later rejected edge.
    EXPECT_TRUE(head.done());

    struct ThrowingBranches
    {
        bool mNoMemory;

        const au::flow::Task& operator*() const
        {
            if (mNoMemory)
                throw std::bad_alloc();
            throw std::runtime_error("input failed");
        }

        ThrowingBranches& operator++() { return *this; }

        bool operator!=(const ThrowingBranches&) const { return true; }
    };

    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks::parallel(ThrowingBranches{true}, ThrowingBranches{true})).wait(),
              au::err::kErrorNoMemory);
    EXPECT_EQ(mFlow.enqueue(au::flow::Tasks::parallel(ThrowingBranches{false}, ThrowingBranches{false})).wait(),
              au::err::kErrorUnknown);
}

TEST_F(XFlow, ClosureValidationIsAtomic)
{
    std::atomic<int> calls{0};
    auto             good   = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    auto             member = mFlow.create([] {});
    auto             tail   = mFlow.create([] {});
    au::flow::Tasks  bad{member, au::flow::Task()};
    auto             chain = bad >> tail;
    ASSERT_EQ(good.precede(tail), 0);
    EXPECT_EQ(mFlow.enqueue(chain).wait(), au::err::kErrorInvalidParam);
    auto rejected = mFlow.enqueueAfter(bad, [] {});
    EXPECT_TRUE(isFailedTask(rejected, au::err::kErrorInvalidParam));
    expectCreated({good, member, tail});

    void*          storage = nullptr;
    au::flow::Task raw(au_flow_create_task_ex(nullptr, 1, 1, [](void*) { return 0; }, nullptr, &storage));
    ASSERT_TRUE(raw.valid());
    auto waiting = mFlow.create([] {});
    ASSERT_EQ(raw.precede(waiting), 0);
    ASSERT_EQ(good.precede(waiting), 0);
    EXPECT_EQ(mFlow.enqueue(waiting).wait(), au::err::kErrorNotReady);
    expectCreated({raw, good, waiting});
    ::new (storage) unsigned char(0);
    ASSERT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), 0);
    ASSERT_EQ(mFlow.enqueue(waiting).wait(), 0);
    EXPECT_EQ(waiting.wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);

    auto invalid = mFlow.create([] {});
    auto clean   = mFlow.create([] {});
    auto end     = mFlow.create([] {});
    EXPECT_EQ(invalid.precede(invalid), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(invalid.precede(end), 0);
    EXPECT_EQ(clean.precede(end), 0);
    EXPECT_EQ(mFlow.enqueue(end).wait(), au::err::kErrorFlowCycleDetected);
    expectCreated({invalid, clean, end});
    auto stopA = mFlow.create([] {});
    auto stopB = mFlow.create([] {});
    EXPECT_EQ(stopA.precede(stopB), 0);
    {
        ScopedClosingState closing(true);
        EXPECT_TRUE(isFailedTask(mFlow.enqueue(stopB), au::err::kErrorStateInvalid));
    }
    expectCreated({stopA, stopB});
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST_F(XFlow, DynamicEnqueueAfter)
{
    for (int round = 0; round < 40; ++round) {
        SCOPED_TRACE(::testing::Message() << "round=" << round);
        std::atomic<int>              total{0};
        auto                          body = [&] { total.fetch_add(1, std::memory_order_relaxed); };
        auto                          pred = round % 2 ? mFlow.create(body) : mFlow.enqueue(body);
        std::array<au::flow::Task, 4> after;
        std::array<std::thread, 4>    threads;
        for (std::size_t i = 0; i < threads.size(); ++i)
            threads[i] = std::thread([&, i] {
                after[i] = mFlow.enqueueAfter(pred, [&] {
                    EXPECT_GE(total.load(std::memory_order_relaxed), 1);
                    total.fetch_add(1, std::memory_order_relaxed);
                });
            });
        for (auto& t : threads)
            t.join();
        auto tail = mFlow.enqueueAfter({after[0], after[1], after[2], after[3]},
                                       [&] { EXPECT_EQ(total.load(std::memory_order_relaxed), 5); });
        EXPECT_EQ(tail.wait(), 0);
        EXPECT_EQ(mFlow.enqueueAfter(pred, [] {}).wait(), 0);  // Completed predecessor.
    }

    auto running = mFlow.enqueue([] {});
    ASSERT_EQ(running.wait(), 0);
    EXPECT_EQ(mFlow.enqueue(running >> running).wait(), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(running.code(), 0);
    EXPECT_EQ(mFlow.enqueueAfter(au::flow::Task(), [] {}).wait(), au::err::kErrorInvalidParam);
}

TEST_F(XFlow, LargeFanInGrowth)
{
    std::atomic<int> calls{0};
    auto             sink    = mFlow.create([&] { EXPECT_EQ(calls.load(std::memory_order_relaxed), 256); });
    std::size_t      growths = 0;
    for (int i = 0; i < 256; ++i) {
        auto        task     = mFlow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        std::size_t capacity = 0;
        {
            auto&                       core = au::flow::detail::core();
            std::lock_guard<std::mutex> lock(core.mMutex);
            capacity = sink.nativeHandle()->mNode->mPredecessors.capacity();
        }
        ASSERT_EQ(task.precede(sink), 0);
        {
            auto&                       core = au::flow::detail::core();
            std::lock_guard<std::mutex> lock(core.mMutex);
            growths += capacity != sink.nativeHandle()->mNode->mPredecessors.capacity();
        }
    }
    EXPECT_LT(growths, 16u);                   // Large fan-in must not reallocate once per edge.
    EXPECT_EQ(mFlow.enqueue(sink).wait(), 0);  // The DAG retains inputs after their public handles are gone.
}

TEST_F(XFlow, ErrorShortCircuiting)
{
    std::atomic<int> ran{0};
    auto             a     = mFlow.create([]() -> int { return -123; });
    auto             b     = mFlow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    auto             c     = mFlow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    auto             graph = a >> b >> c;
    EXPECT_EQ(mFlow.enqueue(graph).wait(), -123);
    EXPECT_EQ(c.wait(), -123);
    EXPECT_EQ(mFlow.waitAll(), 0);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(mFlow.enqueueAfter(a, [&] { ++ran; }).wait(), -123);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
}

TEST_F(XFlow, CycleDetectionAndDiscardedDraftLifetime)
{
    std::weak_ptr<int> observed;
    {
        auto owner            = std::make_shared<int>(1);
        observed              = owner;
        auto            a     = mFlow.create([owner] {});
        auto            b     = mFlow.create([] {});
        auto            cycle = a >> b >> a;
        au::flow::Tasks group{a, b};
        EXPECT_EQ(mFlow.enqueue(group).wait(), au::err::kErrorFlowCycleDetected);
        EXPECT_EQ(mFlow.enqueue(cycle).wait(), au::err::kErrorFlowCycleDetected);
        EXPECT_EQ(a.code(), au::err::kErrorFlowCycleDetected);
    }

    EXPECT_TRUE(observed.expired());
    auto self = mFlow.create([] {});
    EXPECT_EQ(self.precede(self), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(mFlow.enqueue(self).wait(), au::err::kErrorFlowCycleDetected);
    auto target = mFlow.enqueue([] {});
    EXPECT_EQ(target.wait(), 0);
    EXPECT_EQ(self.precede(target), au::err::kErrorStateInvalid);
    EXPECT_EQ(target.code(), 0);
    auto pred      = mFlow.create([] {});
    auto successor = mFlow.create([] {});
    EXPECT_EQ(pred.precede(successor), 0);
    EXPECT_EQ(mFlow.enqueue(successor).wait(), 0);  // Submit the unsubmitted predecessor through the successor.
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(pred.done());
}

TEST_F(XFlow, RandomizedCycleChecksWithCompletedTasksAndForkJoins)
{
    std::mt19937 random(1729);
    for (unsigned graph = 0; graph < 50; ++graph) {
        SCOPED_TRACE(::testing::Message() << "graph=" << graph);
        std::vector<au::flow::Task>            tasks;
        std::vector<au::flow::detail::NodePtr> nodes;
        std::array<std::array<bool, 32>, 32>   edges{};
        for (unsigned i = 0; i < 28; ++i)
            tasks.push_back(mFlow.create([] {}));
        for (int code : {0, -123, 0, 17}) {
            auto done = mFlow.enqueue([code] { return code; });
            ASSERT_EQ(done.wait(), code);
            tasks.push_back(done);
        }
        for (const auto& task : tasks)
            nodes.push_back(task.nativeHandle()->mNode);
        const std::pair<unsigned, unsigned> seeds[] = {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {3, 4},
                                                       {4, 5}, {4, 6}, {5, 7}, {6, 7}};
        for (const auto& edge : seeds) {
            ASSERT_EQ(tasks[edge.first].precede(tasks[edge.second]), 0);
            edges[edge.first][edge.second] = true;
        }
        {
            auto abandoned = mFlow.create([] {});
            ASSERT_EQ(tasks[14].precede(abandoned), 0);
        }  // Leave an expired weak outgoing entry: the fast-path test must remain conservative.
        auto&                       core = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(core.mMutex);
        for (unsigned edit = 0; edit < 300; ++edit) {
            const unsigned pred = random() % nodes.size();
            const unsigned succ = random() % nodes.size();
            SCOPED_TRACE(::testing::Message() << "edit=" << edit << ", edge=" << pred << "->" << succ);
            std::array<bool, 32>     visited{};
            std::array<unsigned, 32> stack{};
            unsigned                 count = 1;
            stack[0]                       = succ;
            visited[succ]                  = true;
            bool cycle                     = pred == succ;
            while (count) {
                const auto current = stack[--count];
                cycle |= current == pred;
                for (unsigned next = 0; next < nodes.size(); ++next)
                    if (edges[current][next] && !visited[next]) {
                        visited[next]  = true;
                        stack[count++] = next;
                    }
            }
            const int expected = pred == succ ? au::err::kErrorFlowCycleDetected
                                 : nodes[succ]->mState != au::flow::detail::TaskState::Created
                                     ? au::err::kErrorStateInvalid
                                 : cycle ? au::err::kErrorFlowCycleDetected
                                         : 0;
            ASSERT_EQ(core.addDependency(nodes[pred], nodes[succ]), expected);
            if (!expected)
                edges[pred][succ] = true;
        }
    }
}

// Cooperative waits and worker lifecycle

TEST_F(XFlow, CooperativeWaitAndWaitCycles)
{
    auto outer = mFlow.enqueue([&] {
        auto child = mFlow.enqueue([]() -> int { return 23; });
        return child.wait();
    });
    EXPECT_EQ(outer.wait(), 23);
    EXPECT_EQ(mFlow.enqueue([&] { return mFlow.waitAll(); }).wait(), au::err::kErrorFlowWouldDeadlock);
    std::atomic<bool> go{false};
    au::flow::Task    a;
    au::flow::Task    b;
    a = mFlow.create([&] { return waitUntil([&] { return go.load(std::memory_order_acquire); }) ? b.wait() : 1; });
    b = mFlow.create([&] { return waitUntil([&] { return go.load(std::memory_order_acquire); }) ? a.wait() : 1; });
    auto completionA = mFlow.enqueue(a);
    auto completionB = mFlow.enqueue(b);
    go.store(true, std::memory_order_release);
    EXPECT_EQ(a.wait(), au::err::kErrorFlowWouldDeadlock);
    EXPECT_EQ(b.wait(), au::err::kErrorFlowWouldDeadlock);
}

TEST_F(XFlow, CompletionQueriesAndIndependentBarriers)
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto              running = mFlow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return waitUntil([&] { return release.load(std::memory_order_acquire); }) ? au::err::kErrorNotReady : 1;
    });
    EXPECT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_FALSE(running.done());
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(running.wait(), au::err::kErrorNotReady);
    EXPECT_TRUE(running.done());
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    auto               failed = mFlow.enqueue([] { return -123; });
    std::array<int, 2> codes{{1, 1}};
    std::thread        a([&] { codes[0] = mFlow.waitAll(); });
    std::thread        b([&] { codes[1] = mFlow.waitAll(); });
    a.join();
    b.join();
    EXPECT_EQ(codes[0], 0);
    EXPECT_EQ(codes[1], 0);
    EXPECT_EQ(failed.code(), -123);
    EXPECT_EQ(failed.wait(), -123);
    EXPECT_EQ(mFlow.releaseWorkers(), 0);
    EXPECT_EQ(failed.code(), -123);
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
}

TEST_F(XFlow, LazyThreadSpawning)
{
    ASSERT_EQ(mFlow.releaseWorkers(), 0);  // Independent of test order and repeated runs.
    ASSERT_EQ(mFlow.workerCount(), 0u);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto              first = mFlow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return waitUntil([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_EQ(mFlow.workerCount(), 1u);
    std::vector<au::flow::Task> tasks;
    for (int i = 0; i < 64; ++i)
        tasks.push_back(mFlow.enqueue([] {}));
    EXPECT_LE(mFlow.workerCount(), 8u);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(first.wait(), 0);
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(mFlow.waitAll(), 0);
}

TEST_F(XFlow, ReleaseWorkersAndRespawn)
{
    for (int round = 0; round < 3; ++round) {
        EXPECT_EQ(mFlow.enqueue([] {}).wait(), 0);
        EXPECT_GT(mFlow.workerCount(), 0u);
        EXPECT_EQ(mFlow.releaseWorkers(), 0);
        EXPECT_EQ(mFlow.workerCount(), 0u);
        mFlow.parallelFor(200, 1, [](size_t, size_t) {});
        EXPECT_GT(mFlow.workerCount(), 0u);
        EXPECT_EQ(mFlow.releaseWorkers(), 0);
        EXPECT_EQ(mFlow.workerCount(), 0u);
    }

    EXPECT_EQ(mFlow.enqueue([&] { return mFlow.releaseWorkers(); }).wait(), au::err::kErrorFlowWouldDeadlock);
    mFlow.parallelFor(20, 1,
                      [&](size_t, size_t) { EXPECT_EQ(mFlow.releaseWorkers(), au::err::kErrorFlowWouldDeadlock); });

    std::atomic<int>  calls{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> drained{false};
    std::atomic<bool> submitted{false};
    std::atomic<bool> attempting{false};
    std::thread       tile([&] {
        mFlow.parallelFor(1, 1, [&](size_t, size_t) {
            entered.store(true, std::memory_order_release);
            if (waitUntil([&] { return release.load(std::memory_order_acquire); }))
                (void)mFlow.enqueue([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        });
    });
    EXPECT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire); }));
    std::thread reclaimer([&] {
        EXPECT_EQ(mFlow.releaseWorkers(), 0);
        drained.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(waitUntil([&] {
        auto&                       core = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(core.mMutex);
        return core.mReleasing;
    }));
    EXPECT_FALSE(drained.load(std::memory_order_acquire));
    au::flow::Task next;
    std::thread    producer([&] {
        attempting.store(true, std::memory_order_release);
        next = mFlow.enqueue([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        {
            auto&                       core = au::flow::detail::core();
            std::lock_guard<std::mutex> lock(core.mMutex);
            EXPECT_FALSE(core.mReleasing);  // External submission must return after the boundary opens.
        }
        submitted.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(waitUntil([&] { return attempting.load(std::memory_order_acquire); }));
    // A progress check, combined with the producer's boundary check and all joins below.
    EXPECT_FALSE(submitted.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    tile.join();
    reclaimer.join();
    producer.join();
    EXPECT_TRUE(drained.load(std::memory_order_acquire));
    EXPECT_EQ(next.wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 2);
}

// Synchronous tiles and affinity

TEST_F(XFlow, TileWorkerIndex)
{
    std::array<std::atomic<bool>, 9>  busy{};
    std::array<std::atomic<int>, 257> seen{};
    std::atomic<bool>                 caller{false};
    std::atomic<int>                  errors{0};
    const auto                        callingThread = std::this_thread::get_id();
    mFlow.parallelFor(seen.size(), 1, [&](size_t begin, size_t end, size_t worker) {
        if (worker >= busy.size()) {
            errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (worker == 0) {
            if (std::this_thread::get_id() != callingThread)
                errors.fetch_add(1, std::memory_order_relaxed);
            caller.store(true, std::memory_order_release);
        } else if (!waitUntil([&] { return caller.load(std::memory_order_acquire); })) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (busy[worker].exchange(true, std::memory_order_acq_rel))
            errors.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = begin; i < end; ++i)
            seen[i].fetch_add(1, std::memory_order_relaxed);
        mFlow.parallelFor(3, 1, [&](size_t, size_t, size_t nested) {
            if (nested != 0)
                errors.fetch_add(1, std::memory_order_relaxed);
        });
        std::this_thread::yield();
        busy[worker].store(false, std::memory_order_release);
    });
    EXPECT_TRUE(caller.load(std::memory_order_acquire));
    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0);
    for (auto& value : seen)
        EXPECT_EQ(value.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(mFlow
                  .enqueue([&] {
                      mFlow.parallelFor(20, 1, [&](size_t, size_t, size_t worker) {
                          if (worker > 8)
                              errors.fetch_add(1, std::memory_order_relaxed);
                      });
                  })
                  .wait(),
              0);
    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0);
}

TEST_F(XFlow, TiledParallelismAndNestedSafety)
{
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    for (size_t range : {size_t(0), size_t(3), size_t(257)}) {
        SCOPED_TRACE(::testing::Message() << "range=" << range);
        std::array<std::atomic<int>, 257> seen{};
        mFlow.parallelFor(range, 7, [&](size_t begin, size_t end) {
            const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
            int       old = maximum.load(std::memory_order_relaxed);
            while (old < now && !maximum.compare_exchange_weak(old, now, std::memory_order_relaxed)) {
            }
            for (size_t i = begin; i < end; ++i)
                seen[i].fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
            active.fetch_sub(1, std::memory_order_release);
        });
        for (size_t i = 0; i < seen.size(); ++i)
            EXPECT_EQ(seen[i].load(std::memory_order_relaxed), i < range ? 1 : 0);
    }

    EXPECT_LE(maximum.load(std::memory_order_relaxed), 4);
    std::array<int, 51> values{};
    XFLOW_PARALLEL_FOR_REF(values.size(), 6) values[i] = static_cast<int>(i);
    XFLOW_PARALLEL_FOR_END
    int* data = values.data();
    XFLOW_PARALLEL_FOR_VALUE(values.size(), 8) data[i] += 1;
    XFLOW_PARALLEL_FOR_END
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(values[i], static_cast<int>(i) + 1);
    std::atomic<int>              arrived{0};
    std::atomic<size_t>           total{0};
    std::array<au::flow::Task, 8> tasks;
    for (auto& task : tasks)
        task = mFlow.enqueue([&]() -> int {
            arrived.fetch_add(1, std::memory_order_release);
            if (!waitUntil([&] { return arrived.load(std::memory_order_acquire) == 8; }))
                return 1;
            mFlow.parallelFor(127, 11, [&](size_t b, size_t e) {
                mFlow.parallelFor(16, 3, [&](size_t ib, size_t ie) {
                    total.fetch_add((e - b) * (ie - ib), std::memory_order_relaxed);
                });
            });
            return 0;
        });
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 8u * 127 * 16);
    EXPECT_LE(mFlow.workerCount(), 8u);
    size_t end = 0;
    mFlow.parallelFor(SIZE_MAX, SIZE_MAX, [&](size_t b, size_t e) {
        EXPECT_EQ(b, 0u);
        end = e;
    });
    EXPECT_EQ(end, SIZE_MAX);
}

TEST_F(XFlow, CpuAffinity)
{
    std::size_t count  = 0;
    const int   status = au::sys::getThreadAffinity(nullptr, 0, count);
    if (status == au::err::kErrorNotSupported) {
        EXPECT_EQ(mFlow.setAffinity(), au::err::kErrorNotSupported);
        return;
    }
    ASSERT_EQ(status, 0);
    ASSERT_EQ(mFlow.setAffinity(), 0);
    const uint64_t allowed = mFlow.getAffinityMask();
    if (allowed) {
        const auto selected = allowed & (~allowed + 1);
        ASSERT_EQ(mFlow.setAffinity(selected), 0);
        EXPECT_EQ(mFlow.getAffinityMask(), selected);
        std::atomic<uint64_t> actual{0};
        auto                  worker = mFlow.enqueue([&] {
            std::array<uint32_t, 1024> cpus;
            std::size_t                size = 0;
            if (!au::sys::getThreadAffinity(cpus.data(), cpus.size(), size)) {
                uint64_t mask = 0;
                for (std::size_t i = 0; i < size; ++i)
                    if (cpus[i] < 64)
                        mask |= UINT64_C(1) << cpus[i];
                actual.store(mask, std::memory_order_release);
            }
        });
        // Observe execution by a pool worker, rather than helping from this unbound caller.
        EXPECT_TRUE(waitUntil([&] { return worker.done(); }));
        EXPECT_EQ(worker.wait(), 0);
        EXPECT_EQ(actual.load(std::memory_order_acquire), selected);
        std::vector<uint32_t> before(count);
        std::vector<uint32_t> after(count);
        ASSERT_EQ(au::sys::getThreadAffinity(before.data(), before.size(), count), 0);
        mFlow.parallelFor(1, 1, [](size_t, size_t) {});
        ASSERT_EQ(au::sys::getThreadAffinity(after.data(), after.size(), count), 0);
        EXPECT_EQ(before, after);
        for (auto type : {au::sys::CpuClass::kPerformance, au::sys::CpuClass::kPowerSaving}) {
            uint64_t mask = 0;
            ASSERT_EQ(au::sys::selectCpuMask(type, mask), 0);
            EXPECT_NE(mask, 0u);
            EXPECT_EQ(mask & ~allowed, 0u);
            EXPECT_EQ(mFlow.setAffinity(mask), 0);
        }
        if (~allowed) {
            const auto outside = ~allowed & (allowed + 1);
            EXPECT_EQ(mFlow.setAffinity(outside), au::err::kErrorInvalidParam);
        }
    }
    ASSERT_EQ(mFlow.setAffinity(), 0);
    EXPECT_EQ(mFlow.getAffinityMask(), allowed);
    std::vector<uint32_t> expected(count);
    std::vector<uint32_t> restored(count);
    ASSERT_EQ(au::sys::getThreadAffinity(expected.data(), expected.size(), count), 0);
    auto worker = mFlow.enqueue([&] {
        std::size_t size = 0;
        EXPECT_EQ(au::sys::getThreadAffinity(restored.data(), restored.size(), size), 0);
        restored.resize(size);
    });
    // A zero request restores the complete default CPU list, including CPUs numbered above 63.
    EXPECT_TRUE(waitUntil([&] { return worker.done(); }));
    EXPECT_EQ(worker.wait(), 0);
    EXPECT_EQ(restored, expected);
}
}  // namespace
#endif
