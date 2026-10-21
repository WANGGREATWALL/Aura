#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

#include "cv/ximage_access.h"
#include "flow/completion.h"
#include "flow/testing.h"
#include "flow/xflow.h"
#include "gpu/xopencl.h"

static_assert(AURA_FLOW_ABI_VERSION == 2u, "flow C ABI version must remain 2");
static_assert(std::is_standard_layout<au::flow::TaskResult>::value, "result must retain standard layout");
static_assert(std::is_standard_layout<au::flow::WorkerInfo>::value, "worker must retain standard layout");
static_assert(sizeof(au::flow::TaskResult) == sizeof(au_flow_result), "C/C++ result size");
static_assert(alignof(au::flow::TaskResult) == alignof(au_flow_result), "C/C++ result alignment");
static_assert(sizeof(au::flow::WorkerInfo) == sizeof(au_flow_worker), "C/C++ worker size");
static_assert(alignof(au::flow::WorkerInfo) == alignof(au_flow_worker), "C/C++ worker alignment");
static_assert(offsetof(au::flow::TaskResult, mStructSize) == offsetof(au_flow_result, mStructSize),
              "TaskResult::mStructSize offset");
static_assert(offsetof(au::flow::TaskResult, mState) == offsetof(au_flow_result, mState), "TaskResult::mState offset");
static_assert(offsetof(au::flow::TaskResult, mDomain) == offsetof(au_flow_result, mDomain),
              "TaskResult::mDomain offset");
static_assert(offsetof(au::flow::TaskResult, mCode) == offsetof(au_flow_result, mCode), "TaskResult::mCode offset");
static_assert(offsetof(au::flow::TaskResult, mNativeCode) == offsetof(au_flow_result, mNativeCode),
              "TaskResult::mNativeCode offset");
static_assert(offsetof(au::flow::TaskResult, mCauseTaskId) == offsetof(au_flow_result, mCauseTaskId),
              "TaskResult::mCauseTaskId offset");
static_assert(offsetof(au::flow::WorkerInfo, mStructSize) == offsetof(au_flow_worker, mStructSize),
              "WorkerInfo::mStructSize offset");
static_assert(offsetof(au::flow::WorkerInfo, mWorkerId) == offsetof(au_flow_worker, mWorkerId),
              "WorkerInfo::mWorkerId offset");
static_assert(offsetof(au::flow::WorkerInfo, mThreadId) == offsetof(au_flow_worker, mThreadId),
              "WorkerInfo::mThreadId offset");
static_assert(offsetof(au::flow::WorkerInfo, mRequestedVersion) == offsetof(au_flow_worker, mRequestedVersion),
              "WorkerInfo::mRequestedVersion offset");
static_assert(offsetof(au::flow::WorkerInfo, mAppliedVersion) == offsetof(au_flow_worker, mAppliedVersion),
              "WorkerInfo::mAppliedVersion offset");
static_assert(offsetof(au::flow::WorkerInfo, mAffinityCode) == offsetof(au_flow_worker, mAffinityCode),
              "WorkerInfo::mAffinityCode offset");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Invalid) == AU_FLOW_INVALID, "TaskState::Invalid value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Draft) == AU_FLOW_DRAFT, "TaskState::Draft value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Pending) == AU_FLOW_PENDING, "TaskState::Pending value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Ready) == AU_FLOW_READY, "TaskState::Ready value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Running) == AU_FLOW_RUNNING, "TaskState::Running value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Waiting) == AU_FLOW_WAITING, "TaskState::Waiting value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Succeeded) == AU_FLOW_SUCCEEDED,
              "TaskState::Succeeded value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Failed) == AU_FLOW_FAILED, "TaskState::Failed value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Skipped) == AU_FLOW_SKIPPED, "TaskState::Skipped value");
static_assert(static_cast<unsigned int>(au::flow::TaskState::Rejected) == AU_FLOW_REJECTED,
              "TaskState::Rejected value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::None) == AU_FLOW_DOMAIN_NONE, "ErrorDomain::None value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::Flow) == AU_FLOW_DOMAIN_FLOW, "ErrorDomain::Flow value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::Callable) == AU_FLOW_DOMAIN_CALLABLE,
              "ErrorDomain::Callable value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::Memory) == AU_FLOW_DOMAIN_MEMORY,
              "ErrorDomain::Memory value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::OpenCL) == AU_FLOW_DOMAIN_OPENCL,
              "ErrorDomain::OpenCL value");
static_assert(static_cast<unsigned int>(au::flow::ErrorDomain::System) == AU_FLOW_DOMAIN_SYSTEM,
              "ErrorDomain::System value");

