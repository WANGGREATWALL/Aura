/**
 * The OpenCL kernel code is written in a .cl file. To convert the kernel.cl file into a kernel.h file
 * and obtain the kernel string for OpenCL compilation, the following command can be used in the shell:
 *
 * pathCur=$(pwd)
 * cd ${pathCur}/../../src/common_utils/
 * xxd -i portrait_style.cl portrait_style_kernel.h
 * cd ${pathCur}/
 *
 */

#ifndef __CL_WRAPPER_H__
#define __CL_WRAPPER_H__

#include <map>
#include <string>
#include <vector>

#include "cl_symbols.h"
#include "cv/ximage.h"
#include "log/xlogger.h"


namespace au {
namespace gpu {

std::string clErrorInfo(int err);

/**
 * @return width if success, else 0
 */
size_t getImageWidth(const cl::Image& image);
size_t getImageHeight(const cl::Image& image);
size_t getImagePitch(const cl::Image& image);
size_t getImageDepth(const cl::Image& image);

bool isValid(const cl::Image& image);
bool isFormat(const cl::Image& image, const cl_image_format& format = {CL_R, CL_UNORM_INT8});
bool isSize(const cl::Image& image, size_t width, size_t height, size_t depth = 0);
bool isSameSize(const cl::Image& image0, const cl::Image& image1);
bool isSameFormat(const cl::Image& image0, const cl::Image& image1);
bool isSameSizeAndFormat(const cl::Image& image0, const cl::Image& image1);

std::string info(const cl::Image& image);


class CLWrapper
{
public:
    static CLWrapper& get()
    {
        static CLWrapper instance;
        return instance;
    }
    ~CLWrapper();

    CLWrapper(const CLWrapper&)            = delete;
    CLWrapper& operator=(const CLWrapper&) = delete;

    bool available() const;

    int init(std::string folderBinary, bool enableProfiling = false);
    int deinit();

    int createBuffer(cl::Buffer& dst, const VImage& image, cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);
    int createBuffer(cl::Buffer& dst,
                     void* data,
                     size_t sizeInByte,
                     cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);

