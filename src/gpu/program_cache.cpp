#include "gpu/program_cache.h"
#include "file/xfile.h"
#include "sys/atomic_file.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
constexpr std::size_t kMaximumCacheBytes = 128 * 1024 * 1024;
void appendNumber(std::string& output, std::uint64_t value)
{ for (unsigned int i = 0; i < 8; ++i) { output.push_back(static_cast<char>(value & 255)); value >>= 8; } }
void appendField(std::string& output, const std::string& field)
{ appendNumber(output, field.size()); output += field; }
std::uint64_t number(const unsigned char* input) noexcept
{ std::uint64_t value = 0; for (unsigned int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(input[i]) << (i * 8); return value; }
std::uint64_t checksum(const void* input, std::size_t size, std::uint64_t seed = 14695981039346656037ull) noexcept
{
    const auto* bytes = static_cast<const unsigned char*>(input);
    for (std::size_t i = 0; i < size; ++i) { seed ^= bytes[i]; seed *= 1099511628211ull; }
    return seed;
}
}
std::string au::gpu::detail::binaryDeviceKey(const au::gpu::detail::NativeApi& api, cl_platform_id platform, cl_device_id device)
{
    std::string key = "Aura.OpenCL.Binary.v1.CL1.2";
    for (auto query : {CL_PLATFORM_NAME, CL_PLATFORM_VENDOR, CL_PLATFORM_VERSION}) {
        std::size_t size = 0;
        if (api.mGetPlatformInfo(platform, query, 0, nullptr, &size) || !size || size > 65536) return {};
        std::string field(size, '\0');
        if (api.mGetPlatformInfo(platform, query, size, &field[0], nullptr) || field.back() != '\0') return {};
        appendField(key, field);
    }
    for (auto query : {CL_DEVICE_NAME, CL_DEVICE_VENDOR, CL_DEVICE_VERSION, CL_DRIVER_VERSION, CL_DEVICE_OPENCL_C_VERSION, CL_DEVICE_PROFILE}) {
        std::size_t size = 0;
        if (api.mGetDeviceInfo(device, query, 0, nullptr, &size) || !size || size > 65536) return {};
        std::string field(size, '\0');
        if (api.mGetDeviceInfo(device, query, size, &field[0], nullptr) || field.back() != '\0') return {};
        appendField(key, field);
    }
    cl_uint vendor = 0, bits = 0;
    if (api.mGetDeviceInfo(device, CL_DEVICE_VENDOR_ID, sizeof(vendor), &vendor, nullptr) ||
        api.mGetDeviceInfo(device, CL_DEVICE_ADDRESS_BITS, sizeof(bits), &bits, nullptr)) return {};
    appendNumber(key, vendor); appendNumber(key, bits); return key;
}
std::string au::gpu::detail::binaryKey(const std::string& device, const std::string& source, const std::string& options)
{ std::string key; appendField(key, device); appendField(key, source); appendField(key, options); return key; }
std::string au::gpu::detail::binaryPath(const std::string& directory, const std::string& key)
{
    const auto first = checksum(key.data(), key.size());
    const auto second = checksum(key.data(), key.size(), 7809847782465536322ull);
    char name[64]{};
    std::snprintf(name, sizeof(name), "aucl-%016llx-%016llx.bin", static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
    return (au::file::XPath(directory) / name).string();
}
bool au::gpu::detail::readBinary(const std::string& path, const std::string& key, std::vector<unsigned char>& binary) noexcept
{
    binary.clear();
    try {
        const auto size = au::file::sizeOf(path);
        if (size < 32 || size > kMaximumCacheBytes) return false;
        std::vector<unsigned char> contents(size);
        if (au::file::read(path, contents.data(), contents.size())) return false;
        if (std::memcmp(contents.data(), "AUCLBC01", 8)) return false;
        const auto keySize = number(contents.data() + 8), binarySize = number(contents.data() + 16);
        if (keySize != key.size() || keySize > size - 32 || !binarySize || binarySize != size - 32 - keySize ||
            std::memcmp(contents.data() + 32, key.data(), key.size()) ||
            checksum(contents.data() + 32, size - 32) != number(contents.data() + 24)) return false;
        binary.assign(contents.begin() + static_cast<std::ptrdiff_t>(32 + key.size()), contents.end());
        return true;
    } catch (...) { binary.clear(); return false; }
}
void au::gpu::detail::writeBinary(const std::string& path, const std::string& key, const au::gpu::detail::NativeApi& api, cl_program program) noexcept
{
    try {
        if (key.size() >= kMaximumCacheBytes - 32) return;
        cl_uint devices = 0; std::size_t size = 0;
        if (api.mGetProgramInfo(program, CL_PROGRAM_NUM_DEVICES, sizeof(devices), &devices, nullptr) || devices != 1 ||
            api.mGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES, sizeof(size), &size, nullptr) || !size ||
            size > kMaximumCacheBytes - 32 - key.size()) return;
        std::string contents = "AUCLBC01"; appendNumber(contents, key.size()); appendNumber(contents, size); appendNumber(contents, 0);
        contents += key; const auto start = contents.size(); contents.resize(start + size);
        auto* output = reinterpret_cast<unsigned char*>(&contents[start]);
        if (api.mGetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(output), &output, nullptr)) return;
        std::string digest; appendNumber(digest, checksum(contents.data() + 32, contents.size() - 32));
        std::memcpy(&contents[24], digest.data(), digest.size());
        (void)au::sys::replaceFile(path, contents.data(), contents.size());
    } catch (...) { /* A cache miss/write failure must not change computation. */ }
}
