#include "flow/xflow.h"
#include "flow/testing.h"
#include "flow/completion.h"
#include <memory>
#include "cv/ximage_access.h"
#include "gpu/xopencl.h"

namespace {
int ordinary(int& result) { result = 11; return 0; }
struct Module {
    int member(int& result) const { result = 12; return 0; }
};
}
int main(int argc, char**)
{
    au::flow::XFlow& flow = au::flow::XFlow::get();
    if (::au_gpu_opencl_version() != AURA_GPU_OPENCL_ABI_VERSION) return 10;
    const au::gpu::Launch launch = au::gpu::CLWrapper::get().setNDRange({32, 16}, {8, 4});
    (void)launch;
    unsigned char storage[128] = {};
    au::mm::ExternalMemory registration;
    ::au_mm_external_memory memory = {};
    memory.mStructSize = sizeof(memory); memory.mData = storage; memory.mSize = sizeof(storage); memory.mFd = -1;
    if (registration.registerMemory(memory)) return 9;
    au::mm::CpuAccessScope access;
    if (access.begin({au::mm::writeAccess(storage, sizeof(storage))}) || access.finish()) return 9;
    if (argc > 1) {
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, 0);
        int covered = 0;
        if (flow.parallelizeTiledTasks(19, 1, [&](std::size_t, std::size_t n) { covered += static_cast<int>(n); }) || covered != 19)
            return 11; // Caller still completes when the first helper cannot start.
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, -1);
        au::flow::testing::failAfter(au::flow::testing::Failure::Worker, 0);
        au::flow::Task rejected = flow.enqueue([]() -> int { return 0; });
        if (rejected.submissionCode() != au::err::kErrorPlatformAPI) return 4;
        au::flow::TaskResult error{};
        if (rejected.result(error) || error.mDomain != au::flow::ErrorDomain::System) return 5;
    }
    int finalized = 0;
    au::flow::detail::ExternalOps ops = {};
    ops.mRelease = [](void* value) noexcept { *static_cast<int*>(value) = 1; };
    if (flow.parallelizeTiledTasks(1, 1, [&](std::size_t, std::size_t) -> int {
        au::flow::detail::ExternalToken* token = nullptr;
        const int ret = au::flow::detail::prepareExternal(ops, &finalized, &token);
        if (ret || !token) return ret ? ret : 1;
        au::flow::detail::notifyExternal(token, {});
        au::flow::detail::leaveExternalRegistrar(token);
        return 0;
    }) || finalized != 1) return 13; // First inline root must have asynchronous completion progress.
    int result = 0;
    au::flow::TaskResult status{};
    au::flow::Task a = flow.enqueue(ordinary, std::ref(result));
    if (flow.wait(a, &status) || status.mCode || result != 11) return 1;
    const Module module{};
    au::flow::Task b = flow.enqueue(&Module::member, std::cref(module), std::ref(result));
    if (flow.wait(b, &status) || status.mCode || result != 12) return 2;
    std::unique_ptr<int> owned(new int(13));
    au::flow::Task c = flow.enqueue([](std::unique_ptr<int> value) -> int { return *value == 13 ? 0 : 3; }, std::move(owned));
    if (c.wait(&status) || !c.ready() || c.code() || c.waitFor(0) || status.mCode) return 3;
    au::flow::Tasks group{flow.makeTask([]() -> int { return 17; })};
    if (flow.enqueue(group).submissionCode() ||
        group.wait() != 17 || !group.ready() || group.code() != 17 || group.waitFor(0) != 17) return 12;
    if (flow.wait(group, &status) != 17 || flow.waitFor(group, 0) != 17 || status.mCode != 17) return 14;
    result = 0;
    if (flow.enqueueAfter(c, &Module::member, std::cref(module), std::ref(result)).wait() || result != 12) return 15;
    if (flow.shutdown()) return 6;
    if (c.result(status) || status.mCode) return 7;
    if (flow.enqueue(ordinary, std::ref(result)).submissionCode() != au::err::kErrorStateInvalid) return 8;
    return 0;
}
