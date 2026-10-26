#if ENABLE_TEST_XFLOW
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "flow/xflow.h"
#include "flow/xflow_core.h"
#include "gtest/gtest.h"
#include "log/xlogger.h"
#include "sys/xsystem.h"
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif
namespace {
bool until(const std::function<bool()>& fn)
{
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fn()) {
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
        EXPECT_EQ(task.nativeHandle()->mNode->mState, au::flow::detail::State::Created);
}

// Keep the released C Group contract covered without restoring the old C++ facade.
typedef std::unique_ptr<XFlowGroupNode, void (*)(XFlowGroupHandle)> LegacyGroup;

LegacyGroup legacyGroup(std::initializer_list<au::flow::Task> members)
{
    LegacyGroup group(au_flow_create_group(nullptr), &au_flow_release_group);
    for (const auto& task : members)
        if (au_flow_group_add_task(group.get(), task.nativeHandle()))
            break;
    return group;
}

struct Member
{
    int add(int& value, int amount) const
    {
        value += amount;
        return 0;
    }
};
}  // namespace

class XFlow : public ::testing::Test
{
protected:
    void TearDown() override { (void)au::flow::Flow::get().waitAll(); }
};

TEST_F(XFlow, LazyThreadSpawning)
{
    auto& flow = au::flow::Flow::get();
    ASSERT_EQ(flow.releaseWorkers(), 0);  // Independent of test order and repeated runs.
    ASSERT_EQ(flow.workerCount(), 0u);
    std::atomic<bool> entered{false}, release{false};
    auto              first = flow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return until([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_EQ(flow.workerCount(), 1u);
    std::vector<au::flow::Task> tasks;
    for (int i = 0; i < 64; ++i)
        tasks.push_back(flow.enqueue([] {}));
    EXPECT_LE(flow.workerCount(), 8u);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(first.wait(), 0);
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(flow.waitAll(), 0);
}

namespace {
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
        return !mGate || until([&] { return mGate->load(std::memory_order_acquire); }) ? 0 : 1;
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
}  // namespace

TEST_F(XFlow, InlineAndLargePayloadLifetime)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> calls{0}, destructors{0};
    {
        auto small = flow.create(PayloadProbe<1>(calls, destructors));
        ASSERT_TRUE(small.valid());
        auto* handle = small.nativeHandle();
        EXPECT_TRUE(handle->mUsesInlinePayload);
        EXPECT_EQ(handle->mNode.get(), &handle->mStorage);
        EXPECT_EQ(handle->mNode->mPayload.get(), handle->mInlinePayload);
        ASSERT_EQ(flow.enqueue(small).wait(), 0);
        EXPECT_EQ(small.wait(), 0);
        EXPECT_EQ(destructors.load(std::memory_order_relaxed), 1);
    }

    {
        auto large = flow.create(PayloadProbe<256>(calls, destructors));
        ASSERT_TRUE(large.valid());
        EXPECT_FALSE(large.nativeHandle()->mUsesInlinePayload);
        ASSERT_EQ(flow.enqueue(large).wait(), 0);
        EXPECT_EQ(large.wait(), 0);
    }

    {
        auto aligned = flow.create(PayloadProbe<1, 128>(calls, destructors));
        ASSERT_TRUE(aligned.valid());
        EXPECT_FALSE(aligned.nativeHandle()->mUsesInlinePayload);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned.nativeHandle()->mNode->mPayload.get()) % 128, 0u);
        ASSERT_EQ(flow.enqueue(aligned).wait(), 0);
        EXPECT_EQ(aligned.wait(), 0);
    }

    {
        auto abandoned = flow.create(PayloadProbe<1>(calls, destructors));
        EXPECT_TRUE(abandoned.valid());
    }

    EXPECT_EQ(calls.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 4);

    // Dropping the last public handle must not free in-flight inline bytes.
    std::atomic<bool> release{false};
    auto              detached = flow.enqueue(PayloadProbe<1>(calls, destructors, &release));
    EXPECT_TRUE(until([&] { return calls.load(std::memory_order_acquire) == 4; }));
    detached = au::flow::Task();
    release.store(true, std::memory_order_release);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(destructors.load(std::memory_order_relaxed), 5);

    int failedDestructors = 0;
    {
        ThrowingPayload<1>   small(failedDestructors);
        ThrowingPayload<256> large(failedDestructors);
        auto                 failed = flow.create(small);
        EXPECT_FALSE(failed.valid());
        EXPECT_TRUE(failed.done());
        EXPECT_EQ(failed.code(), au::err::kErrorUnknown);
        EXPECT_EQ(failed.wait(), au::err::kErrorUnknown);
        EXPECT_EQ(flow.create(large).wait(), au::err::kErrorUnknown);
        ThrowingPayload<1, true> noMemory(failedDestructors);
        auto                     exhausted = flow.create(noMemory);
        EXPECT_FALSE(exhausted.valid());
        EXPECT_TRUE(exhausted.done());
        EXPECT_EQ(exhausted.code(), au::err::kErrorNoMemory);
        auto exhaustedCopy = exhausted;
        EXPECT_EQ(exhaustedCopy.wait(), au::err::kErrorNoMemory);
        auto afterError = flow.enqueueAfter(exhausted, [] {});
        EXPECT_TRUE(afterError.done());
        EXPECT_EQ(afterError.code(), au::err::kErrorNoMemory);
        EXPECT_EQ(afterError.wait(), au::err::kErrorNoMemory);
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

TEST_F(XFlow, CompletedDependenciesDoNotRetainHistory)
{
    auto&                                                 flow = au::flow::Flow::get();
    std::array<std::weak_ptr<au::flow::detail::Node>, 64> window;
    std::atomic<int>                                      calls{0};
    auto                                                  tail = flow.enqueue([] {});
    ASSERT_EQ(tail.wait(), 0);
    for (std::size_t i = 0; i < 100000; ++i) {
        auto next = flow.enqueueAfter(tail, [&, i] {
            calls.fetch_add(1, std::memory_order_relaxed);
            return i == 50000 ? -123 : 0;
        });
        ASSERT_EQ(next.wait(), i < 50000 ? 0 : -123);
        tail      = std::move(next);
        auto& old = window[i % window.size()];
        // Keep only a bounded weak window: make_shared coallocates the node and control block.
        if (i >= window.size())
            ASSERT_TRUE(until([&] { return old.expired(); }));
        old = tail.nativeHandle()->mNode;
    }
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 50001);  // Failed descendants were skipped too.
    ASSERT_EQ(flow.releaseWorkers(), 0);
    for (std::size_t i = 0; i < window.size(); ++i)
        EXPECT_EQ(window[i].expired(), i != (99999 % window.size()));
    tail = au::flow::Task();
    for (const auto& weak : window)
        EXPECT_TRUE(weak.expired());

    auto&             core = au::flow::detail::core();
    std::atomic<bool> destroyed{false};
    auto              upstream  = au::flow::detail::NodePtr(new au::flow::detail::Node(
                                                  nullptr, [](void*) { return 0; }, nullptr, nullptr),
                                                            [&](au::flow::detail::Node* node) {
                                                  // Reentrant scheduler access would deadlock if the last predecessor
                                                  // owner died under its lock.
                                                  {
                                                      std::lock_guard<std::mutex> lock(core.mMutex);
                                                  }
                                                  delete node;
                                                  destroyed.store(true, std::memory_order_release);
                                              });
    auto              successor = flow.create([] {});
    {
        std::lock_guard<std::mutex> lock(core.mMutex);
        ASSERT_EQ(core.edge(upstream, successor.nativeHandle()->mNode), 0);
        ASSERT_EQ(core.submit(successor.nativeHandle()->mNode), 0);
    }
    upstream.reset();
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(until([&] { return destroyed.load(std::memory_order_acquire); }));
}

