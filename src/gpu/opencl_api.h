#ifndef AURA_GPU_NATIVE_OPENCL_API_H_
#define AURA_GPU_NATIVE_OPENCL_API_H_
#include "CL/cl.h"
namespace au {
namespace gpu {
namespace detail {
// Private side-effect seam, not a second runtime or public backend abstraction.
// A test table is immutable while a wrapper instance is initialized.
struct NativeApi {
#define AU_CL_ENTRY(Name) decltype(&::cl##Name) m##Name = &::cl##Name
    AU_CL_ENTRY(GetPlatformInfo); AU_CL_ENTRY(GetPlatformIDs); AU_CL_ENTRY(GetDeviceIDs); AU_CL_ENTRY(GetDeviceInfo);
    AU_CL_ENTRY(CreateContext); AU_CL_ENTRY(ReleaseContext);
    AU_CL_ENTRY(CreateCommandQueue); AU_CL_ENTRY(ReleaseCommandQueue);
    AU_CL_ENTRY(CreateProgramWithBinary); AU_CL_ENTRY(GetProgramInfo); AU_CL_ENTRY(CreateProgramWithSource); AU_CL_ENTRY(BuildProgram); AU_CL_ENTRY(GetProgramBuildInfo); AU_CL_ENTRY(ReleaseProgram);
    AU_CL_ENTRY(CreateKernel); AU_CL_ENTRY(GetKernelInfo); AU_CL_ENTRY(SetKernelArg); AU_CL_ENTRY(ReleaseKernel);
    AU_CL_ENTRY(CreateBuffer); AU_CL_ENTRY(CreateImage); AU_CL_ENTRY(GetImageInfo); AU_CL_ENTRY(GetMemObjectInfo); AU_CL_ENTRY(RetainMemObject); AU_CL_ENTRY(ReleaseMemObject);
    AU_CL_ENTRY(EnqueueWriteBuffer); AU_CL_ENTRY(EnqueueReadBuffer);
    AU_CL_ENTRY(EnqueueWriteImage); AU_CL_ENTRY(EnqueueReadImage); AU_CL_ENTRY(EnqueueNDRangeKernel);
    AU_CL_ENTRY(GetEventInfo); AU_CL_ENTRY(WaitForEvents); AU_CL_ENTRY(RetainEvent); AU_CL_ENTRY(ReleaseEvent);
    AU_CL_ENTRY(GetEventProfilingInfo); AU_CL_ENTRY(Flush); AU_CL_ENTRY(Finish);
    AU_CL_ENTRY(GetExtensionFunctionAddressForPlatform);
#undef AU_CL_ENTRY
};
#ifdef AURA_GPU_TESTING
// Only an uninitialized wrapper can change tables. Caller keeps the immutable
// table alive until deinit AND release of all returned native events. Changing
// tables while such a borrowed native handle exists violates this test contract.
int setNativeApi(unsigned int backend, const au::gpu::detail::NativeApi* api) noexcept;
void* backendIdentity(unsigned int backend) noexcept;
#endif
} // namespace detail
} // namespace gpu
} // namespace au
#endif
