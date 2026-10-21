#ifndef AURA_SYS_OPENCL_MEMORY_H_
#define AURA_SYS_OPENCL_MEMORY_H_
#include "CL/cl.h"
#include <cstddef>
namespace au {
namespace sys {
namespace opencl {
struct ImportCapabilities { void* mArmImport = nullptr; };
// Capability strings and extension lookup, never a vendor-name heuristic.
int queryImports(cl_platform_id platform, cl_device_id device, decltype(&::clGetDeviceInfo) deviceInfo,
    decltype(&::clGetExtensionFunctionAddressForPlatform) lookup,
    au::sys::opencl::ImportCapabilities& out, int* native) noexcept;
int importDmaBuf(const au::sys::opencl::ImportCapabilities& capabilities, cl_context context,
    int fd, std::size_t size, cl_mem* out, int* native) noexcept;
} // namespace opencl
} // namespace sys
} // namespace au
#endif
