#include "gpu/xopencl.h"
#include "gpu/opencl_api.h"
#include "flow/xflow.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
// Process-exit lifetime model: this immutable function table and trivial
// counters outlive function-local Aura statics. There is deliberately no
// explicit shutdown/deinit in main; the normal-exit fallback must order them.
au::gpu::detail::NativeApi gApi;
std::atomic<int> gNative{0}, gEvents{0}, gProfileQueries{0}, gManagedCode{91};
std::atomic<bool> gExitGate{false};
struct Event { std::atomic<int> mReferences{1}; };
template<class T> T create(cl_int* code) { *code = 0; gNative.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<T>(new int(0)); }
template<class T> cl_int release(T object) { delete reinterpret_cast<int*>(object); gNative.fetch_sub(1, std::memory_order_relaxed); return 0; }
cl_int CL_API_CALL platforms(cl_uint count, cl_platform_id* output, cl_uint* size)
{ if (size) *size = 1; if (count) *output = reinterpret_cast<cl_platform_id>(1); return 0; }
cl_int CL_API_CALL devices(cl_platform_id, cl_device_type, cl_uint count, cl_device_id* output, cl_uint* size)
{ if (size) *size = 1; if (count) *output = reinterpret_cast<cl_device_id>(2); return 0; }
cl_int CL_API_CALL deviceInfo(cl_device_id, cl_device_info name, std::size_t, void* output, std::size_t* size)
{ if (name != CL_DEVICE_EXTENSIONS) return CL_INVALID_VALUE; if (size) *size = 1; if (output) *static_cast<char*>(output) = '\0'; return 0; }
cl_context CL_API_CALL context(const cl_context_properties*, cl_uint, const cl_device_id*,
    void (CL_CALLBACK *)(const char*, const void*, std::size_t, void*), void*, cl_int* code) { return create<cl_context>(code); }
cl_command_queue CL_API_CALL queue(cl_context, cl_device_id, cl_command_queue_properties, cl_int* code) { return create<cl_command_queue>(code); }
cl_program CL_API_CALL program(cl_context, cl_uint, const char**, const std::size_t*, cl_int* code) { return create<cl_program>(code); }
cl_int CL_API_CALL build(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_CALLBACK *)(cl_program, void*), void*) { return 0; }
cl_kernel CL_API_CALL kernel(cl_program, const char*, cl_int* code) { return create<cl_kernel>(code); }
cl_int CL_API_CALL kernelInfo(cl_kernel, cl_kernel_info name, std::size_t, void* output, std::size_t*)
{ if (name != CL_KERNEL_NUM_ARGS) return CL_INVALID_VALUE; *static_cast<cl_uint*>(output) = 0; return 0; }
cl_int CL_API_CALL enqueue(cl_command_queue, cl_kernel, cl_uint, const std::size_t*, const std::size_t*, const std::size_t*,
    cl_uint, const cl_event*, cl_event* output)
{ *output = reinterpret_cast<cl_event>(new Event); gEvents.fetch_add(1, std::memory_order_relaxed); return 0; }
cl_int CL_API_CALL eventInfo(cl_event, cl_event_info name, std::size_t, void* output, std::size_t*)
{ if (name != CL_EVENT_COMMAND_EXECUTION_STATUS) return CL_INVALID_VALUE; *static_cast<cl_int*>(output) = CL_COMPLETE; return 0; }
cl_int CL_API_CALL wait(cl_uint, const cl_event*) { return 0; }
cl_int CL_API_CALL retainEvent(cl_event event)
{ reinterpret_cast<Event*>(event)->mReferences.fetch_add(1, std::memory_order_relaxed); return 0; }
cl_int CL_API_CALL releaseEvent(cl_event value)
{
    auto* event = reinterpret_cast<Event*>(value);
    if (event->mReferences.fetch_sub(1, std::memory_order_acq_rel) == 1) { delete event; gEvents.fetch_sub(1, std::memory_order_relaxed); }
    return 0;
}
cl_int CL_API_CALL profiling(cl_event, cl_profiling_info name, std::size_t, void* output, std::size_t*)
{
    gProfileQueries.fetch_add(1, std::memory_order_relaxed);
    *static_cast<cl_ulong*>(output) = name - CL_PROFILING_COMMAND_QUEUED; return 0;
}
cl_int CL_API_CALL flush(cl_command_queue) { return 0; }
cl_int CL_API_CALL releaseContext(cl_context object) { return release(object); }
cl_int CL_API_CALL releaseQueue(cl_command_queue object) { return release(object); }
cl_int CL_API_CALL releaseProgram(cl_program object) { return release(object); }
cl_int CL_API_CALL releaseKernel(cl_kernel object) { return release(object); }
void openExitGate() { gExitGate.store(true, std::memory_order_release); }
void verifyTeardown()
{
    if (gNative.load(std::memory_order_relaxed) || gEvents.load(std::memory_order_relaxed) ||
        gProfileQueries.load(std::memory_order_relaxed) != 8 || gManagedCode.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "teardown native=%d events=%d profiling=%d managed=%d\n", gNative.load(std::memory_order_relaxed),
            gEvents.load(std::memory_order_relaxed), gProfileQueries.load(std::memory_order_relaxed), gManagedCode.load(std::memory_order_relaxed));
        std::abort();
    }
}
}

int main()
{
    if (std::atexit(&verifyTeardown)) return 1;
    gApi.mGetPlatformIDs = &platforms; gApi.mGetDeviceIDs = &devices; gApi.mGetDeviceInfo = &deviceInfo;
    gApi.mCreateContext = &context; gApi.mReleaseContext = &releaseContext;
    gApi.mCreateCommandQueue = &queue; gApi.mReleaseCommandQueue = &releaseQueue;
    gApi.mCreateProgramWithSource = &program; gApi.mBuildProgram = &build; gApi.mReleaseProgram = &releaseProgram;
    gApi.mCreateKernel = &kernel; gApi.mGetKernelInfo = &kernelInfo; gApi.mReleaseKernel = &releaseKernel;
    gApi.mEnqueueNDRangeKernel = &enqueue; gApi.mGetEventInfo = &eventInfo; gApi.mWaitForEvents = &wait;
    gApi.mRetainEvent = &retainEvent; gApi.mReleaseEvent = &releaseEvent; gApi.mGetEventProfilingInfo = &profiling; gApi.mFlush = &flush;
    if (au::gpu::detail::setNativeApi(0, &gApi) || au::gpu::detail::setNativeApi(1, &gApi)) return 2;
    if (au::gpu::CLWrapper::get().init({}, true) || au::gpu::OpenCLWrapper::get().init({}, true)) return 3;
    if (au::flow::XFlow::get().setTracing(true)) return 4;
    if (au::gpu::OpenCLWrapper::get().setNDRange({1}).enqueue("empty", "__kernel void empty() {}", nullptr)) return 5;
    auto task = au::flow::XFlow::get().enqueue([]() -> int {
        while (!gExitGate.load(std::memory_order_acquire)) std::this_thread::yield();
        const int code = au::gpu::CLWrapper::get().setNDRange({1}).enqueue("empty", "__kernel void empty() {}", nullptr);
        gManagedCode.store(code, std::memory_order_relaxed); return code;
    });
    if (task.submissionCode()) return 6;
    return std::atexit(&openExitGate) ? 7 : 0;
}
