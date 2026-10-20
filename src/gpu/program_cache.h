#ifndef AURA_GPU_PROGRAM_CACHE_H_
#define AURA_GPU_PROGRAM_CACHE_H_
#include "gpu/opencl_api.h"
#include <string>
#include <vector>
namespace au {
namespace gpu {
namespace detail {
// Optional, application-private cache. Exact metadata matching plus a checksum
// detects stale/partial entries; this is not authentication of untrusted code.
std::string binaryDeviceKey(const au::gpu::detail::NativeApi& api, cl_platform_id platform, cl_device_id device);
std::string binaryKey(const std::string& device, const std::string& source, const std::string& options);
std::string binaryPath(const std::string& directory, const std::string& key);
bool readBinary(const std::string& path, const std::string& key, std::vector<unsigned char>& binary) noexcept;
void writeBinary(const std::string& path, const std::string& key, const au::gpu::detail::NativeApi& api, cl_program program) noexcept;
} // namespace detail
} // namespace gpu
} // namespace au
#endif
