#ifndef AURA_GPU_XOPENCL_H_
#define AURA_GPU_XOPENCL_H_
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include "gpu/xopencl_api.h"
#include "mm/xaccess.h"
#include "sys/xvisibility.h"

namespace au {
namespace gpu AU_LOCAL {
// These C++11 value adapters are local to the consumer. Only versioned C PODs
// and integer results cross the implementation boundary; no OpenCL/STL ABI does.
typedef ::au_gpu_arg KernelArg;
inline int bufferOffset(const au::mm::Access& access, std::size_t& offset) noexcept
{ return ::au_gpu_buffer_offset(&access, &offset); }
inline au::gpu::KernelArg buffer(const au::mm::Access& access) noexcept
{
    au::gpu::KernelArg arg{}; arg.mStructSize = sizeof(arg); arg.mKind = AU_GPU_BUFFER; arg.mAccess = access;
    return arg;
}
inline au::gpu::KernelArg image2D(const au::mm::Access& access, std::size_t width, std::size_t height,
    std::size_t rowPitch, std::uint32_t channelOrder, std::uint32_t channelType) noexcept
{
    auto arg = au::gpu::buffer(access); arg.mKind = AU_GPU_IMAGE2D;
    arg.mWidth = width; arg.mHeight = height; arg.mRowPitch = rowPitch;
    arg.mChannelOrder = channelOrder; arg.mChannelType = channelType; return arg;
}
inline au::gpu::KernelArg localMemory(std::size_t size) noexcept
{
    au::gpu::KernelArg arg{}; arg.mStructSize = sizeof(arg); arg.mKind = AU_GPU_LOCAL; arg.mSize = size; return arg;
}
// Native cl_mem is retained and context-checked. Host storage/cache declared
// outside Aura remains the caller's responsibility when no Access is supplied.
inline au::gpu::KernelArg nativeMemory(void* memory) noexcept
{
    au::gpu::KernelArg arg{}; arg.mStructSize = sizeof(arg); arg.mKind = AU_GPU_NATIVE_MEMORY; arg.mValue = memory; return arg;
}
namespace detail {
inline ::au_gpu_arg describe(const au::gpu::KernelArg& arg) noexcept { return arg; }
template<class T> ::au_gpu_arg describe(const T& value) noexcept
{
    static_assert(std::is_trivially_copyable<T>::value && !std::is_pointer<T>::value,
                  "Kernel scalars must be trivially copyable values; describe memory explicitly");
    ::au_gpu_arg arg{}; arg.mStructSize = sizeof(arg); arg.mKind = AU_GPU_SCALAR;
    arg.mValue = &value; arg.mSize = sizeof(value); return arg;
}
}
class NDRange {
public:
    NDRange() noexcept : mValues{{0, 0, 0}}, mDimensions(0) {}
    NDRange(std::initializer_list<std::size_t> values) noexcept : mValues{{0, 0, 0}}, mDimensions(values.size())
    { std::size_t i = 0; for (auto value : values) { if (i < 3) mValues[i++] = value; } }
    std::size_t dimensions() const noexcept { return mDimensions; }
    std::size_t operator[](std::size_t i) const noexcept { return i < 3 ? mValues[i] : 0; }
private:
    std::array<std::size_t, 3> mValues;
    std::size_t mDimensions;
};
class Launch {
public:
    Launch(std::uint32_t backend, const au::gpu::NDRange& global, const au::gpu::NDRange& local,
           const au::gpu::NDRange& offset) noexcept : mBackend(backend), mRange{}
    {
        mRange.mStructSize = sizeof(mRange);
        mRange.mDimensions = global.dimensions() <= 3 ? static_cast<std::uint32_t>(global.dimensions()) : 0;
        for (std::size_t i = 0; i < 3; ++i) {
            mRange.mGlobal[i] = global[i]; mRange.mLocal[i] = local[i]; mRange.mOffset[i] = offset[i];
        }
        if (local.dimensions() && local.dimensions() != global.dimensions()) mRange.mDimensions = 0;
        if (offset.dimensions() && offset.dimensions() != global.dimensions()) mRange.mDimensions = 0;
    }
    template<class... Args>
    int enqueue(const char* kernel, const char* source, void** nativeEvent, const Args&... args) const noexcept
    {
        const std::array<::au_gpu_arg, sizeof...(Args)> descriptions{{au::gpu::detail::describe(args)...}};
        return ::au_gpu_enqueue(mBackend, &mRange, kernel, source, descriptions.data(), descriptions.size(), nativeEvent);
    }
private:
    std::uint32_t mBackend;
    ::au_gpu_range mRange;
};
class Wrapper {
public:
    bool available() const noexcept { return ::au_gpu_available(mBackend) != 0; }
    int nativeContext(void*& context) const noexcept { return ::au_gpu_context(mBackend, &context); }
    int init(const std::string& directory = {}, bool profiling = false) const noexcept
    { return ::au_gpu_init(mBackend, directory.c_str(), profiling ? 1 : 0); }
    int deinit() const noexcept { return ::au_gpu_deinit(mBackend); }
    int prepare(const char* kernel, const char* source, const char* options = "-cl-std=CL1.2") const noexcept
    { return ::au_gpu_prepare(mBackend, kernel, source, options); }
    au::gpu::Launch setNDRange(au::gpu::NDRange global, au::gpu::NDRange local = {}, au::gpu::NDRange offset = {}) const noexcept
    { return au::gpu::Launch(mBackend, global, local, offset); }
    int flush() const noexcept { return ::au_gpu_flush(mBackend); }
    int finish() const noexcept { return ::au_gpu_finish(mBackend); }
    int releaseEvent(void* event) const noexcept { return ::au_gpu_event_release(mBackend, event); }
    Wrapper(const au::gpu::Wrapper&) = delete;
    au::gpu::Wrapper& operator=(const au::gpu::Wrapper&) = delete;
    Wrapper(au::gpu::Wrapper&&) = delete;
    au::gpu::Wrapper& operator=(au::gpu::Wrapper&&) = delete;
protected:
    explicit Wrapper(std::uint32_t backend) noexcept : mBackend(backend) {}
    ~Wrapper() noexcept = default;
private:
    std::uint32_t mBackend;
};
class CLWrapper final : public au::gpu::Wrapper {
public:
    static const au::gpu::CLWrapper& get() noexcept { static const au::gpu::CLWrapper value; return value; }
private:
    CLWrapper() noexcept : au::gpu::Wrapper(AU_GPU_GENERAL) {}
};
class OpenCLWrapper final : public au::gpu::Wrapper {
public:
    static const au::gpu::OpenCLWrapper& get() noexcept { static const au::gpu::OpenCLWrapper value; return value; }
private:
    OpenCLWrapper() noexcept : au::gpu::Wrapper(AU_GPU_DMA_BUFFER) {}
};
} // namespace gpu
} // namespace au
#endif
