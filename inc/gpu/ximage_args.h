#ifndef AURA_GPU_XIMAGE_ARGS_H_
#define AURA_GPU_XIMAGE_ARGS_H_
#include "gpu/xopencl.h"
#include "cv/ximage_access.h"
namespace au {
namespace gpu AU_LOCAL {
// One native buffer denotes the whole allocation, including all planes. Pass
// each plane's validated allocation offset/stride explicitly to the kernel.
inline au::gpu::KernelArg buf(const au::cv::Image& image,
                             au::mm::AccessMode mode = au::mm::AccessMode::ReadWrite) noexcept
{ return au::gpu::buffer(au::cv::imageAccess(image, mode)); }
inline au::gpu::KernelArg buf(const au::cv::XImage& image,
                             au::mm::AccessMode mode = au::mm::AccessMode::ReadWrite) noexcept
{ return au::gpu::buffer(au::cv::imageAccess(image.view(), mode)); }
} // namespace gpu
} // namespace au
#endif
