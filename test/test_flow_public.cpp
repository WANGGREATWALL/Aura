#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <utility>

#include "cv/ximage_access.h"
#include "flow/testing.h"
#include "flow/xflow.h"
#include "gtest/gtest.h"

namespace {
void expectQuery(const au::flow::Task& task, au::flow::TaskState state, bool valid, bool ready, int submission,
                 int resultCode, int waitCode)
{
    EXPECT_EQ(task.valid(), valid);
    EXPECT_EQ(task.state(), state);
    EXPECT_EQ(task.ready(), ready);
    EXPECT_EQ(task.submissionCode(), submission);
    EXPECT_EQ(task.code(), resultCode);
    au::flow::TaskResult result{};
    result.mCode    = 12345;
    const int query = task.result(result);
    EXPECT_EQ(query, ready ? 0 : resultCode);
    EXPECT_EQ(result.mCode, ready ? resultCode : 12345);
    result.mCode = 12345;
    EXPECT_EQ(task.waitFor(0, &result), waitCode);
    EXPECT_EQ(result.mCode, ready ? resultCode : 12345);
}
}  // namespace

TEST(XFlowPublic, QueryMatrixSeparatesAcceptanceCompletionAndFailure)
{
    auto&          flow = au::flow::XFlow::get();
    au::flow::Task empty;
    expectQuery(empty, au::flow::TaskState::Invalid, false, false, au::err::kErrorInvalidHandle,
                au::err::kErrorInvalidHandle, au::err::kErrorInvalidHandle);
    auto draft = flow.makeTask([]() -> int { return 0; });
    expectQuery(draft, au::flow::TaskState::Draft, true, false, au::err::kErrorNotReady, au::err::kErrorNotReady,
                au::err::kErrorNotReady);
    auto moved = std::move(draft);
    EXPECT_EQ(draft.state(), au::flow::TaskState::Invalid);
    EXPECT_FALSE(draft.valid());
    EXPECT_EQ(flow.enqueue(moved).wait(), 0);
    expectQuery(moved, au::flow::TaskState::Succeeded, true, true, 0, 0, 0);

    std::promise<void> release;
    auto               gate    = release.get_future().share();
    auto               before  = flow.enqueue([gate]() -> int {
        gate.wait();
        return 17;
    });
    auto               pending = flow.enqueueAfter(before, []() -> int { return 0; });
    expectQuery(pending, au::flow::TaskState::Pending, true, false, 0, au::err::kErrorNotReady, au::err::kErrorTimeout);
    release.set_value();
    EXPECT_EQ(pending.wait(), 17);
    expectQuery(before, au::flow::TaskState::Failed, true, true, 0, 17, 17);
    expectQuery(pending, au::flow::TaskState::Skipped, true, true, 0, 17, 17);
    au::flow::TaskResult result{};
    EXPECT_EQ(pending.result(result), 0);
    EXPECT_EQ(result.mCauseTaskId, before.id());
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);

    auto cyclic = flow.makeTask([]() -> int { return 0; });
    EXPECT_EQ(cyclic.precede(cyclic), au::flow::kErrorCycleDetected);
    EXPECT_EQ(flow.enqueue(cyclic).submissionCode(), au::flow::kErrorCycleDetected);
    expectQuery(cyclic, au::flow::TaskState::Rejected, true, true, au::flow::kErrorCycleDetected,
                au::flow::kErrorCycleDetected, au::flow::kErrorCycleDetected);
    EXPECT_NE(cyclic.id(), 0u);
    au::flow::testing::failAfter(au::flow::testing::Failure::Node, 0);
    auto rejected = flow.makeTask([]() -> int { return 0; });
    expectQuery(rejected, au::flow::TaskState::Rejected, true, true, au::err::kErrorNoMemory, au::err::kErrorNoMemory,
                au::err::kErrorNoMemory);
    EXPECT_EQ(rejected.id(), 0u);  // Allocation-free rejection has no node identity.
}

