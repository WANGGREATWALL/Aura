/**
 * @file opencl_wrapper.h
 * @brief Self-contained, buffer-only OpenCL wrapper built on the single-fd image model.
 *
 * OpenCLWrapper is an independent sibling of CLWrapper. It intentionally supports
 * ONLY cl::Buffer arguments — no cl::Image2D, no sub-buffers. A cv::Image is
 * imported as one dmabuf-backed OpenCLBuffer (see opencl_buffer.h / opencl_registry.h);
 * planes are addressed inside the kernel via (buffer, fdOffset[i], stride[i]).
 * This sidesteps all device memory-alignment constraints.
 *
 * Async lifetime: enqueue() collects the shared_ptr<OpenCLBuffer> of every buffer
 * argument and binds them to the kernel completion event (clSetEventCallback,
 * CL_COMPLETE), so the underlying cl_mem outlives a temporary source image.
 *
 * Input contract for buf(img):
 *   - img.fd[0] > 0 (DMA-BUF backed).
 *   - Multi-plane images share one fd; fdOffset[i] gives each plane's byte offset.
 */

#ifndef AU_GPU_OPENCL_WRAPPER_H_
#define AU_GPU_OPENCL_WRAPPER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cl_symbols.h"
#include "log/xlogger.h"  // XCHECK_* macros
#include "opencl_arg.h"
#include "vivo_comdef.h"  // VDKResult*

namespace au {
namespace gpu {

/// Human-readable OpenCL error string (shared helper, defined in cl_wrapper.cpp).
std::string clErrorInfo(int err);

class OpenCLWrapper
{
public:
    static OpenCLWrapper& get();

    ~OpenCLWrapper();

    OpenCLWrapper(const OpenCLWrapper&)            = delete;
    OpenCLWrapper& operator=(const OpenCLWrapper&) = delete;

    /// True once platform/device/context/queue are all live.
    bool available() const noexcept;

    /// Raw cl context accessor (used by OpenCLBuffer's lazy import).
    cl_context context() const noexcept { return mContext(); }

    cl_platform_id platform() const noexcept { return mPlatforms.empty() ? nullptr : mPlatforms.front()(); }

    int init(std::string folderBinary, bool enableProfiling = false);
    int deinit();

    OpenCLWrapper& setNDRange(::cl::NDRange global, ::cl::NDRange local = {1, 1}, ::cl::NDRange offset = {0, 0});

    /**
     * @brief Compiles/loads @p nameKernel and enqueues it with @p args.
     *
     * Plain scalars/buffers are forwarded to cl::Kernel::setArg. OpenCLArgBuffer
     * wrappers additionally get their shared_ptr captured until kernel completion.
     */
    template <typename... Args>
    int enqueue(const std::string& nameKernel, const char* kernelSource, ::cl::Event* event, Args... args)
    {
        XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);

        ::cl::Kernel kernel;
        int retKernel = requireKernel(kernel, nameKernel, kernelSource);
        XCHECK_WITH_RET(retKernel == VDKResultSuccess, retKernel);

        std::vector<std::shared_ptr<OpenCLBuffer>> pending;
        int idx = 0;
        std::vector<int> rc{setArgDispatch(kernel, idx, args, pending)...};
        (void)rc;
        for (size_t i = 0; i < rc.size(); ++i) {
            XCHECK_WITH_MSG(rc[i] == CL_SUCCESS, VDKResultEInvalidParam, "setArg[%zu] failed: %s!\n", i,
                            clErrorInfo(rc[i]).c_str());
        }

        ::cl::Event localEvent;
        ::cl::Event* evtOut = event ? event : &localEvent;
        int retEnqueue = mCommandQueue.enqueueNDRangeKernel(kernel, mOffset, mGlobal, mLocal, nullptr, evtOut);
        XCHECK_WITH_MSG(retEnqueue == CL_SUCCESS, retEnqueue, "clError: %s\n", clErrorInfo(retEnqueue).c_str());

        if (!pending.empty()) {
            attachPending(*evtOut, std::move(pending));
        }

        flush();  // flush in time

        return VDKResultSuccess;
    }

    int flush();
    int finish();

private:
    OpenCLWrapper();

    int getPlatform();
    int getDevice();
    int createContext();
    int createCommandQueue(cl_command_queue_properties properties = 0);
    int requireKernel(::cl::Kernel& kernel,
                      const std::string& nameKernel,
                      const char* kernelSource,
                      const std::string& optionsCompile = "-cl-std=CL2.0 -cl-fast-relaxed-math");
    int deviceKey(std::string& key);

    /// Registers a CL_COMPLETE callback releasing @p pending after the kernel runs.
    void attachPending(::cl::Event& evt, std::vector<std::shared_ptr<OpenCLBuffer>>&& pending);

    // --- arg dispatch: plain args vs OpenCLArgBuffer wrappers ------------------
    template <class T>
    static typename std::enable_if<!isOpenCLArg<T>(), int>::type setArgDispatch(
        ::cl::Kernel& kernel, int& idx, const T& arg, std::vector<std::shared_ptr<OpenCLBuffer>>&)
    {
        const int rc = kernel.setArg(idx, arg);
        ++idx;
        return rc;
    }

    static int setArgDispatch(::cl::Kernel& kernel,
                              int& idx,
                              const OpenCLArgBuffer& arg,
                              std::vector<std::shared_ptr<OpenCLBuffer>>& pending)
    {
        if (!arg.handle) {
            ++idx;
            return CL_INVALID_MEM_OBJECT;
        }
        const int rc = kernel.setArg(idx, arg.handle->buffer());
        ++idx;
        pending.push_back(arg.handle);
        return rc;
    }

private:
    std::string mFolderBinary;
    bool mEnableProfiling{false};

    std::vector<::cl::Platform> mPlatforms;
    std::vector<::cl::Device> mDevices;
    ::cl::Context mContext;
    ::cl::CommandQueue mCommandQueue;
    std::map<std::string, ::cl::Kernel> mKernels;

    ::cl::NDRange mOffset;
    ::cl::NDRange mGlobal;
    ::cl::NDRange mLocal;
};

}  // namespace gpu
}  // namespace au

#endif  // AU_GPU_OPENCL_WRAPPER_H_
