#include "opencl_buffer.h"

#include <unistd.h>

#include "CL/cl_ext.h"
#include "log/xerror.h"
#include "log/xlogger.h"
#include "mm/xmemory.h"
#include "opencl_wrapper.h"
#include "sys/xsystem.h"

namespace au {
namespace gpu {

OpenCLBuffer::OpenCLBuffer(int dupedFd, void* hostPtr, std::size_t size) noexcept
    : mFd(dupedFd), mHostPtr(hostPtr), mSize(size)
{
}

OpenCLBuffer::~OpenCLBuffer() noexcept
{
    mBuffer = ::cl::Buffer{};
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
}

::cl::Buffer& OpenCLBuffer::buffer() noexcept
{
    if (mBuffer()) {
        return mBuffer;
    }

    if (mFd < 0 || mSize == 0) {
        XLOG_E("OpenCLBuffer::buffer fd=%d size=%zu invalid\n", mFd, mSize);
        return mBuffer;
    }

    cl_context ctx = OpenCLWrapper::get().context();
    if (ctx == nullptr) {
        XLOG_E("OpenCLBuffer::buffer no CL context\n");
        return mBuffer;
    }

    cl_int retImport = CL_SUCCESS;
    cl_mem handle   = nullptr;

    if (sys::isMediaTekPlatform()) {
        cl_platform_id              plat = OpenCLWrapper::get().platform();
        static clImportMemoryARM_fn pfnImport =
            plat ? reinterpret_cast<clImportMemoryARM_fn>(
                       clGetExtensionFunctionAddressForPlatform(plat, "clImportMemoryARM"))
                 : nullptr;
        if (!pfnImport) {
            XLOG_E("OpenCLBuffer::buffer clImportMemoryARM not resolvable on this platform\n");
            return mBuffer;
        }

        cl_import_properties_arm props[] = {CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
                                           CL_IMPORT_DMA_BUF_DATA_CONSISTENCY_WITH_HOST_ARM, CL_FALSE, 0};
        int                      fd      = mFd;
        handle                           = pfnImport(ctx, CL_MEM_READ_WRITE, props, &fd, mSize, &retImport);
    } else {
        cl_mem_ion_host_ptr ion             = {{0}};
        ion.ext_host_ptr.allocation_type   = CL_MEM_ION_HOST_PTR_QCOM;
        ion.ext_host_ptr.host_cache_policy = CL_MEM_HOST_IOCOHERENT_QCOM;
        ion.ion_filedesc                    = mFd;
        ion.ion_hostptr                     = mHostPtr;
        handle = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM, mSize, &ion,
                                &retImport);
    }

    if (handle == nullptr || retImport != CL_SUCCESS) {
        XLOG_E("OpenCLBuffer::buffer import failed: %s\n", clErrorInfo(retImport).c_str());
        return mBuffer;
    }

    mBuffer = ::cl::Buffer(handle, /*retainObject=*/false);
    return mBuffer;
}

int OpenCLBuffer::syncToDevice() noexcept
{
    return mHostPtr ? au::mm::syncCpuToDevice(mHostPtr) : au::err::kSuccess;
}

int OpenCLBuffer::syncToHost() noexcept
{
    return mHostPtr ? au::mm::syncDeviceToCpu(mHostPtr) : au::err::kSuccess;
}

} // namespace gpu
} // namespace au