TEST(XFlowPublic, BuilderFirstErrorAndAcceptedTargetsAreIndependent)
{
    auto&          flow = au::flow::XFlow::get();
    auto           a    = flow.makeTask([]() -> int { return 0; });
    auto           b    = flow.makeTask([]() -> int { return 0; });
    au::flow::Task invalid;
    EXPECT_EQ(a.precede(b), 0);
    EXPECT_EQ(b.precede(a), au::flow::kErrorCycleDetected);
    EXPECT_EQ(invalid.precede(a), au::err::kErrorInvalidHandle);
    au::flow::Tasks graph{a, b};
    EXPECT_EQ(flow.enqueue(graph).wait(), au::flow::kErrorCycleDetected);  // First error wins.

    auto untouched = flow.makeTask([]() -> int { return 0; });
    auto accepted  = flow.enqueue([]() -> int { return 29; });
    EXPECT_EQ(accepted.wait(), 29);
    EXPECT_EQ(untouched.precede(accepted), au::err::kErrorStateInvalid);
    EXPECT_EQ(invalid.precede(accepted), au::err::kErrorInvalidHandle);
    EXPECT_EQ(accepted.submissionCode(), 0);
    EXPECT_EQ(accepted.code(), 29);
    EXPECT_EQ(untouched.state(), au::flow::TaskState::Draft);
    EXPECT_EQ(flow.enqueue(untouched).wait(), 0);

    std::promise<void> release;
    auto               gate    = release.get_future().share();
    auto               blocked = flow.enqueue([gate]() -> int {
        gate.wait();
        return 0;
    });
    auto               target  = flow.enqueueAfter(blocked, []() -> int { return 0; });
    auto               late    = flow.makeTask([]() -> int { return 0; });
    EXPECT_EQ(late.precede(target), au::err::kErrorStateInvalid);
    EXPECT_EQ(target.state(), au::flow::TaskState::Pending);
    release.set_value();
    EXPECT_EQ(target.wait(), 0);  // Original accepted dependency is preserved.
    EXPECT_EQ(flow.enqueue(late).wait(), 0);
    EXPECT_EQ(flow.enqueueAfter(flow.enqueue([]() -> int { return 0; }), []() -> int { return 0; }).wait(), 0);
    auto rejected = flow.enqueueAfter({invalid, accepted}, []() -> int { return 0; });
    EXPECT_EQ(rejected.wait(), au::err::kErrorInvalidHandle);
    EXPECT_EQ(accepted.code(), 29);
}

TEST(XFlowPublic, FlowErrorsKeepNumbersNamesAndCallableProvenance)
{
    const std::array<int, 4>         codes{{au::flow::kErrorDependencyFailed, au::flow::kErrorCycleDetected,
                                            au::flow::kErrorWouldDeadlock, au::flow::kErrorUnhandledException}};
    const std::array<const char*, 4> names{
        {"ErrorDependencyFailed", "ErrorCycleDetected", "ErrorWouldDeadlock", "ErrorUnhandledException"}};
    auto& flow = au::flow::XFlow::get();
    for (std::size_t i = 0; i < codes.size(); ++i) {
        EXPECT_EQ(codes[i], -5001 - static_cast<int>(i));
        EXPECT_STREQ(au::flow::getErrorStr(codes[i]), names[i]);
        auto                 business = flow.enqueue([&, i]() -> int { return codes[i]; });
        au::flow::TaskResult result{};
        EXPECT_EQ(business.wait(&result), codes[i]);
        EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Callable);
        EXPECT_EQ(result.mNativeCode, 0);
        EXPECT_EQ(result.mCauseTaskId, business.id());
    }
    EXPECT_STREQ(au::flow::getErrorStr(au::err::kErrorNoMemory), au::err::getErrorStr(au::err::kErrorNoMemory));
    EXPECT_STREQ(au::flow::getErrorStr(12345), "UnknownErrorCode");
    EXPECT_STREQ(au::flow::getErrorStr(-5999), "UnknownErrorCode");
    EXPECT_FALSE(au::err::isError(17));  // Historical helper; flow fails on every nonzero int.
    EXPECT_EQ(flow.enqueue([]() -> int { return 17; }).wait(), 17);
}