TEST_F(XFlow, FailedResultsDoNotOwnNodes)
{
    auto&              flow = au::flow::Flow::get();
    std::weak_ptr<int> lifetime;
    auto               owner = std::make_shared<int>(1);
    lifetime                 = owner;
    auto failure             = flow.enqueueAfter(au::flow::Task(), [owner] {});
    owner.reset();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(failure.valid());
    EXPECT_TRUE(failure.done());
    EXPECT_EQ(failure.nativeHandle(), nullptr);
    EXPECT_EQ(failure.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(failure.costUs(), 0);
    auto copy  = failure;
    auto moved = std::move(copy);
    EXPECT_EQ(moved.code(), au::err::kErrorInvalidParam);
    EXPECT_FALSE(copy.valid());
    EXPECT_FALSE(copy.done());

    auto businessFailure = flow.enqueue([] { return -123; });
    EXPECT_EQ(businessFailure.wait(), -123);
    EXPECT_TRUE(businessFailure.valid());
    EXPECT_TRUE(businessFailure.done());

    int                      destructors = 0;
    ThrowingPayload<1, true> payload(destructors);
    auto                     exhausted = flow.create(payload);
    std::atomic<int>         calls{0};
    auto                     next = flow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(exhausted.precede(next), au::err::kErrorNoMemory);
    EXPECT_EQ(next.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueue(next).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);

    auto clean = flow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(clean.precede(exhausted), au::err::kErrorNoMemory);
    {
        auto&                       core = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(core.mMutex);
        EXPECT_TRUE(clean.nativeHandle()->mNode->mSuccessors.empty());
    }
    EXPECT_EQ(flow.enqueue(clean).wait(), 0);
    EXPECT_EQ(exhausted.precede(businessFailure), au::err::kErrorNoMemory);
    EXPECT_EQ(businessFailure.code(), -123);  // Accepted results are never poisoned.
}

TEST_F(XFlow, ReleaseWorkersAndRespawn)
{
    auto& flow = au::flow::Flow::get();
    for (int round = 0; round < 3; ++round) {
        EXPECT_EQ(flow.enqueue([] {}).wait(), 0);
        EXPECT_GT(flow.workerCount(), 0u);
        EXPECT_EQ(flow.releaseWorkers(), 0);
        EXPECT_EQ(flow.workerCount(), 0u);
        flow.parallelizeTiledTasks(200, 1, [](size_t, size_t) {});
        EXPECT_GT(flow.workerCount(), 0u);
        EXPECT_EQ(flow.releaseWorkers(), 0);
        EXPECT_EQ(flow.workerCount(), 0u);
    }

    EXPECT_EQ(flow.enqueue([&] { return flow.releaseWorkers(); }).wait(), au::err::kErrorFlowWouldDeadlock);
    flow.parallelizeTiledTasks(
        20, 1, [&](size_t, size_t) { EXPECT_EQ(flow.releaseWorkers(), au::err::kErrorFlowWouldDeadlock); });

    std::atomic<int>  calls{0};
    std::atomic<bool> entered{false}, release{false}, drained{false}, submitted{false};
    std::thread       tile([&] {
        flow.parallelizeTiledTasks(1, 1, [&](size_t, size_t) {
            entered.store(true, std::memory_order_release);
            if (until([&] { return release.load(std::memory_order_acquire); }))
                (void)flow.enqueue([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        });
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    std::thread reclaimer([&] {
        EXPECT_EQ(flow.releaseWorkers(), 0);
        drained.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(until([&] {
        auto&                       core = au::flow::detail::core();
        std::lock_guard<std::mutex> lock(core.mMutex);
        return core.mReleasing;
    }));
    EXPECT_FALSE(drained.load(std::memory_order_acquire));
    au::flow::Task next;
    std::thread    producer([&] {
        next = flow.enqueue([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        submitted.store(true, std::memory_order_release);
    });
    EXPECT_FALSE(submitted.load(std::memory_order_acquire));
    release.store(true, std::memory_order_release);
    tile.join();
    reclaimer.join();
    producer.join();
    EXPECT_TRUE(drained.load(std::memory_order_acquire));
    EXPECT_EQ(next.wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 2);
}

TEST_F(XFlow, TileWorkerIndex)
{
    auto&                             flow = au::flow::Flow::get();
    std::array<std::atomic<bool>, 9>  busy{};
    std::array<std::atomic<int>, 257> seen{};
    std::atomic<bool>                 caller{false};
    std::atomic<int>                  errors{0};
    const auto                        callingThread = std::this_thread::get_id();
    flow.parallelizeTiledTasks(seen.size(), 1, [&](size_t begin, size_t end, size_t worker) {
        if (worker >= busy.size()) {
            errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (worker == 0) {
            if (std::this_thread::get_id() != callingThread)
                errors.fetch_add(1, std::memory_order_relaxed);
            caller.store(true, std::memory_order_release);
        } else if (!until([&] { return caller.load(std::memory_order_acquire); })) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (busy[worker].exchange(true, std::memory_order_acq_rel))
            errors.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = begin; i < end; ++i)
            seen[i].fetch_add(1, std::memory_order_relaxed);
        flow.parallelizeTiledTasks(3, 1, [&](size_t, size_t, size_t nested) {
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
    EXPECT_EQ(flow.enqueue([&] {
                      flow.parallelizeTiledTasks(20, 1, [&](size_t, size_t, size_t worker) {
                          if (worker > 8)
                              errors.fetch_add(1, std::memory_order_relaxed);
                      });
                  })
                  .wait(),
              0);
    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0);
}

TEST_F(XFlow, AsyncEnqueueAndWait)
{
    auto& flow  = au::flow::Flow::get();
    int   value = 0;
    EXPECT_EQ(flow.enqueue([](int& v) { v = 4; }, std::ref(value)).wait(), 0);
    Member member;
    EXPECT_EQ(flow.enqueue(&Member::add, &member, std::ref(value), 3).wait(), 0);
    EXPECT_EQ(value, 7);
    std::unique_ptr<int> owned(new int(19));
    EXPECT_EQ(flow.enqueue([](std::unique_ptr<int> p) -> int { return *p == 19 ? 0 : 1; }, std::move(owned)).wait(), 0);
    EXPECT_FALSE(owned);
    EXPECT_EQ(flow.enqueue([]() -> int { return 71; }).wait(), 71);
    EXPECT_EQ(flow.enqueue([]() -> int { throw std::runtime_error("unexpected"); }).wait(), au::err::kErrorUnknown);
    au::flow::Task invalid;
    EXPECT_FALSE(invalid.done());
    EXPECT_EQ(invalid.code(), au::err::kErrorInvalidParam);
    EXPECT_EQ(invalid.wait(), au::err::kErrorInvalidParam);
    auto draft = flow.create([] {});
    EXPECT_EQ(draft.wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(draft.code(), au::err::kErrorNotReady);
    EXPECT_FALSE(draft.done());
    auto copy = draft;
    draft     = std::move(draft);
    copy      = copy;
    EXPECT_EQ(flow.enqueue(draft).wait(), 0);
    EXPECT_EQ(flow.enqueue(copy).wait(), 0);
    EXPECT_EQ(copy.wait(), 0);
    EXPECT_TRUE(copy.done());
    EXPECT_EQ(copy.code(), 0);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(flow.waitAll(), 0);
    std::weak_ptr<int> released;
    {
        auto owner               = std::make_shared<int>(7);
        released                 = owner;
        auto            detached = flow.create([owner] {});
        au::flow::Tasks group{detached};
        EXPECT_EQ(flow.enqueue(group).wait(), 0);
    }

    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_TRUE(released.expired());
}

TEST_F(XFlow, DependencyChainAndDiamondDAG)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> stage{0};
    auto             a     = flow.create([&] { stage.store(1, std::memory_order_relaxed); });
    auto             b     = flow.create([&] {
        EXPECT_GE(stage.load(std::memory_order_relaxed), 1);
        stage.fetch_add(1, std::memory_order_relaxed);
    });
    auto             c     = flow.create([&] {
        EXPECT_GE(stage.load(std::memory_order_relaxed), 1);
        stage.fetch_add(1, std::memory_order_relaxed);
    });
    auto             d     = flow.create([&] { EXPECT_EQ(stage.load(std::memory_order_relaxed), 3); });
    auto             graph = a >> (b | c) >> d;
    EXPECT_EQ(a.precede(b), 0);  // Duplicate edges are idempotent.
    ASSERT_EQ(flow.enqueue(graph).wait(), 0);
    ASSERT_EQ(d.wait(), 0);
    EXPECT_EQ(flow.enqueue(graph).wait(), 0);
    EXPECT_EQ(stage.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, CompletionQueriesAndIndependentBarriers)
{
    auto&             flow = au::flow::Flow::get();
    std::atomic<bool> entered{false}, release{false};
    auto              running = flow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return until([&] { return release.load(std::memory_order_acquire); }) ? au::err::kErrorNotReady : 1;
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    EXPECT_FALSE(running.done());
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    EXPECT_EQ(running.costUs(), 0);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(running.wait(), au::err::kErrorNotReady);
    EXPECT_TRUE(running.done());
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    auto               failed = flow.enqueue([] { return -123; });
    std::array<int, 2> codes{{1, 1}};
    std::thread        a([&] { codes[0] = flow.waitAll(); });
    std::thread        b([&] { codes[1] = flow.waitAll(); });
    a.join();
    b.join();
    EXPECT_EQ(codes[0], 0);
    EXPECT_EQ(codes[1], 0);
    EXPECT_EQ(failed.code(), -123);
    EXPECT_EQ(failed.wait(), -123);
    EXPECT_EQ(flow.releaseWorkers(), 0);
    EXPECT_EQ(failed.code(), -123);
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
}

TEST_F(XFlow, UpstreamClosureAndExpressionSubmission)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> roots{0}, branches{0};
    auto             root  = flow.create([&] { roots.fetch_add(1, std::memory_order_relaxed); });
    auto             left  = flow.create([&] {
        EXPECT_EQ(roots.load(std::memory_order_relaxed), 1);
        branches.fetch_add(1, std::memory_order_relaxed);
    });
    auto             right = flow.create([&] {
        EXPECT_EQ(roots.load(std::memory_order_relaxed), 1);
        branches.fetch_add(1, std::memory_order_relaxed);
    });
    auto             tail  = flow.create([&] { EXPECT_EQ(branches.load(std::memory_order_relaxed), 2); });
    (void)(root >> left >> tail);
    auto        chain = root >> right >> tail;
    const auto& view  = chain;
    ASSERT_EQ(flow.enqueue(view).wait(), 0);
    EXPECT_EQ(flow.enqueue(chain).nativeHandle(), tail.nativeHandle());
    EXPECT_EQ(tail.wait(), 0);
    EXPECT_EQ(flow.enqueue(chain).wait(), 0);
    EXPECT_EQ(roots.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(branches.load(std::memory_order_relaxed), 2);

    auto a = flow.create([] {}), b = flow.create([] {});
    EXPECT_EQ(flow.enqueue(a >> b).wait(), 0);  // Temporary expression must not select enqueue(callable).
    EXPECT_EQ(b.wait(), 0);
    EXPECT_TRUE(a.done());
    auto draft     = flow.create([] {});
    auto successor = flow.enqueueAfter({draft, draft}, [] {});
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(draft.done());
}

TEST_F(XFlow, MixedStateExpressionSubmission)
{
    auto& flow = au::flow::Flow::get();
    auto  done = flow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    std::atomic<bool> entered{false}, release{false};
    auto              running = flow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return until([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    auto pending           = flow.create([] {});
    auto pendingCompletion = flow.enqueue(running >> pending);
    EXPECT_EQ(pendingCompletion.code(), au::err::kErrorNotReady);
    auto draft = flow.create([] {}), tail = flow.create([] {}), untouched = flow.create([] {});
    EXPECT_EQ(done.precede(draft), 0);
    EXPECT_EQ(running.precede(draft), 0);
    EXPECT_EQ(pending.precede(draft), 0);
    auto tailCompletion = flow.enqueue(draft >> tail);
    EXPECT_EQ(tailCompletion.code(), au::err::kErrorNotReady);
    for (const auto& target : {done, running, pending}) {
        auto invalid = untouched >> target;
        EXPECT_EQ(flow.enqueue(invalid).wait(), au::err::kErrorStateInvalid);
    }
    expectCreated({untouched});
    EXPECT_FALSE(pending.done());
    release.store(true, std::memory_order_release);
    EXPECT_EQ(tail.wait(), 0);
    EXPECT_EQ(running.code(), 0);
    EXPECT_EQ(pending.code(), 0);
    EXPECT_EQ(done.code(), 0);
}

TEST_F(XFlow, LegacyGroupCompatibility)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> count{0};
    auto             a     = flow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             b     = flow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             c     = flow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             d     = flow.create([&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 3); });
    auto             first = legacyGroup({b, b}), second = legacyGroup({c});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(au_flow_add_task_group_dependency(a.nativeHandle(), first.get()), 0);
    ASSERT_EQ(au_flow_add_group_dependency(first.get(), second.get()), 0);
    ASSERT_EQ(au_flow_add_group_task_dependency(second.get(), d.nativeHandle()), 0);
    ASSERT_EQ(flow.enqueue(d).wait(), 0);
    EXPECT_EQ(au_flow_wait_group(first.get()), 0);
    EXPECT_EQ(au_flow_wait_group(second.get()), 0);
    EXPECT_EQ(au_flow_enqueue_group(second.get()), 0);
    EXPECT_FALSE(b.nativeHandle()->mNode->mGroupExit.expired());
    EXPECT_EQ(au_flow_enqueue_task(b.nativeHandle()), 0);  // Accepted members bypass Draft group-boundary checks.
    EXPECT_EQ(flow.enqueue(b).wait(), 0);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 3);

    auto transientMember = flow.create([] {}), transientTail = flow.create([] {});
    {
        auto transient = legacyGroup({transientMember});
        ASSERT_EQ(au_flow_add_group_task_dependency(transient.get(), transientTail.nativeHandle()), 0);
    }
    EXPECT_EQ(flow.enqueue(transientMember).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueue(transientTail).wait(), 0);  // The tail retained the abandoned group's exit.

    auto x = flow.create([] {}), y = flow.create([] {}), tail = flow.create([] {});
    auto group = legacyGroup({x, y});
    ASSERT_EQ(x.precede(tail), 0);
    EXPECT_EQ(flow.enqueue(x).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueue(tail).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueueAfter(x, [] {}).wait(), au::err::kErrorInvalidParam);
    expectCreated({x, y, tail});
    ASSERT_EQ(au_flow_enqueue_group(group.get()), 0);
    ASSERT_EQ(au_flow_wait_group(group.get()), 0);
    EXPECT_EQ(flow.enqueue(tail).wait(), 0);

    auto               orphan = flow.create([] {});
    std::weak_ptr<int> released;
    {
        auto owner     = std::make_shared<int>(1);
        released       = owner;
        auto abandoned = legacyGroup({orphan, flow.create([owner] {})});
    }
    EXPECT_TRUE(released.expired());
    EXPECT_EQ(flow.enqueue(orphan).wait(), 0);
    auto regrouped = flow.create([] {});
    {
        auto abandoned = legacyGroup({regrouped});
    }
    auto replacement = legacyGroup({regrouped});
    ASSERT_EQ(au_flow_enqueue_group(replacement.get()), 0);
    EXPECT_EQ(au_flow_wait_group(replacement.get()), 0);

    std::atomic<int> stage{0};
    auto             upstream = flow.create([&] { stage.store(1, std::memory_order_relaxed); });
    auto             survivor = flow.create([&] { EXPECT_EQ(stage.load(std::memory_order_relaxed), 1); });
    {
        auto abandoned = legacyGroup({survivor});
        ASSERT_EQ(au_flow_add_task_group_dependency(upstream.nativeHandle(), abandoned.get()), 0);
    }
    EXPECT_EQ(flow.enqueue(survivor).wait(), 0);
    EXPECT_TRUE(upstream.done());

    auto guarded  = flow.create([] {});
    auto retained = legacyGroup({guarded});
    au_flow_retain_group(retained.get());
    {
        LegacyGroup copy(retained.get(), &au_flow_release_group);
    }
    EXPECT_EQ(flow.enqueue(guarded).wait(), au::err::kErrorInvalidParam);
    expectCreated({guarded});
    retained.reset();
    EXPECT_EQ(flow.enqueue(guarded).wait(), 0);

    auto failed = flow.create([] {});
    {
        auto abandoned = legacyGroup({failed});
        EXPECT_EQ(au_flow_group_add_task(abandoned.get(), nullptr), au::err::kErrorInvalidParam);
        EXPECT_EQ(au_flow_enqueue_group(abandoned.get()), au::err::kErrorInvalidParam);
        EXPECT_EQ(au_flow_wait_group(abandoned.get()), au::err::kErrorInvalidParam);
    }
    EXPECT_EQ(flow.enqueue(failed).wait(), au::err::kErrorInvalidParam);
    expectCreated({failed});
}

TEST_F(XFlow, ClosureValidationIsAtomic)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> calls{0};
    auto             good   = flow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    auto             member = flow.create([] {}), tail = flow.create([] {});
    au::flow::Tasks  bad{member, au::flow::Task()};
    auto             chain = bad >> tail;
    ASSERT_EQ(good.precede(tail), 0);
    EXPECT_EQ(flow.enqueue(chain).wait(), au::err::kErrorInvalidParam);
    auto rejected = flow.enqueueAfter(bad, [] {});
    EXPECT_EQ(rejected.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(rejected.code(), au::err::kErrorInvalidParam);
    EXPECT_TRUE(rejected.done());
    expectCreated({good, member, tail});

    void*          storage = nullptr;
    au::flow::Task raw(au_flow_create_task_ex(nullptr, 1, 1, [](void*) { return 0; }, nullptr, &storage));
    ASSERT_TRUE(raw.valid());
    auto waiting = flow.create([] {});
    ASSERT_EQ(raw.precede(waiting), 0);
    ASSERT_EQ(good.precede(waiting), 0);
    EXPECT_EQ(flow.enqueue(waiting).wait(), au::err::kErrorNotReady);
    expectCreated({raw, good, waiting});
    ::new (storage) unsigned char(0);
    ASSERT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), 0);
    ASSERT_EQ(flow.enqueue(waiting).wait(), 0);
    EXPECT_EQ(waiting.wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);

    auto invalid = flow.create([] {}), clean = flow.create([] {}), end = flow.create([] {});
    EXPECT_EQ(invalid.precede(invalid), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(invalid.precede(end), 0);
    EXPECT_EQ(clean.precede(end), 0);
    EXPECT_EQ(flow.enqueue(end).wait(), au::err::kErrorFlowCycleDetected);
    expectCreated({invalid, clean, end});
    auto stopA = flow.create([] {}), stopB = flow.create([] {});
    EXPECT_EQ(stopA.precede(stopB), 0);
    auto& core = au::flow::detail::core();
    {
        std::lock_guard<std::mutex> lock(core.mMutex);
        core.mClosing = true;
    }
    const auto stopped = flow.enqueue(stopB);
    {
        std::lock_guard<std::mutex> lock(core.mMutex);
        core.mClosing = false;
    }
    EXPECT_EQ(stopped.wait(), au::err::kErrorStateInvalid);
    expectCreated({stopA, stopB});
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST_F(XFlow, TaskExceptionDiagnostics)
{
    auto&      flow  = au::flow::Flow::get();
    auto&      log   = au::log::Config::get();
    const auto level = log.getLevel();
    const bool shell = log.isShellPrintEnabled();
    log.setLevel(au::log::Level::Error);
    log.setShellPrintEnabled(true);
    ::testing::internal::CaptureStdout();
    const int standard = flow.enqueueNamed("std-task", []() -> int { throw std::runtime_error("task-detail"); }).wait();
    const int memory   = flow.enqueueNamed("oom-task", []() -> int { throw std::bad_alloc(); }).wait();
    const int unknown  = flow.enqueueNamed("other-task", []() -> int { throw 7; }).wait();
    au::flow::Task cTask(
        au_flow_create_task("c-task", [](void*) -> int { throw std::runtime_error("c-detail"); }, nullptr, nullptr));
    const auto        submitted = flow.enqueue(cTask);
    const int         cCode     = cTask.wait();
    const std::string output    = ::testing::internal::GetCapturedStdout();
    log.setLevel(level);
    log.setShellPrintEnabled(shell);
    EXPECT_EQ(standard, au::err::kErrorUnknown);
    EXPECT_EQ(memory, au::err::kErrorNoMemory);
    EXPECT_EQ(unknown, au::err::kErrorUnknown);
    EXPECT_EQ(submitted.wait(), au::err::kErrorUnknown);
    EXPECT_EQ(cCode, au::err::kErrorUnknown);
    for (const char* text : {"std-task", "task-detail", "oom-task", "other-task", "c-task", "c-detail"})
        EXPECT_NE(output.find(text), std::string::npos);
}

TEST_F(XFlow, DynamicEnqueueAfter)
{
    auto& flow = au::flow::Flow::get();
    for (int round = 0; round < 40; ++round) {
        std::atomic<int>              total{0};
        auto                          body = [&] { total.fetch_add(1, std::memory_order_relaxed); };
        auto                          pred = round % 2 ? flow.create(body) : flow.enqueue(body);
        std::array<au::flow::Task, 4> after;
        std::array<std::thread, 4>    threads;
        for (std::size_t i = 0; i < threads.size(); ++i)
            threads[i] = std::thread([&, i] {
                after[i] = flow.enqueueAfter(pred, [&] {
                    EXPECT_GE(total.load(std::memory_order_relaxed), 1);
                    total.fetch_add(1, std::memory_order_relaxed);
                });
            });
        for (auto& t : threads)
            t.join();
        auto tail = flow.enqueueAfter({after[0], after[1], after[2], after[3]},
                                      [&] { EXPECT_EQ(total.load(std::memory_order_relaxed), 5); });
        EXPECT_EQ(tail.wait(), 0);
        EXPECT_EQ(flow.enqueueAfter(pred, [] {}).wait(), 0);  // Completed predecessor.
    }

    auto running = flow.enqueue([] {});
    ASSERT_EQ(running.wait(), 0);
    EXPECT_EQ(flow.enqueue(running >> running).wait(), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(running.code(), 0);
    EXPECT_EQ(flow.enqueueAfter(au::flow::Task(), [] {}).wait(), au::err::kErrorInvalidParam);
}

TEST_F(XFlow, ExpressionsNamedTasksAndEmptyIdentity)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> count{0};
    auto             a = flow.createNamed("prepare", [&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             b = flow.createNamed("left", [&] {
        EXPECT_GE(count.load(std::memory_order_relaxed), 1);
        count.fetch_add(1, std::memory_order_relaxed);
    });
    auto             c = flow.createNamed("right", [&] {
        EXPECT_GE(count.load(std::memory_order_relaxed), 1);
        count.fetch_add(1, std::memory_order_relaxed);
    });
    auto             d = flow.createNamed("finish", [&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 3); });
    au::flow::Tasks  empty;
    auto             graph = a >> (empty | b | c) >> empty >> d;
    auto             copy  = graph;
    graph                  = std::move(graph);
    copy                   = copy;
    auto completion        = flow.enqueue(graph);
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(completion.nativeHandle(), d.nativeHandle());
    EXPECT_EQ(flow.enqueue(copy).wait(), 0);
    EXPECT_EQ(flow.enqueueAfter(copy, [] {}).wait(), 0);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 3);
    EXPECT_GE(a.costUs(), 0);
    auto emptyCompletion = flow.enqueue(empty);
    EXPECT_TRUE(emptyCompletion.valid());
    EXPECT_EQ(emptyCompletion.wait(), 0);
    EXPECT_EQ(flow.enqueueAfter(empty, [] {}).wait(), 0);

    auto single = flow.create([] {});
    EXPECT_EQ(flow.enqueue(empty >> single >> empty).nativeHandle(), single.nativeHandle());
    EXPECT_EQ(single.wait(), 0);
    EXPECT_EQ(flow.enqueue(au::flow::Tasks{empty, single, empty}).nativeHandle(), single.nativeHandle());
    EXPECT_EQ(flow.enqueue(au::flow::Tasks{empty, empty}).wait(), 0);
    auto failed = flow.enqueue(au::flow::Task() | empty);
    EXPECT_TRUE(failed.done());
    EXPECT_EQ(failed.wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueue(empty >> au::flow::Task()).wait(), au::err::kErrorInvalidParam);
    auto            untouched = flow.create([] {});
    au::flow::Tasks rejected{au::flow::Task(), untouched};
    EXPECT_EQ(flow.enqueue(rejected).wait(), au::err::kErrorInvalidParam);
    EXPECT_EQ(flow.enqueue(rejected).code(), au::err::kErrorInvalidParam);
    expectCreated({untouched});
}

TEST_F(XFlow, NestedExpressionsAndLocalDependencies)
{
    auto&                 flow = au::flow::Flow::get();
    std::atomic<unsigned> completed{0};
    auto                  make = [&](unsigned index, unsigned dependencies) {
        return flow.create([&, index, dependencies] {
            EXPECT_EQ(completed.load(std::memory_order_acquire) & dependencies, dependencies);
            completed.fetch_or(1u << index, std::memory_order_acq_rel);
        });
    };
    auto a = make(0, 0), b = make(1, 1), c = make(2, 1), d = make(3, 1);
    auto e = make(4, 1u << 3), f = make(5, 1), g = make(6, 1);
    auto h = make(7, 1u << 6), i = make(8, 1u << 7);
    auto j = make(9, 511), k = make(10, 1u << 9), l = make(11, 511);
    auto graph = a >> (b | c | ((d >> e) | f | (g >> h >> i))) >> ((j >> k) | l);
    EXPECT_EQ(flow.enqueue(graph).wait(), 0);
    EXPECT_EQ(completed.load(std::memory_order_relaxed), 4095u);

    std::atomic<int> stage{0};
    auto             first  = flow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 0); });
    auto             second = flow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 1); });
    auto             third  = flow.create([&] { EXPECT_EQ(stage.fetch_add(1, std::memory_order_relaxed), 2); });
    (void)(first >> second >> third);
    // Listing existing nodes adds a join, without replacing their local dependency edges.
    EXPECT_EQ(flow.enqueue(au::flow::Tasks{first, second, third}).wait(), 0);
    EXPECT_EQ(stage.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, ParallelAcceptedPredecessorsAndBranchErrors)
{
    auto& flow = au::flow::Flow::get();
    auto  done = flow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    std::atomic<bool> entered{false}, release{false};
    auto              running = flow.enqueue([&] {
        entered.store(true, std::memory_order_release);
        return until([&] { return release.load(std::memory_order_acquire); }) ? 0 : 1;
    });
    EXPECT_TRUE(until([&] { return entered.load(std::memory_order_acquire); }));
    std::atomic<int> count{0};
    auto             pending    = flow.enqueueAfter(running, [&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto             consumer   = flow.create([&] { EXPECT_EQ(count.load(std::memory_order_relaxed), 1); });
    auto             branches   = done | running | pending;
    auto             completion = flow.enqueue(branches >> consumer);
    EXPECT_FALSE(completion.done());
    auto incoming = flow.create([] {});
    auto rejected = flow.enqueue(incoming >> branches);
    EXPECT_EQ(rejected.wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(rejected.code(), au::err::kErrorStateInvalid);
    expectCreated({incoming});
    EXPECT_EQ(running.code(), au::err::kErrorNotReady);
    release.store(true, std::memory_order_release);
    EXPECT_EQ(completion.wait(), 0);
    EXPECT_EQ(pending.wait(), 0);

    auto failed = flow.enqueue([] { return -123; });
    ASSERT_EQ(failed.wait(), -123);
    auto sibling = flow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    auto skipped = flow.create([&] { ADD_FAILURE() << "Failed join must skip its successor"; });
    EXPECT_EQ(flow.enqueue((failed | sibling) >> skipped).wait(), -123);
    EXPECT_EQ(sibling.wait(), 0);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 2);

    auto notReadyResult = flow.enqueue([] { return au::err::kErrorNotReady; });
    ASSERT_EQ(notReadyResult.wait(), au::err::kErrorNotReady);
    auto independent = flow.create([&] { count.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(flow.enqueue(notReadyResult | independent).wait(), au::err::kErrorNotReady);
    EXPECT_EQ(independent.wait(), 0);  // A completed business error is not an expression build error.
    EXPECT_EQ(count.load(std::memory_order_relaxed), 3);
}

TEST_F(XFlow, FlatRangesSharingAndDiscardedExpressions)
{
    auto&                       flow = au::flow::Flow::get();
    std::atomic<int>            calls{0};
    std::vector<au::flow::Task> tasks;
    for (int i = 0; i < 64; ++i)
        tasks.push_back(flow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); }));

    // Copies share a cursor: attempting a preliminary traversal would consume this input range.
    struct Input
    {
        const std::vector<au::flow::Task>* mTasks;
        std::size_t*                       mPosition;
        bool                               mEnd;

        const au::flow::Task& operator*() const { return (*mTasks)[*mPosition]; }

        Input& operator++()
        {
            ++*mPosition;
            return *this;
        }

        bool operator!=(const Input&) const { return !mEnd && *mPosition < mTasks->size(); }
    };

    std::size_t position = 0;
    auto        flat     = au::flow::Tasks::parallel(Input{&tasks, &position, false}, Input{&tasks, &position, true});
    EXPECT_EQ(position, tasks.size());
    EXPECT_EQ(flow.enqueue(flat).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 64);
    EXPECT_EQ(flow.enqueue(flat).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 64);
    EXPECT_EQ(flow.enqueue(au::flow::Tasks::parallel(tasks.begin(), tasks.begin())).wait(), 0);
    auto single = flow.enqueue(au::flow::Tasks::parallel(tasks.begin(), tasks.begin() + 1));
    EXPECT_EQ(single.nativeHandle(), tasks[0].nativeHandle());
    EXPECT_EQ(single.wait(), 0);

    std::atomic<unsigned> flags{0};
    auto                  a      = flow.create([&] { flags.fetch_or(1, std::memory_order_relaxed); });
    auto                  b      = flow.create([&] { flags.fetch_or(2, std::memory_order_relaxed); });
    auto                  left   = flow.create([&] { EXPECT_EQ(flags.load(std::memory_order_relaxed), 3u); });
    auto                  right  = flow.create([&] { EXPECT_EQ(flags.load(std::memory_order_relaxed), 3u); });
    auto                  shared = left | right;
    auto                  first  = a >> shared;
    auto                  second = b >> shared;  // Shared nodes accumulate both prefixes.
    EXPECT_EQ(flow.enqueue(first).wait(), 0);
    EXPECT_EQ(flow.enqueue(second).wait(), 0);
    EXPECT_TRUE(b.done());

    auto abandoned = flow.create([] {}), untouched = flow.create([] {});
    {
        auto discarded = abandoned | untouched;
    }
    EXPECT_EQ(flow.enqueue(abandoned).wait(), 0);
    expectCreated({untouched});  // A discarded parallel join did not add an entry before either branch.

    auto head = flow.create([] {}), tail = flow.create([] {});
    {
        auto discarded = head >> tail;
    }
    EXPECT_EQ(flow.enqueue(tail).wait(), 0);
    EXPECT_TRUE(head.done());  // Eager serial edges survive destruction of their expression.

    auto                         duplicate  = flow.create([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    auto                         serialTail = flow.create([] {});
    std::vector<au::flow::Tasks> fragments{duplicate >> serialTail, duplicate, au::flow::Tasks{}};
    EXPECT_EQ(flow.enqueue(au::flow::Tasks::parallel(fragments.begin(), fragments.end())).wait(), 0);
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 65);
}

TEST_F(XFlow, ExpressionCreationAndSubmissionErrors)
{
    auto&                    flow        = au::flow::Flow::get();
    int                      destructors = 0;
    ThrowingPayload<1, true> payload(destructors);
    auto                     failed    = flow.create(payload);
    auto                     untouched = flow.create([] {});
    au::flow::Tasks          empty;
    EXPECT_EQ(flow.enqueue(untouched >> failed).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(flow.enqueue(empty | failed).wait(), au::err::kErrorNoMemory);
    EXPECT_EQ(flow.enqueue(failed >> empty).code(), au::err::kErrorNoMemory);
    EXPECT_EQ(flow.enqueueAfter(failed | untouched, [] {}).wait(), au::err::kErrorNoMemory);
    expectCreated({untouched});

    void*          storage = nullptr;
    au::flow::Task raw(au_flow_create_task_ex(nullptr, 1, 1, [](void*) { return 0; }, nullptr, &storage));
    ASSERT_TRUE(raw.valid());
    auto rejected = flow.enqueue(raw);
    EXPECT_FALSE(rejected.valid());
    EXPECT_EQ(rejected.nativeHandle(), nullptr);
    EXPECT_TRUE(rejected.done());
    EXPECT_EQ(rejected.code(), au::err::kErrorNotReady);
    EXPECT_FALSE(raw.done());
    ::new (storage) unsigned char(0);
    ASSERT_EQ(au_flow_commit_task_payload(raw.nativeHandle()), 0);
    EXPECT_EQ(flow.enqueue(raw).wait(), 0);
    EXPECT_EQ(raw.code(), 0);  // The submission error belonged to the returned handle, not this original.
    EXPECT_EQ(rejected.wait(), au::err::kErrorNotReady);
    EXPECT_EQ(flow.enqueue(rejected | untouched).wait(), au::err::kErrorNotReady);
    expectCreated({untouched});

    auto head = flow.create([] {}), middle = flow.create([] {});
    auto done = flow.enqueue([] {});
    ASSERT_EQ(done.wait(), 0);
    EXPECT_EQ(flow.enqueue(head >> middle >> done).wait(), au::err::kErrorStateInvalid);
    expectCreated({head, middle});
    EXPECT_EQ(flow.enqueue(middle).wait(), 0);  // Successful eager edges survived the later rejected edge.
    EXPECT_TRUE(head.done());

    struct ThrowingInput
    {
        bool mNoMemory;

        const au::flow::Task& operator*() const
        {
            if (mNoMemory)
                throw std::bad_alloc();
            throw std::runtime_error("input failed");
        }

        ThrowingInput& operator++() { return *this; }

        bool operator!=(const ThrowingInput&) const { return true; }
    };

    EXPECT_EQ(flow.enqueue(au::flow::Tasks::parallel(ThrowingInput{true}, ThrowingInput{true})).wait(),
              au::err::kErrorNoMemory);
    EXPECT_EQ(flow.enqueue(au::flow::Tasks::parallel(ThrowingInput{false}, ThrowingInput{false})).wait(),
              au::err::kErrorUnknown);
}

TEST_F(XFlow, ErrorShortCircuiting)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> ran{0};
    auto             a     = flow.create([]() -> int { return -123; });
    auto             b     = flow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    auto             c     = flow.create([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    auto             graph = a >> b >> c;
    EXPECT_EQ(flow.enqueue(graph).wait(), -123);
    EXPECT_EQ(c.wait(), -123);
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(flow.enqueueAfter(a, [&] { ++ran; }).wait(), -123);
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 0);
}

TEST_F(XFlow, CycleDetectionAndDiscardedDraftLifetime)
{
    auto&              flow = au::flow::Flow::get();
    std::weak_ptr<int> observed;
    {
        auto owner        = std::make_shared<int>(1);
        observed          = owner;
        auto            a = flow.create([owner] {}), b = flow.create([] {});
        auto            cycle = a >> b >> a;
        au::flow::Tasks group{a, b};
        EXPECT_EQ(flow.enqueue(group).wait(), au::err::kErrorFlowCycleDetected);
        EXPECT_EQ(flow.enqueue(cycle).wait(), au::err::kErrorFlowCycleDetected);
        EXPECT_EQ(a.code(), au::err::kErrorFlowCycleDetected);
    }

    EXPECT_TRUE(observed.expired());
    auto self = flow.create([] {});
    EXPECT_EQ(self.precede(self), au::err::kErrorFlowCycleDetected);
    EXPECT_EQ(flow.enqueue(self).wait(), au::err::kErrorFlowCycleDetected);
    auto target = flow.enqueue([] {});
    EXPECT_EQ(target.wait(), 0);
    EXPECT_EQ(self.precede(target), au::err::kErrorStateInvalid);
    EXPECT_EQ(target.code(), 0);
    auto pred = flow.create([] {}), successor = flow.create([] {});
    EXPECT_EQ(pred.precede(successor), 0);
    EXPECT_EQ(flow.enqueue(successor).wait(), 0);  // Submit the unsubmitted predecessor through the successor.
    EXPECT_EQ(successor.wait(), 0);
    EXPECT_TRUE(pred.done());
}

TEST_F(XFlow, CooperativeWaitAndWaitCycles)
{
    auto& flow  = au::flow::Flow::get();
    auto  outer = flow.enqueue([&] {
        auto child = flow.enqueue([]() -> int { return 23; });
        return child.wait();
    });
    EXPECT_EQ(outer.wait(), 23);
    EXPECT_EQ(flow.enqueue([&] { return flow.waitAll(); }).wait(), au::err::kErrorFlowWouldDeadlock);
    std::atomic<bool> go{false};
    au::flow::Task    a, b;
    a = flow.create([&] { return until([&] { return go.load(std::memory_order_acquire); }) ? b.wait() : 1; });
    b = flow.create([&] { return until([&] { return go.load(std::memory_order_acquire); }) ? a.wait() : 1; });
    auto completionA = flow.enqueue(a);
    auto completionB = flow.enqueue(b);
    go.store(true, std::memory_order_release);
    EXPECT_EQ(a.wait(), au::err::kErrorFlowWouldDeadlock);
    EXPECT_EQ(b.wait(), au::err::kErrorFlowWouldDeadlock);
}

TEST_F(XFlow, TiledParallelismAndNestedSafety)
{
    auto&            flow = au::flow::Flow::get();
    std::atomic<int> active{0}, maximum{0};
    for (size_t range : {size_t(0), size_t(3), size_t(257)}) {
        std::array<std::atomic<int>, 257> seen{};
        flow.parallelizeTiledTasks(range, 7, [&](size_t begin, size_t end) {
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
    XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(values.size(), 6) values[i] = static_cast<int>(i);
    XTHREAD_PARALLELIZE_END
    int* data = values.data();
    XTHREAD_PARALLELIZE_TILED_TASK_ASSIGN(values.size(), 8) data[i] += 1;
    XTHREAD_PARALLELIZE_END
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(values[i], static_cast<int>(i) + 1);
    std::atomic<int>              arrived{0};
    std::atomic<size_t>           total{0};
    std::array<au::flow::Task, 8> tasks;
    for (auto& task : tasks)
        task = flow.enqueue([&]() -> int {
            arrived.fetch_add(1, std::memory_order_release);
            if (!until([&] { return arrived.load(std::memory_order_acquire) == 8; }))
                return 1;
            flow.parallelizeTiledTasks(127, 11, [&](size_t b, size_t e) {
                flow.parallelizeTiledTasks(16, 3, [&](size_t ib, size_t ie) {
                    total.fetch_add((e - b) * (ie - ib), std::memory_order_relaxed);
                });
            });
            return 0;
        });
    for (auto& task : tasks)
        EXPECT_EQ(task.wait(), 0);
    EXPECT_EQ(total.load(std::memory_order_relaxed), 8u * 127 * 16);
    EXPECT_LE(flow.workerCount(), 8u);
    size_t end = 0;
    flow.parallelizeTiledTasks(SIZE_MAX, SIZE_MAX, [&](size_t b, size_t e) {
        EXPECT_EQ(b, 0u);
        end = e;
    });
    EXPECT_EQ(end, SIZE_MAX);
}

TEST_F(XFlow, CpuAffinity)
{
    auto&       flow   = au::flow::Flow::get();
    std::size_t count  = 0;
    const int   status = au::sys::getThreadAffinity(nullptr, 0, count);
    if (status == au::err::kErrorNotSupported) {
        EXPECT_EQ(flow.setAffinity(), au::err::kErrorNotSupported);
        return;
    }
    ASSERT_EQ(status, 0);
    ASSERT_EQ(flow.setAffinity(), 0);
    const uint64_t allowed = flow.getAffinityMask();
    if (allowed) {
        const auto selected = allowed & (~allowed + 1);
        ASSERT_EQ(flow.setAffinity(selected), 0);
        EXPECT_EQ(flow.getAffinityMask(), selected);
        std::atomic<uint64_t> actual{0};
        auto                  worker = flow.enqueue([&] {
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
        EXPECT_TRUE(until([&] { return worker.done(); }));
        EXPECT_EQ(worker.wait(), 0);
        EXPECT_EQ(actual.load(std::memory_order_acquire), selected);
        std::vector<uint32_t> before(count), after(count);
        ASSERT_EQ(au::sys::getThreadAffinity(before.data(), before.size(), count), 0);
        flow.parallelizeTiledTasks(1, 1, [](size_t, size_t) {});
        ASSERT_EQ(au::sys::getThreadAffinity(after.data(), after.size(), count), 0);
        EXPECT_EQ(before, after);
        for (auto type : {au::sys::CpuClass::kPerformance, au::sys::CpuClass::kPowerSaving}) {
            uint64_t mask = 0;
            ASSERT_EQ(au::sys::selectCpuMask(type, mask), 0);
            EXPECT_NE(mask, 0u);
            EXPECT_EQ(mask & ~allowed, 0u);
            EXPECT_EQ(flow.setAffinity(mask), 0);
        }
        if (~allowed) {
            const auto outside = ~allowed & (allowed + 1);
            EXPECT_EQ(flow.setAffinity(outside), au::err::kErrorInvalidParam);
        }
    }
    EXPECT_EQ(flow.setAffinity(), 0);
}
#endif
