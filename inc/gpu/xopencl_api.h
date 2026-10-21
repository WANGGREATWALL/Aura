#ifndef AURA_GPU_XOPENCL_API_H_
#define AURA_GPU_XOPENCL_API_H_
#include <stddef.h>
#include <stdint.h>
#include "mm/xaccess_api.h"
#ifdef __cplusplus
#define AU_GPU_NOEXCEPT noexcept
extern "C" {
#else
#define AU_GPU_NOEXCEPT
#endif
#define AURA_GPU_OPENCL_ABI_VERSION 1u
enum { AU_GPU_GENERAL = 0, AU_GPU_DMA_BUFFER = 1 };
enum { AU_GPU_SCALAR = 0, AU_GPU_BUFFER = 1, AU_GPU_IMAGE2D = 2, AU_GPU_LOCAL = 3, AU_GPU_NATIVE_MEMORY = 4 };
typedef struct au_gpu_range {
    uint32_t mStructSize;
    uint32_t mDimensions;
    size_t mGlobal[3];
    size_t mLocal[3]; /* All zero selects the runtime's work-group size. */
    size_t mOffset[3];
} au_gpu_range;
typedef struct au_gpu_arg {
    uint32_t mStructSize;
    uint32_t mKind;
    au_mm_access_set mAccess;
    const void* mValue;
    size_t mSize;
    size_t mWidth;
    size_t mHeight;
    size_t mRowPitch;
    uint32_t mChannelOrder; /* OpenCL channel order/type numerical constants. */
    uint32_t mChannelType;
} au_gpu_arg;
uint32_t au_gpu_opencl_version(void) AU_GPU_NOEXCEPT;
/* Empty directory disables disk binaries. Otherwise use an application-private
 * directory: device/driver/source/options are matched exactly; corrupt or
 * rejected binaries fall back to source. Disk failure is nonfatal. */
int au_gpu_init(uint32_t backend, const char* binaryDirectory, int profiling) AU_GPU_NOEXCEPT;
int au_gpu_available(uint32_t backend) AU_GPU_NOEXCEPT;
/* Borrowed cl_context, valid until deinit. Native interop callers coordinate
 * deinit and release their native objects before unloading this Aura instance. */
int au_gpu_context(uint32_t backend, void** nativeContext) AU_GPU_NOEXCEPT;
int au_gpu_deinit(uint32_t backend) AU_GPU_NOEXCEPT;
int au_gpu_prepare(uint32_t backend, const char* kernel, const char* source, const char* options) AU_GPU_NOEXCEPT;
/* Arguments are consumed during this call; accepted native work owns its leases.
 * nativeEvent, if non-null, receives a retained cl_event (device completion only).
 * Release it with au_gpu_event_release. Task/fence/finish include host cleanup. */
int au_gpu_enqueue(uint32_t backend, const au_gpu_range* range, const char* kernel,
                   const char* source, const au_gpu_arg* args, size_t count, void** nativeEvent) AU_GPU_NOEXCEPT;
int au_gpu_flush(uint32_t backend) AU_GPU_NOEXCEPT;
int au_gpu_finish(uint32_t backend) AU_GPU_NOEXCEPT;
int au_gpu_event_release(uint32_t backend, void* nativeEvent) AU_GPU_NOEXCEPT;
/* Managed buffer arguments bind the WHOLE allocation, without sub-buffers.
 * Query the first range's byte offset and pass it explicitly to ROI kernels. */
int au_gpu_buffer_offset(const au_mm_access_set* access, size_t* offset) AU_GPU_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef AU_GPU_NOEXCEPT
#endif