    int createImage2D(cl::Image2D& dst,
                      const VImage& image,
                      cl::ImageFormat format = {CL_R, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);
    int createImage2D(cl::Image2D& dst,
                      void* data,
                      int width,
                      int height,
                      cl::ImageFormat format = {CL_RG, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);
    int createImage2D(cl::Image2D& dst,
                      int width,
                      int height,
                      cl::ImageFormat format = {CL_R, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_WRITE);

    // for cv::XImage.toGPU()
    int createBufferFromDMAImage(cl::Buffer& dst, const cv::XImage& src);
    int createImage2DFromCLBuffer(cl::Image2D& dst,
                                 const cl::Buffer& src,
                                 int width,
                                 int height,
                                 int stride,
                                 cl::ImageFormat format = {CL_R, CL_UNORM_INT8});

    int createImage3D(cl::Image3D& dst,
                      const VImage& image,
                      cl::ImageFormat format = {CL_R, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);
    int createImage3D(cl::Image3D& dst,
                      void* data,
                      int width,
                      int height,
                      int depth,
                      cl::ImageFormat format = {CL_RG, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR);
    int createImage3D(cl::Image3D& dst,
                      int width,
                      int height,
                      int depth,
                      cl::ImageFormat format = {CL_R, CL_UNORM_INT8},
                      cl_mem_flags flags = CL_MEM_READ_WRITE);

    int copyImage2D(cl::Image2D& dst, const cl::Image2D& src);

    int readImage2D(VImage& dst, const cl::Image2D& image, bool block);
    int readImage2D(VImage& dst, const cl::Image2D& plane0, const cl::Image2D& plane1);
    int readImage2D(void* dst, const cl::Image2D& image, int width, int height, int pitch, bool block);

    /**
     * @param dst the mapping output host image, could be 1/2 plane
     * @param image the cl mem object to be mapped
     */
    int mapImage2D(VImage& dst, const cl::Image2D& image);
    int mapImage2D(VImage& dst, const cl::Image2D& plane0, const cl::Image2D& plane1);

    void* mallocSVM(size_t sizeInByte,
                    size_t align = 64,
                    cl_mem_flags flags = CL_MEM_READ_WRITE | CL_MEM_SVM_FINE_GRAIN_BUFFER);
    int freeSVM(void* data);

    CLWrapper& setNDRange(cl::NDRange global, cl::NDRange local = {1, 1}, cl::NDRange offset = {0, 0});

    template <typename... Args>
    int enqueue(const std::string& nameKernel,
                const std::vector<const char*> kernelsCommon,  // common cl kernels neet to be used
                const char* kernelSource,                      // cl kernel source
                cl::Event* event,                              // could be nullptr
                Args... args)
    {
        XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);

        cl::Kernel kernel;
        int retGetKernel = requireKernel(kernel, nameKernel, kernelsCommon, kernelSource);
        XCHECK_WITH_RET(retGetKernel == VDKResultSuccess, retGetKernel);

        int idx = 0;
        std::vector<int> setargs{[&] { return kernel.setArg(idx++, args); }()...};

        for (int i = 0; i < setargs.size(); ++i) {
            XCHECK_WITH_MSG(setargs[i] == CL_SUCCESS, VDKResultEInvalidParam, "failed to setargs[%d]: %s!", i,
                            clErrorInfo(setargs[i]).c_str());
        }

        if (event) {
            int retEnqueueNDRangeKernel =
                mCommandQueue.enqueueNDRangeKernel(kernel, mOffset, mGlobal, mLocal, nullptr, event);
            XCHECK_WITH_MSG(retEnqueueNDRangeKernel == CL_SUCCESS, retEnqueueNDRangeKernel, "clError: %s",
                            clErrorInfo(retEnqueueNDRangeKernel).c_str());
            mEvents.emplace_back(nameKernel, *event);
        } else {
            cl::Event eventInner;
            int retEnqueueNDRangeKernel =
                mCommandQueue.enqueueNDRangeKernel(kernel, mOffset, mGlobal, mLocal, nullptr, &eventInner);
            XCHECK_WITH_MSG(retEnqueueNDRangeKernel == CL_SUCCESS, retEnqueueNDRangeKernel, "clError: %s",
                            clErrorInfo(retEnqueueNDRangeKernel).c_str());
            mEvents.emplace_back(nameKernel, eventInner);
        }

        flush();  // flush in time

        return VDKResultSuccess;
    }

    template <typename... Args>
    int enqueue(const std::string& nameKernel,
                const char* kernelSource,  // cl kernel source
                cl::Event* event,          // could be nullptr
                Args... args)
    {
        XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
        return enqueue(nameKernel, {}, kernelSource, event, args...);
    }

    /**
     * @brief
     * - flush(): batch submission of the above commands
     * - finish(): program end full sync
     * - wait(): wait for a specific event to complete
     * - barrier(): order barrier between commands
     * @note
     *           | BLOCK HOST | WAIT COMMAND TO COMPLETE
     * ----------|------------|-------------------------
     * flush()   | NO         | NO
     * finish()  | YES        | YES
     * wait()    | YES        | YES
     * barrier() | NO         | NO
     */
    int flush();
    int finish();
    int wait(const cl::Event& event);
    int barrier();

    int clearAndSyncEvents();

    int querySupportedImageFormats(const cl_mem_object_type object = CL_MEM_OBJECT_IMAGE2D,
                                   const cl_mem_flags flags = CL_MEM_READ_ONLY) const;
    int querySVMCapabilities() const;
    int queryPrintfSupport() const;

    size_t queryImageRowPitch(int width, int height, cl::ImageFormat format = {CL_R, CL_UNORM_INT8}) const;

    bool isImageFormatSupported(const cl_channel_order& order = CL_RGB,
                                const cl_channel_type& type = CL_UNORM_INT8) const;

private:
    struct CLEvent
    {
        CLEvent(const std::string& _name, const cl::Event& _event) : name(_name), event(_event) {}

        std::string name;
        cl::Event event;
    };

    CLWrapper();

    int getPlatform();
    int getDevice();
    int createContext(cl_context_properties* properties = nullptr);
    int createCommandQueue(cl_command_queue_properties queue_properties = 0);
    int getKernel(cl::Kernel& kernel, const std::string& name);

    int requireKernel(cl::Kernel& kernel,
                      const std::string& nameKernel,
                      const std::vector<const char*> kernelsCommon,
                      const char* kernelSource,
                      const std::string optionsCompile = "-cl-std=CL2.0 -cl-fast-relaxed-math");

    int getDeviceKey(std::string& key);

    int getDeviceName(std::string& name);
    int getDeviceVendor(std::string& vendor);
    int getDeviceVersion(std::string& version);
    int getDeviceDriver(std::string& driver);

    int getKernelName(const cl::Kernel kernel, std::string& name);

private:
    std::string mFolderBinary;

    bool mIsMediaTekPlatform;
    bool mEnableProfiling;

    std::vector<cl::Platform> mPlatforms;
    std::vector<cl::Device> mDevices;
    cl::Context mContext;
    cl::CommandQueue mCommandQueue;
    std::map<std::string, cl::Kernel> mKernels;
    std::vector<CLEvent> mEvents;
    std::vector<void*> mSVMs;

    cl::NDRange mOffset;
    cl::NDRange mGlobal;
    cl::NDRange mLocal;
};

}  // namespace gpu
}  // namespace au

#endif  // __CL_WRAPPER_H__

// AURA_NS_WRAPPED
