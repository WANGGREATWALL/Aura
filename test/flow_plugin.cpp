#include "flow/xflow.h"
#include "mm/xaccess_api.h"
#include "mm/xaccess.h"
#include "mm/xmemory.h"
#include "flow/completion.h"
#include "gpu/xopencl.h"
#ifdef AURA_GPU_TESTING
#include "gpu/opencl_api.h"
#endif
#include <cstdio>
namespace {
int cppWork() { return au::flow::detail::currentTaskId() ? 0 : au::err::kErrorInvalidHandle; }
}

#if defined(_WIN32)
#define AURA_FLOW_TEST_EXPORT __declspec(dllexport)
#else
#define AURA_FLOW_TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" {
AURA_FLOW_TEST_EXPORT void* flow_test_instance() { return &au::flow::XFlow::get(); }
AURA_FLOW_TEST_EXPORT const void* flow_test_gpu_instance() { return &au::gpu::CLWrapper::get(); }
AURA_FLOW_TEST_EXPORT void* flow_test_gpu_backend()
{
#ifdef AURA_GPU_TESTING
    return au::gpu::detail::backendIdentity(0);
#else
    return nullptr;
#endif
}
AURA_FLOW_TEST_EXPORT int flow_test_make(au_flow_handle* handle)
{
    return au_flow_make([](void*) -> int { return 0; }, nullptr, nullptr, handle);
}
AURA_FLOW_TEST_EXPORT int flow_test_submit(au_flow_handle handle) { return au_flow_submit(handle); }
AURA_FLOW_TEST_EXPORT int flow_test_wait(au_flow_handle handle) { return au_flow_wait(handle, UINT64_MAX, nullptr); }
AURA_FLOW_TEST_EXPORT void flow_test_retain(au_flow_handle handle) { au_flow_retain(handle); }
AURA_FLOW_TEST_EXPORT void flow_test_release(au_flow_handle handle) { au_flow_release(handle); }
AURA_FLOW_TEST_EXPORT int flow_test_shutdown() { return au_flow_shutdown(); }
AURA_FLOW_TEST_EXPORT int flow_test_cpp_run()
{
    au::flow::Task task = au::flow::XFlow::get().enqueue(&cppWork);
    au::flow::TaskResult result{};
    const int wait = au::flow::XFlow::get().wait(task, &result);
    if (wait || result.mCode) std::fprintf(stderr, "cpp facade isolation: submission=%d wait=%d result=%d\n",
        task.submissionCode(), wait, result.mCode);
    return wait ? wait : result.mCode;
}
AURA_FLOW_TEST_EXPORT int flow_test_cpp_memory()
{
    static unsigned char bytes[64];
    ::au_mm_external_memory memory{sizeof(::au_mm_external_memory), bytes, sizeof(bytes), -1, 0, nullptr, nullptr, nullptr};
    au::mm::ExternalMemory registration;
    int code = registration.registerMemory(memory);
    if (code) return code;
    // isManaged is a non-inline module-local query, independent of any weak
    // facade instantiation the dynamic linker might otherwise preempt.
    if (!au::mm::isManaged(bytes)) return au::err::kErrorInvalidHandle;
    au::mm::CpuAccessScope access;
    code = access.begin({au::mm::writeAccess(bytes, sizeof(bytes))});
    if (code) return code;
    code = access.finish();
    if (code) return code;
    au::mm::MemBlock owned;
    code = au::mm::alloc(64, au::mm::MemType::Pss, owned);
    return code ? code : au::mm::free(owned.ptr); // Backend singleton must also disappear cleanly on dlclose.
}
AURA_FLOW_TEST_EXPORT int flow_test_mm_begin(void** scope)
{
    static unsigned char bytes[64];
    ::au_mm_external_memory memory{sizeof(::au_mm_external_memory), bytes, sizeof(bytes), -1, 0, nullptr, nullptr, nullptr};
    void* registration = nullptr;
    int code = au_mm_register(&memory, &registration);
    if (code) return code;
    ::au_mm_access_set access{};
    access.mStructSize = sizeof(access); access.mCount = 1;
    access.mRanges[0] = {bytes, sizeof(bytes), AU_MM_WRITE};
    code = au_mm_cpu_begin(&access, 1, scope);
    const int retired = au_mm_unregister(registration); // Scope still pins the private registration.
    return code ? code : retired;
}
AURA_FLOW_TEST_EXPORT int flow_test_mm_finish(void* scope) { return au_mm_cpu_finish(scope, 0); }
}
