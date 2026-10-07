/**
 * @file opencl_arg.h
 * @brief Syntactic sugar for passing a cv::Image as an OpenCL buffer argument.
 *
 * @code
 *   #include "opencl_arg.h"
 *
 *   au::gpu::OpenCLWrapper::get().setNDRange(...).enqueue(
 *       "MyKernel", kSrc, nullptr,
 *       au::gpu::buf(src), src.fdOffset[0], src.stride[0],
 *       au::gpu::buf(dst), dst.fdOffset[0], dst.stride[0]);
 * @endcode
 *
 * @c buf() returns a small POD wrapper carrying a @c shared_ptr<OpenCLBuffer>. The
 * @c OpenCLWrapper::enqueue overload recognises the wrapper, forwards the underlying
 * @c cl::Buffer as the kernel arg, and captures the @c shared_ptr in a pending
 * list bound to the kernel event so the cl_mem outlives any temporary image.
 * Planes are located inside the kernel via (buffer, fdOffset[i], stride[i]).
 */

#ifndef AU_GPU_OPENCL_ARG_H_
#define AU_GPU_OPENCL_ARG_H_

#include <memory>
#include <type_traits>

#include "opencl_buffer.h"
#include "opencl_registry.h"

namespace au {
namespace cv {
class Image;
}
namespace gpu {

/// Wrapper signaling: pass the whole dmabuf as a cl::Buffer to the kernel.
struct OpenCLArgBuffer
{
    std::shared_ptr<OpenCLBuffer> handle;
};

/**
 * @brief Returns an OpenCLArgBuffer for @p img, flushing CPU caches to device.
 *
 * The wrapper holds a strong reference to the OpenCLBuffer, keeping the cl_mem
 * alive across the kernel's asynchronous lifetime.
 */
inline OpenCLArgBuffer buf(const cv::Image& img) noexcept
{
    auto h = importImage(img);
    if (h) {
        h->syncToDevice();
    }
    return OpenCLArgBuffer{std::move(h)};
}

// --- Trait: is T an OpenCL arg wrapper? (used by OpenCLWrapper::enqueue) ----------

template <class T>
struct IsOpenCLArg : std::false_type
{
};
template <>
struct IsOpenCLArg<OpenCLArgBuffer> : std::true_type
{
};

template <class T>
constexpr bool isOpenCLArg() noexcept
{
    return IsOpenCLArg<typename std::remove_cv<typename std::remove_reference<T>::type>::type>::value;
}

} // namespace gpu
} // namespace au

#endif // AU_GPU_OPENCL_ARG_H_