TEST(XFlowPublic, OwningReferencesPinLiveNodesDuringPoolChurn)
{
    au_flow_handle handle    = nullptr;
    int            destroyed = 0;
    ASSERT_EQ(au_flow_make([](void*) -> int { return 31; }, [](void* value) { ++*static_cast<int*>(value); },
                           &destroyed, &handle),
              0);
    const auto id = au_flow_id(handle);
    au_flow_retain(handle);
    au_flow_release(handle);  // The retained reference, not the old pointer variable, pins the identity.
    for (int i = 0; i < 512; ++i) {
        au_flow_handle other = nullptr;
        ASSERT_EQ(au_flow_make([](void*) -> int { return 0; }, nullptr, nullptr, &other), 0);
        EXPECT_NE(other, handle);
        EXPECT_NE(au_flow_id(other), id);
        au_flow_release(other);
    }
    EXPECT_EQ(au_flow_id(handle), id);
    EXPECT_EQ(destroyed, 0);
    ASSERT_EQ(au_flow_submit(handle), 0);
    au_flow_result result{};
    result.mStructSize = sizeof(result);
    EXPECT_EQ(au_flow_wait(handle, UINT64_MAX, &result), 0);  // C wait status, not task code.
    EXPECT_EQ(result.mCode, 31);
    EXPECT_EQ(destroyed, 1);
    au_flow_release(handle);
    // Do not query handle after final release: dangling pointers are not validated inputs.

    auto&      flow   = au::flow::XFlow::get();
    auto       owner  = flow.makeTask([]() -> int { return 0; });
    auto       copy   = owner;
    const auto taskId = copy.id();
    owner             = {};
    auto moved        = std::move(copy);
    EXPECT_FALSE(copy.valid());
    EXPECT_EQ(moved.id(), taskId);
    EXPECT_EQ(flow.enqueue(moved).wait(), 0);
}

TEST(XFlowPublic, BorrowedImageMovesAndOwningPayloadLifetimeAreExplicit)
{
    auto&          flow = au::flow::XFlow::get();
    au::cv::XImage owner(8, 4, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    ASSERT_TRUE(owner.isValid());
    auto*          storage       = owner.view().data[0];
    au::cv::XImage borrowed      = owner;
    au::cv::XImage movedBorrowed = std::move(borrowed);
    EXPECT_FALSE(movedBorrowed.ownsMemory());
    EXPECT_TRUE(owner.ownsMemory());
    auto read = flow.enqueue(
        [](au::cv::XImage image) -> int {
            if (image.ownsMemory())
                return 1;
            au::mm::CpuAccessScope access;
            const int              ret = access.begin({au::cv::readAccess(image)});
            return ret ? ret : access.finish();
        },
        std::move(movedBorrowed));
    EXPECT_EQ(read.wait(), 0);  // owner is kept alive until the borrowed task completes.
    EXPECT_TRUE(au::mm::isManaged(storage));

    std::promise<void> release;
    auto               gate   = release.get_future().share();
    auto               before = flow.enqueue([gate]() -> int {
        gate.wait();
        return 0;
    });
    auto               task   = flow.enqueueAfter(
        before,
        [](au::cv::XImage image) -> int {
            if (!image.ownsMemory())
                return 2;
            au::mm::CpuAccessScope access;
            const int              ret = access.begin({au::cv::writeAccess(image)});
            if (ret)
                return ret;
            image.view().data[0][0] = 0x7a;
            return access.finish();
        },
        std::move(owner));
    EXPECT_FALSE(owner.isValid());
    EXPECT_TRUE(au::mm::isManaged(storage));
    task = {};  // Dropping the task reference does not cancel or destroy accepted payloads.
    release.set_value();
    EXPECT_EQ(flow.waitAll(), 0);
    EXPECT_FALSE(au::mm::isManaged(storage));

    au::cv::XImage abandoned(8, 4, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    ASSERT_TRUE(abandoned.isValid());
    storage    = abandoned.view().data[0];
    auto draft = flow.makeTask([](au::cv::XImage) -> int { return 0; }, std::move(abandoned));
    EXPECT_TRUE(au::mm::isManaged(storage));
    draft = {};  // An unsubmitted owning payload is also destroyed exactly once.
    EXPECT_FALSE(au::mm::isManaged(storage));
}