namespace {
int ordinary(int& result)
{
    result = 11;
    return 0;
}
struct Module
{
    int member(int& result) const
    {
        result = 12;
        return 0;
    }
};
struct Tiles
{
    std::atomic<std::size_t> mTotal{0};
    int                      operator()(std::size_t, std::size_t count) &
    {
        mTotal.fetch_add(count, std::memory_order_relaxed);
        return 0;
    }
};
int ordinaryTile(std::size_t, std::size_t count) { return count ? 0 : 1; }
}  // namespace
int main(int argc, char**)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    if (::au_gpu_opencl_version() != AURA_GPU_OPENCL_ABI_VERSION)
        return 10;
    const au::gpu::Launch launch = au::gpu::CLWrapper::get().setNDRange({32, 16}, {8, 4});
    (void)launch;
    unsigned char           storage[128] = {};
    au::mm::ExternalMemory  registration;
    ::au_mm_external_memory memory = {};
    memory.mStructSize             = sizeof(memory);
    memory.mData                   = storage;
    memory.mSize                   = sizeof(storage);
    memory.mFd                     = -1;
    if (registration.registerMemory(memory))
        return 9;
    au::mm::CpuAccessScope access;
    if (access.begin({au::mm::writeAccess(storage, sizeof(storage))}) || access.finish())
        return 9;
    if (argc > 1) {
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, 0);
        int covered = 0;
        if (flow.parallelizeTiledTasks(19, 1, [&](std::size_t, std::size_t n) { covered += static_cast<int>(n); }) ||
            covered != 19)
            return 11;  // Caller still completes when the first helper cannot start.
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, -1);
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, 0);
        au::flow::Task rejected = flow.enqueue([]() -> int { return 0; });
        if (rejected.submissionCode() != au::err::kErrorPlatformAPI)
            return 4;
        au::flow::TaskResult error{};
        if (rejected.result(error) || error.mDomain != au::flow::ErrorDomain::System)
            return 5;
    }
    int                           finalized = 0;
    au::flow::detail::ExternalOps ops       = {};
    ops.mRelease                            = [](void* value) noexcept { *static_cast<int*>(value) = 1; };
    if (flow.enqueue([&]() -> int {
                au::flow::detail::ExternalToken* token = nullptr;
                const int                        ret   = au::flow::detail::prepareExternal(ops, &finalized, &token);
                if (ret || !token)
                    return ret ? ret : 1;
                au::flow::detail::notifyExternal(token, {});
                au::flow::detail::leaveExternalRegistrar(token);
                return 0;
            })
            .wait() ||
        finalized != 1)
        return 13;  // First managed task must have asynchronous completion progress.
    int                  result = 0;
    au::flow::TaskResult status{};
    au::flow::Task       a = flow.enqueue(ordinary, std::ref(result));
    if (flow.wait(a, &status) || status.mCode || result != 11)
        return 1;
    const Module   module{};
    au::flow::Task b = flow.enqueue(&Module::member, std::cref(module), std::ref(result));
    if (flow.wait(b, &status) || status.mCode || result != 12)
        return 2;
    std::unique_ptr<int> owned(new int(13));
    au::flow::Task       c =
        flow.enqueue([](std::unique_ptr<int> value) -> int { return *value == 13 ? 0 : 3; }, std::move(owned));
    if (c.wait(&status) || !c.ready() || c.code() || c.waitFor(0) || status.mCode)
        return 3;
    au::flow::Tasks group{flow.makeTask([]() -> int { return 17; })};
    if (flow.enqueue(group).submissionCode() || group.wait() != 17 || !group.ready() || group.code() != 17 ||
        group.waitFor(0) != 17)
        return 12;
    if (flow.wait(group, &status) != 17 || flow.waitFor(group, 0) != 17 || status.mCode != 17)
        return 14;
    result = 0;
    if (flow.enqueueAfter(c, &Module::member, std::cref(module), std::ref(result)).wait() || result != 12)
        return 15;
    au::flow::Task pair = flow.enqueue(
        au::flow::Tasks{flow.makeTask([]() -> int { return 0; }), flow.makeTask([]() -> int { return 0; })});
    if (flow.enqueueAfter({a, b, c, pair}, &Module::member, std::cref(module), std::ref(result)).wait())
        return 16;
    if (flow.enqueueAfter({}, []() -> int { return 0; }).wait() || result != 12)
        return 17;
    Tiles tiles;  // Atomic state makes this noncopyable in C++11.
    if (flow.parallelizeTiledTasks(31, 3, tiles) || tiles.mTotal.load(std::memory_order_relaxed) != 31)
        return 18;
    if (flow.parallelizeTiledTasks(31, 3, ordinaryTile))
        return 19;
    au::flow::Tasks emptyGroup;
    if (flow.enqueue(emptyGroup).wait())
        return 20;
    if (flow.shutdown())
        return 6;
    if (c.result(status) || status.mCode)
        return 7;
    if (flow.enqueue(ordinary, std::ref(result)).submissionCode() != au::err::kErrorStateInvalid)
        return 8;
    return 0;
}
