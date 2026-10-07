/**
 * @file opencl_buffer.h
 * @brief One dmabuf fd -> one cl_mem, with lifetime decoupled from the source image.
 *
 * OpenCLBuffer imports a single DMA-BUF fd into one cl::Buffer. The whole image
 * (every plane) lives in that one buffer; individual planes are addressed inside
 * the kernel via (buffer, fdOffset[i], stride[i]) — there is no cl::Image2D and
 * no clCreateSubBuffer, hence no CL_DEVICE_MEM_BASE_ADDR_ALIGN constraint.
 *
 * Lifetime model:
 *   - dup(fd) at construction; close(fd) at destruction.
 *   - The cl::Buffer is imported lazily on first buffer() and RAII-owned.
 *   - Reference-counted via shared_ptr (see OpenCLRegistry); survives across source
 *     image destruction and across pending kernel completion.
 */

#ifndef AU_GPU_OPENCL_BUFFER_H_
#define AU_GPU_OPENCL_BUFFER_H_

#include <cstddef>

#include "cl_symbols.h"

namespace au {
namespace gpu {

class OpenCLRegistry;

/// A single dmabuf-backed cl_mem. Non-copyable, shared_ptr-managed.
class OpenCLBuffer
{
public:
    ~OpenCLBuffer() noexcept;

    OpenCLBuffer(const OpenCLBuffer&)            = delete;
    OpenCLBuffer& operator=(const OpenCLBuffer&) = delete;

    /**
     * @brief Returns (and lazily imports) the cl::Buffer wrapping the dup'd fd.
     *
     * MediaTek: clImportMemoryARM with CL_IMPORT_TYPE_DMA_BUF_ARM.
     * Qualcomm: clCreateBuffer with CL_MEM_EXT_HOST_PTR_QCOM.
     *
     * @return non-empty cl::Buffer on success; empty cl::Buffer on failure.
     */
    ::cl::Buffer& buffer() noexcept;

    /// CPU wrote -> device will read; flush CPU caches for this dmabuf.
    int syncToDevice() noexcept;

    /// Device wrote -> CPU will read; invalidate CPU caches for this dmabuf.
    int syncToHost() noexcept;

    int         fd() const noexcept { return mFd; }
    std::size_t size() const noexcept { return mSize; }

private:
    friend class OpenCLRegistry;

    OpenCLBuffer(int dupedFd, void* hostPtr, std::size_t size) noexcept;

    int          mFd{-1};
    void*        mHostPtr{nullptr};
    std::size_t  mSize{0};
    ::cl::Buffer mBuffer;
};

} // namespace gpu
} // namespace au

#endif // AU_GPU_OPENCL_BUFFER_H_
