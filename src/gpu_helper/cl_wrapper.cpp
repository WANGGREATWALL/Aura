#include "cl_wrapper.h"

#include <set>

#include "cv/ximage_algo.h"
#include "file/xfile.h"
#include "log/xlogger.h"
#include "math/xmath.h"
#include "mm/xmemory.h"
#include "perf/xtracer.h"
#include "sys/xsystem.h"
#include "vivo_comdef.h"

namespace au {
namespace gpu {

std::string clErrorInfo(int err)
{
    switch (err) {
        case 0: return std::string("CL_SUCCESS");
        case -1: return std::string("CL_DEVICE_NOT_FOUND");
        case -2: return std::string("CL_DEVICE_NOT_AVAILABLE");
        case -3: return std::string("CL_COMPILER_NOT_AVAILABLE");
        case -4: return std::string("CL_MEM_OBJECT_ALLOCATION_FAILURE");
        case -5: return std::string("CL_OUT_OF_RESOURCES");
        case -6: return std::string("CL_OUT_OF_HOST_MEMORY");
        case -7: return std::string("CL_PROFILING_INFO_NOT_AVAILABLE");
        case -8: return std::string("CL_MEM_COPY_OVERLAP");
        case -9: return std::string("CL_IMAGE_FORMAT_MISMATCH");
        case -10: return std::string("CL_IMAGE_FORMAT_NOT_SUPPORTED");
        case -11: return std::string("CL_BUILD_PROGRAM_FAILURE");
        case -12: return std::string("CL_MAP_FAILURE");
        case -13: return std::string("CL_MISALIGNED_SUB_BUFFER_OFFSET");
        case -14: return std::string("CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST");
        case -15: return std::string("CL_COMPILE_PROGRAM_FAILURE");
        case -16: return std::string("CL_LINKER_NOT_AVAILABLE");
        case -17: return std::string("CL_LINK_PROGRAM_FAILURE");
        case -18: return std::string("CL_DEVICE_PARTITION_FAILED");
        case -19: return std::string("CL_KERNEL_ARG_INFO_NOT_AVAILABLE");
        case -30: return std::string("CL_INVALID_VALUE");
        case -31: return std::string("CL_INVALID_DEVICE_TYPE");
        case -32: return std::string("CL_INVALID_PLATFORM");
        case -33: return std::string("CL_INVALID_DEVICE");
        case -34: return std::string("CL_INVALID_CONTEXT");
        case -35: return std::string("CL_INVALID_QUEUE_PROPERTIES");
        case -36: return std::string("CL_INVALID_COMMAND_QUEUE");
        case -37: return std::string("CL_INVALID_HOST_PTR");
        case -38: return std::string("CL_INVALID_MEM_OBJECT");
        case -39: return std::string("CL_INVALID_IMAGE_FORMAT_DESCRIPTOR");
        case -40: return std::string("CL_INVALID_IMAGE_SIZE");
        case -41: return std::string("CL_INVALID_SAMPLER");
        case -42: return std::string("CL_INVALID_BINARY");
        case -43: return std::string("CL_INVALID_BUILD_OPTIONS");
        case -44: return std::string("CL_INVALID_PROGRAM");
        case -45: return std::string("CL_INVALID_PROGRAM_EXECUTABLE");
        case -46: return std::string("CL_INVALID_KERNEL_NAME");
        case -47: return std::string("CL_INVALID_KERNEL_DEFINITION");
        case -48: return std::string("CL_INVALID_KERNEL");
        case -49: return std::string("CL_INVALID_ARG_INDEX");
        case -50: return std::string("CL_INVALID_ARG_VALUE");
        case -51: return std::string("CL_INVALID_ARG_SIZE");
        case -52: return std::string("CL_INVALID_KERNEL_ARGS");
        case -53: return std::string("CL_INVALID_WORK_DIMENSION");
        case -54: return std::string("CL_INVALID_WORK_GROUP_SIZE");
        case -55: return std::string("CL_INVALID_WORK_ITEM_SIZE");
        case -56: return std::string("CL_INVALID_GLOBAL_OFFSET");
        case -57: return std::string("CL_INVALID_EVENT_WAIT_LIST");
        case -58: return std::string("CL_INVALID_EVENT");
        case -59: return std::string("CL_INVALID_OPERATION");
        case -60: return std::string("CL_INVALID_GL_OBJECT");
        case -61: return std::string("CL_INVALID_BUFFER_SIZE");
        case -62: return std::string("CL_INVALID_MIP_LEVEL");
        case -63: return std::string("CL_INVALID_GLOBAL_WORK_SIZE");
        case -64: return std::string("CL_INVALID_PROPERTY");
        case -65: return std::string("CL_INVALID_IMAGE_DESCRIPTOR");
        case -66: return std::string("CL_INVALID_COMPILER_OPTIONS");
        case -67: return std::string("CL_INVALID_LINKER_OPTIONS");
        case -68: return std::string("CL_INVALID_DEVICE_PARTITION_COUNT");
        case -69: return std::string("CL_INVALID_PIPE_SIZE");
        case -70: return std::string("CL_INVALID_DEVICE_QUEUE");
    }
    return std::string("CL_UNKNOWN");
}

std::map<cl_channel_order, std::string> mapChannelOrder = {{CL_R, "CL_R"},
                                                           {CL_A, "CL_A"},
                                                           {CL_RG, "CL_RG"},
                                                           {CL_RA, "CL_RA"},
                                                           {CL_RGB, "CL_RGB"},
                                                           {CL_RGBA, "CL_RGBA"},
                                                           {CL_BGRA, "CL_BGRA"},
                                                           {CL_ARGB, "CL_ARGB"},
                                                           {CL_INTENSITY, "CL_INTENSITY"},
                                                           {CL_LUMINANCE, "CL_LUMINANCE"}};

std::map<cl_channel_type, std::string> mapChannelDataType = {{CL_SNORM_INT8, "CL_SNORM_INT8"},
                                                             {CL_SNORM_INT16, "CL_SNORM_INT16"},
                                                             {CL_UNORM_INT8, "CL_UNORM_INT8"},
                                                             {CL_UNORM_INT16, "CL_UNORM_INT16"},
                                                             {CL_UNORM_SHORT_565, "CL_UNORM_SHORT_565"},
                                                             {CL_UNORM_SHORT_555, "CL_UNORM_SHORT_555"},
                                                             {CL_UNORM_INT_101010, "CL_UNORM_INT_101010"},
                                                             {CL_SIGNED_INT8, "CL_SIGNED_INT8"},
                                                             {CL_SIGNED_INT16, "CL_SIGNED_INT16"},
                                                             {CL_SIGNED_INT32, "CL_SIGNED_INT32"},
                                                             {CL_UNSIGNED_INT8, "CL_UNSIGNED_INT8"},
                                                             {CL_UNSIGNED_INT16, "CL_UNSIGNED_INT16"},
                                                             {CL_UNSIGNED_INT32, "CL_UNSIGNED_INT32"},
                                                             {CL_HALF_FLOAT, "CL_HALF_FLOAT"},
                                                             {CL_FLOAT, "CL_FLOAT"}};

int getChannelBitWidth(const cl_channel_type& typeChannel)
{
    switch (typeChannel) {
        case CL_SNORM_INT8:
        case CL_UNORM_INT8:
        case CL_SIGNED_INT8:
        case CL_UNSIGNED_INT8: return 8;
        case CL_SNORM_INT16:
        case CL_UNORM_INT16:
        case CL_SIGNED_INT16:
        case CL_UNSIGNED_INT16:
        case CL_HALF_FLOAT: return 16;
        case CL_SIGNED_INT32:
        case CL_UNSIGNED_INT32:
        case CL_FLOAT: return 32;
        default: return -1;
    }
}

size_t getElementSize(const cl::ImageFormat& format)
{
    size_t sizePixel = 0;
    switch (format.image_channel_order) {
        case CL_R:
        case CL_A:
        case CL_INTENSITY:
        case CL_LUMINANCE: sizePixel = 1; break;
        case CL_RG:
        case CL_RA: sizePixel = 2; break;
        case CL_RGB: sizePixel = 3; break;
        case CL_RGBA:
        case CL_BGRA:
        case CL_ARGB:
        case CL_ABGR: sizePixel = 4; break;
        default: return 0;
    }

    switch (format.image_channel_data_type) {
        case CL_SNORM_INT8:
        case CL_UNORM_INT8:
        case CL_SIGNED_INT8:
        case CL_UNSIGNED_INT8: break;
        case CL_SNORM_INT16:
        case CL_UNORM_INT16:
        case CL_SIGNED_INT16:
        case CL_UNSIGNED_INT16:
        case CL_HALF_FLOAT: sizePixel *= 2; break;
        case CL_SIGNED_INT32:
        case CL_UNSIGNED_INT32:
        case CL_FLOAT: sizePixel *= 4; break;
        default: return 0;
    }

    return sizePixel;
}

int convertCLImageFormat2NTIFormat(const cl_image_format& fmtCL, int& fmtCV)
{
    int bit = getChannelBitWidth(fmtCL.image_channel_data_type);
    XCHECK_WITH_RET(bit > 0, VDKResultEUnsupported);

    if (fmtCL.image_channel_order == CL_R) {
        switch (bit) {
            case 8: fmtCV = NTI_GRAY; break;
            case 16: fmtCV = NTI_GRAY_U16; break;
            case 32: fmtCV = NTI_GRAY_U32; break;
            default: return VDKResultEUnsupported;
        }
    } else if (fmtCL.image_channel_order == CL_RG) {
        switch (bit) {
            case 8: fmtCV = NTI_UV; break;
            case 16: fmtCV = NTI_UV_U16; break;
            case 32: fmtCV = NTI_UV_F32; break;
            default: return VDKResultEUnsupported;
        }
    } else if (fmtCL.image_channel_order == CL_RGB) {
        switch (bit) {
            case 8: fmtCV = NTI_R8G8B8; break;
            case 16: fmtCV = NTI_RGB_U16; break;
            case 32: fmtCV = NTI_R32G32B32; break;
            default: return VDKResultEUnsupported;
        }
    } else if (fmtCL.image_channel_order == CL_RGBA || fmtCL.image_channel_order == CL_BGRA) {
        switch (bit) {
            case 8: fmtCV = NTI_R8G8B8A8; break;
            case 32: fmtCV = NTI_R32G32B32A32; break;
            default: return VDKResultEUnsupported;
        }
    } else {
        fmtCV = 0;
        return VDKResultEUnsupported;
    }

    return VDKResultSuccess;
}

size_t getImageWidth(const cl::Image& image)
{
    int           ret   = CL_SUCCESS;
    cl::size_type width = image.getImageInfo<CL_IMAGE_WIDTH>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, 0, "clError: %s", clErrorInfo(ret).c_str());

    return width;
}

size_t getImageHeight(const cl::Image& image)
{
    int           ret    = CL_SUCCESS;
    cl::size_type height = image.getImageInfo<CL_IMAGE_HEIGHT>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, 0, "clError: %s", clErrorInfo(ret).c_str());

    return height;
}

size_t getImagePitch(const cl::Image& image)
{
    cl_int        ret   = CL_SUCCESS;
    cl::size_type pitch = image.getImageInfo<CL_IMAGE_ROW_PITCH>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, 0, "clError: %s", clErrorInfo(ret).c_str());

    return pitch;
}

size_t getImageDepth(const cl::Image& image)
{
    cl_int        ret   = CL_SUCCESS;
    cl::size_type depth = image.getImageInfo<CL_IMAGE_DEPTH>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, 0, "clError: %s", clErrorInfo(ret).c_str());

    return depth;
}

bool isValid(const cl::Image& image)
{
    return getImageWidth(image) > 0 && getImageHeight(image) > 0;
}

bool isFormat(const cl::Image& image, const cl_image_format& format)
{
    int             ret = CL_SUCCESS;
    cl_image_format fmt = image.getImageInfo<CL_IMAGE_FORMAT>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, false, "clError: %s", clErrorInfo(ret).c_str());

    return fmt.image_channel_order == format.image_channel_order &&
           fmt.image_channel_data_type == format.image_channel_data_type;
}

bool isSize(const cl::Image& image, size_t width, size_t height, size_t depth)
{
    return getImageWidth(image) == width && getImageHeight(image) == height && getImageDepth(image) == depth;
}

bool isSameSize(const cl::Image& image0, const cl::Image& image1)
{
    return getImageWidth(image0) == getImageWidth(image1) && getImageHeight(image0) == getImageHeight(image1) &&
           getImageDepth(image0) == getImageDepth(image1);
}

bool isSameFormat(const cl::Image& image0, const cl::Image& image1)
{
    int             ret  = CL_SUCCESS;
    cl_image_format fmt0 = image0.getImageInfo<CL_IMAGE_FORMAT>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, false, "clError: %s", clErrorInfo(ret).c_str());

    cl_image_format fmt1 = image1.getImageInfo<CL_IMAGE_FORMAT>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, false, "clError: %s", clErrorInfo(ret).c_str());

    return fmt0.image_channel_order == fmt1.image_channel_order &&
           fmt0.image_channel_data_type == fmt1.image_channel_data_type;
}

bool isSameSizeAndFormat(const cl::Image& image0, const cl::Image& image1)
{
    return isSameSize(image0, image1) && isSameFormat(image0, image1);
}

std::string info(const cl::Image& image)
{
    char str[256];

    int             ret = CL_SUCCESS;
    cl_image_format fmt = image.getImageInfo<CL_IMAGE_FORMAT>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, "unknown", "clError: %s", clErrorInfo(ret).c_str());

    snprintf(str, 256, "[%zux%zux%zu], fmt:{%s, %s}", getImageWidth(image), getImageHeight(image), getImageDepth(image),
             mapChannelOrder[fmt.image_channel_order].c_str(), mapChannelDataType[fmt.image_channel_data_type].c_str());

    return std::string(str);
}

CLWrapper::CLWrapper() {}

CLWrapper::~CLWrapper()
{
    XCHECK(!mCommandQueue() && "CLWrapper need to deinit!");
    clearAndSyncEvents();
}

bool CLWrapper::available() const
{
    return !mPlatforms.empty() && !mDevices.empty() && mContext() && mCommandQueue();
}

void printf_callback(const char* buffer, size_t len /*, size_t complete, void* user_data*/)
{
    printf("%.*s", (int)len, buffer);
}

int CLWrapper::init(std::string folderBinary, bool enableProfiling)
{
    perf::XTracerScoped trace("CLWrapper::init");

    if (mCommandQueue()) {
        XLOG_W("CLWrapper(%s, %s) reset to (%s, %s)!\n", mFolderBinary.c_str(), mEnableProfiling ? "true" : "false",
               folderBinary.c_str(), enableProfiling ? "true" : "false");

        deinit();
    }

    mFolderBinary = file::XPath(folderBinary + "/").parent().string();

    XCHECK_WITH_RET(sys::isMediaTekPlatform() ^ sys::isQualcommPlatform(), VDKResultEBadState);
    mIsMediaTekPlatform = sys::isMediaTekPlatform();

    XCHECK_WITH_RET(!mFolderBinary.empty(), VDKResultEInvalidParam);
    XCHECK_WITH_RET(file::existDir(mFolderBinary), VDKResultEInvalidParam);

    int retGetPlatform = getPlatform();
    XCHECK_WITH_MSG(retGetPlatform == CL_SUCCESS, retGetPlatform, "clError: %s", clErrorInfo(retGetPlatform).c_str());

    int retGetDevice = getDevice();
    XCHECK_WITH_MSG(retGetDevice == CL_SUCCESS, retGetDevice, "clError: %s", clErrorInfo(retGetDevice).c_str());

    cl_context_properties properties[] = {
        /* Enable a printf callback function for this context. */
        CL_PRINTF_CALLBACK_ARM, (cl_context_properties)printf_callback,

        /* Request a minimum printf buffer size of 4MiB for devices in the context that support this extension. */
        CL_PRINTF_BUFFERSIZE_ARM, (cl_context_properties)0x100000,
        // CL_CONTEXT_PLATFORM, (cl_context_properties)mPlatforms.front()(),
        0};

    int retCreateContext = mIsMediaTekPlatform ? createContext(properties) : createContext();
    XCHECK_WITH_MSG(retCreateContext == CL_SUCCESS, retCreateContext, "clError: %s",
                    clErrorInfo(retCreateContext).c_str());

    int retCreateCommandQueue =
        createCommandQueue((mEnableProfiling = enableProfiling) ? CL_QUEUE_PROFILING_ENABLE : 0);
    XCHECK_WITH_MSG(retCreateCommandQueue == CL_SUCCESS, retCreateCommandQueue, "clError: %s",
                    clErrorInfo(retCreateCommandQueue).c_str());

    return VDKResultSuccess;
}

int CLWrapper::deinit()
{
    perf::XTracerScoped trace("CLWrapper::deinit");

    mLocal  = cl::NDRange();
    mGlobal = cl::NDRange();
    mOffset = cl::NDRange();

    mSVMs.clear();
    mEvents.clear();
    mKernels.clear();

    mCommandQueue = cl::CommandQueue();
    mContext      = cl::Context();

    mDevices.clear();
    mPlatforms.clear();

    return VDKResultSuccess;
}

int CLWrapper::createBuffer(cl::Buffer& dst, const VImage& image, cl_mem_flags flags)
{
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);
    XCHECK_WITH_RET(cv::isValid(image), VDKResultEInvalidParam);
    XCHECK_WITH_RET(cv::isFormatIn(image, {NTI_GRAY, NTI_GRAY_U16, NTI_GRAY_U32, NTI_R8G8B8, NTI_RGB_U16, NTI_R32G32B32,
                                         NTI_R8G8B8A8, NTI_R32G32B32A32}),
                    VDKResultEUnsupported);

    int ret = CL_SUCCESS;
    dst     = cl::Buffer(mContext, flags, image.height * image.stride[0], image.data[0], &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createBuffer(cl::Buffer& dst, void* data, size_t sizeInByte, cl_mem_flags flags)
{
    XCHECK_WITH_RET(data != nullptr, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Buffer(mContext, flags, sizeInByte, data, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createBufferFromDMAImage(cl::Buffer& dst, const cv::XImage& src)
{
    XCHECK_WITH_RET(src.isValid(), VDKResultEInvalidParam);
    XCHECK_WITH_RET(src.fd[0] > 0, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int retImport = CL_SUCCESS;
    cl_mem handleCLBuffer = nullptr;

    if (mIsMediaTekPlatform) {
        static clImportMemoryARM_fn pfnImport =
            !mPlatforms.empty() ? reinterpret_cast<clImportMemoryARM_fn>(clGetExtensionFunctionAddressForPlatform(
                                     mPlatforms.front()(), "clImportMemoryARM"))
                                : nullptr;
        XCHECK_WITH_RET(pfnImport, VDKResultEBadState);

        cl_import_properties_arm mem_properties[] = {CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
                                                      CL_IMPORT_DMA_BUF_DATA_CONSISTENCY_WITH_HOST_ARM, CL_FALSE, 0};

        int filedesc = src.fd[0];
        handleCLBuffer =
            pfnImport(mContext(), CL_MEM_READ_WRITE, mem_properties, &filedesc, src.stride[0] * src.height, &retImport);
    } else {
        cl_mem_ion_host_ptr ionmem        = {{0}};
        ionmem.ext_host_ptr.allocation_type   = CL_MEM_ION_HOST_PTR_QCOM;
        ionmem.ext_host_ptr.host_cache_policy = CL_MEM_HOST_IOCOHERENT_QCOM;
        ionmem.ion_filedesc                    = src.fd[0];
        ionmem.ion_hostptr                     = src.data[0];
        handleCLBuffer = clCreateBuffer(mContext(), CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM,
                                        src.stride[0] * src.height, &ionmem, &retImport);
    }
    XCHECK_WITH_RET(handleCLBuffer, VDKResultEBadState);
    XCHECK_WITH_MSG(retImport == CL_SUCCESS, VDKResultEBadState, "clError: %s", clErrorInfo(retImport).c_str());

    dst = cl::Buffer(handleCLBuffer, false);

    return VDKResultSuccess;
}

int CLWrapper::createImage2DFromCLBuffer(
    cl::Image2D& dst, const cl::Buffer& src, int width, int height, int stride, cl::ImageFormat format)
{
    XCHECK_WITH_RET(src(), VDKResultEInvalidParam);
    XCHECK_WITH_RET(width > 0 && height > 0, VDKResultEInvalidParam);
    XCHECK_WITH_RET(stride == queryImageRowPitch(width, height, format), VDKResultEUnsupported);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    // create image description
    cl_image_desc desc = {0};
    desc.image_type      = CL_MEM_OBJECT_IMAGE2D;
    desc.image_width     = width;
    desc.image_height    = height;
    desc.image_row_pitch = stride;
    desc.buffer          = src();

    // create cl image using description
    cl_int err = CL_SUCCESS;
    auto handleCLImage = clCreateImage(mContext(), CL_MEM_READ_WRITE, &format, &desc, nullptr, &err);
    XCHECK_WITH_MSG(err == CL_SUCCESS, VDKResultEBadState, "clError: %s", clErrorInfo(err).c_str());

    dst = cl::Image2D(handleCLImage, false);

    return VDKResultSuccess;
}

int CLWrapper::createImage2D(cl::Image2D& dst, const VImage& image, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(cv::isValid(image), VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Image2D(mContext, flags, format, image.width, image.height, image.stride[0], image.data[0], &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createImage2D(
    cl::Image2D& dst, void* data, int width, int height, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);
    XCHECK_WITH_RET(data != nullptr, VDKResultEInvalidParam);
    XCHECK_WITH_RET(width > 0 && height > 0, VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    dst     = cl::Image2D(mContext, flags, format, (cl::size_type)width, (cl::size_type)height, 0, data, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createImage2D(cl::Image2D& dst, int width, int height, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(width > 0 && height > 0, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Image2D(mContext, flags, format, width, height, 0, nullptr, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createImage3D(cl::Image3D& dst, const VImage& image, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(cv::isValid(image), VDKResultEInvalidParam);
    XCHECK_WITH_RET(cv::isFormatIn(image, {NTI_GRAY, NTI_GRAY_U16, NTI_GRAY_U32, NTI_R8G8B8, NTI_RGB_U16, NTI_R32G32B32,
                                         NTI_R8G8B8A8, NTI_R32G32B32A32}),
                    VDKResultEUnsupported);
    XCHECK_WITH_RET(image.width * image.width == image.height, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Image3D(mContext, flags, format, image.width, image.width, image.width, 0, 0, image.data[0], &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createImage3D(
    cl::Image3D& dst, void* data, int width, int height, int depth, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(data != nullptr, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Image3D(mContext, flags, format, width, height, depth, 0, 0, data, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::createImage3D(
    cl::Image3D& dst, int width, int height, int depth, cl::ImageFormat format, cl_mem_flags flags)
{
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    dst     = cl::Image3D(mContext, flags, format, width, height, depth, 0, 0, nullptr, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return VDKResultSuccess;
}

int CLWrapper::copyImage2D(cl::Image2D& dst, const cl::Image2D& src)
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    XCHECK_WITH_RET(isValid(src), VDKResultEInvalidParam);
    XCHECK_WITH_RET(isSameSizeAndFormat(src, dst), VDKResultEInvalidParam);

    auto width  = getImageWidth(src);
    auto height = getImageHeight(src);

    cl::Event event;
    int retCopyImage = mCommandQueue.enqueueCopyImage(src, dst, {0, 0, 0}, {0, 0, 0}, {width, height, 1}, 0, &event);
    XCHECK_WITH_MSG(retCopyImage == CL_SUCCESS, retCopyImage, "clError: %s", clErrorInfo(retCopyImage).c_str());

    mEvents.emplace_back("copyImage2D", event);

    return VDKResultSuccess;
}

int CLWrapper::readImage2D(VImage& dst, const cl::Image2D& image, bool block)
{
    perf::XTracerScoped trace("CLWrapper::readImage2D");

    XCHECK_WITH_RET(cv::isValid(dst), VDKResultEInvalidParam);
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);

    cl::Event event;
    int retReadImage = mCommandQueue.enqueueReadImage(image, block, {0, 0, 0},
                                                     {(cl::size_type)dst.width, (cl::size_type)dst.height, 1},
                                                     dst.stride[0], 0, dst.data[0], 0, &event);
    XCHECK_WITH_MSG(retReadImage == CL_SUCCESS, retReadImage, "clError: %s", clErrorInfo(retReadImage).c_str());

    mEvents.emplace_back("readImage2D", event);

    return VDKResultSuccess;
}

int CLWrapper::readImage2D(VImage& dst, const cl::Image2D& plane0, const cl::Image2D& plane1)
{
    perf::XTracerScoped trace("CLWrapper::readImage2D");

    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    XCHECK_WITH_RET(cv::isValid(dst), VDKResultEInvalidParam);
    XCHECK_WITH_RET(cv::isFormatIn(dst, {kVIFormatNV12, kVIFormatNV21}), VDKResultEUnsupported);

    trace.sub("query");
    cl::size_type pitch0  = getImagePitch(plane0);
    cl::size_type pitch1  = getImagePitch(plane1);
    cl::size_type width0  = getImageWidth(plane0);
    cl::size_type width1  = getImageWidth(plane1);
    cl::size_type height0 = getImageHeight(plane0);
    cl::size_type height1 = getImageHeight(plane1);

    XCHECK_WITH_RET(pitch0 == pitch1, VDKResultEInvalidParam);
    XCHECK_WITH_RET(width0 == width1 * 2, VDKResultEInvalidParam);
    XCHECK_WITH_RET(height0 == height1 * 2, VDKResultEInvalidParam);

    trace.sub("read");
    cl::Event event0;
    int retReadImage0 =
        mCommandQueue.enqueueReadImage(plane0, false, {0, 0, 0}, {(cl::size_type)width0, (cl::size_type)height0, 1},
                                       pitch0, 0, dst.data[0], 0, &event0);
    XCHECK_WITH_MSG(retReadImage0 == CL_SUCCESS, retReadImage0, "clError: %s", clErrorInfo(retReadImage0).c_str());
    mEvents.emplace_back("readImage2DP0", event0);

    cl::Event event1;
    int retReadImage1 =
        mCommandQueue.enqueueReadImage(plane1, true, {0, 0, 0}, {(cl::size_type)width1, (cl::size_type)height1, 1},
                                       pitch1, 0, dst.data[1], 0, &event1);
    XCHECK_WITH_MSG(retReadImage1 == CL_SUCCESS, retReadImage1, "clError: %s", clErrorInfo(retReadImage1).c_str());
    mEvents.emplace_back("readImage2DP1", event1);

    return VDKResultSuccess;
}

int CLWrapper::readImage2D(void* dst, const cl::Image2D& image, int width, int height, int pitch, bool block)
{
    perf::XTracerScoped trace("CLWrapper::readImage2D");

    XCHECK_WITH_RET(dst != nullptr, VDKResultEInvalidParam);
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);

    cl::Event event;
    int retReadImage = mCommandQueue.enqueueReadImage(
        image, block, {0, 0, 0}, {(cl::size_type)width, (cl::size_type)height, 1}, pitch, 0, dst, 0, &event);
    XCHECK_WITH_MSG(retReadImage == CL_SUCCESS, retReadImage, "clError: %s", clErrorInfo(retReadImage).c_str());

    mEvents.emplace_back("readImage2D", event);

    return VDKResultSuccess;
}

int CLWrapper::mapImage2D(VImage& dst, const cl::Image2D& image)
{
    perf::XTracerScoped trace("CLWrapper::mapImage2D");

    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    XCHECK_WITH_RET(cv::isValid(dst), VDKResultEInvalidParam);
    XCHECK_WITH_RET(cv::isFormatIn(dst, {NTI_GRAY, NTI_R8G8B8, NTI_R8G8B8A8}), VDKResultEInvalidParam);

    cl_int ret = CL_SUCCESS;

    trace.sub("query");
    cl::size_type pitch  = getImagePitch(image);
    cl::size_type width  = getImageWidth(image);
    cl::size_type height = getImageHeight(image);

    XCHECK_WITH_RET(dst.width == width, VDKResultEInvalidParam);
    XCHECK_WITH_RET(dst.height == height, VDKResultEInvalidParam);
    XCHECK_WITH_RET(dst.stride[0] == pitch, VDKResultEInvalidParam);

    trace.sub("map");
    cl::Event event;
    void* data = mCommandQueue.enqueueMapImage(image, CL_TRUE, CL_MAP_READ, {0, 0, 0}, {width, height, 1}, &pitch, 0, 0,
                                               &event, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());
    mEvents.emplace_back("mapImage", event);

    trace.sub("copy");
    for (int i = 0; i < dst.height; ++i) {
        auto dataSrc = (uint8_t*)data + pitch * i;
        auto dataDst = cv::dataptr<uint8_t>(dst, cv::Plane0, i);
        memcpy(dataDst, dataSrc, dst.stride[0]);
    }

    trace.sub("unmap");
    auto retUnmapImage = mCommandQueue.enqueueUnmapMemObject(image, data, 0, &event);
    XCHECK_WITH_MSG(retUnmapImage == CL_SUCCESS, retUnmapImage, "clError: %s", clErrorInfo(retUnmapImage).c_str());
    mEvents.emplace_back("unmapImage", event);

    return VDKResultSuccess;
}

int CLWrapper::mapImage2D(VImage& dst, const cl::Image2D& plane0, const cl::Image2D& plane1)
{
    perf::XTracerScoped trace("CLWrapper::mapImage2D");

    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    XCHECK_WITH_RET(cv::isValid(dst), VDKResultEInvalidParam);
    XCHECK_WITH_RET(cv::isFormatIn(dst, {NTI_NV12, NTI_NV21}), VDKResultEInvalidParam);

    cl_int ret = CL_SUCCESS;

    trace.sub("query");
    cl::size_type pitch0  = getImagePitch(plane0);
    cl::size_type pitch1  = getImagePitch(plane1);
    cl::size_type width0  = getImageWidth(plane0);
    cl::size_type width1  = getImageWidth(plane1);
    cl::size_type height0 = getImageHeight(plane0);
    cl::size_type height1 = getImageHeight(plane1);

    XCHECK_WITH_RET(pitch0 == pitch1, VDKResultEInvalidParam);
    XCHECK_WITH_RET(width0 == width1 * 2, VDKResultEInvalidParam);
    XCHECK_WITH_RET(height0 == height1 * 2, VDKResultEInvalidParam);
    XCHECK_WITH_RET(dst.width == width0 && dst.height == height0 && dst.stride[0] == pitch0, VDKResultEInvalidParam);

    trace.sub("map");
    cl::Event event;
    void* data0 = mCommandQueue.enqueueMapImage(plane0, false, CL_MAP_READ | CL_MAP_WRITE, {0, 0, 0},
                                                {width0, height0, 1}, &pitch0, 0, 0, &event, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());
    mEvents.emplace_back("mapImageP0", event);

    void* data1 = mCommandQueue.enqueueMapImage(plane1, true, CL_MAP_READ | CL_MAP_WRITE, {0, 0, 0},
                                                {width1, height1, 1}, &pitch1, 0, 0, &event, &ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());
    mEvents.emplace_back("mapImageP1", event);

    trace.sub("copy");
    memcpy(dst.data[0], data0, dst.stride[0] * dst.height);
    memcpy(dst.data[1], data1, dst.stride[1] * dst.height >> 1);

    trace.sub("unmap");
    auto retUnmapImage0 = mCommandQueue.enqueueUnmapMemObject(plane0, data0, 0, &event);
    XCHECK_WITH_MSG(retUnmapImage0 == CL_SUCCESS, retUnmapImage0, "clError: %s", clErrorInfo(retUnmapImage0).c_str());
    mEvents.emplace_back("unmapImageP0", event);

    auto retUnmapImage1 = mCommandQueue.enqueueUnmapMemObject(plane1, data0, 0, &event);
    XCHECK_WITH_MSG(retUnmapImage1 == CL_SUCCESS, retUnmapImage1, "clError: %s", clErrorInfo(retUnmapImage1).c_str());
    mEvents.emplace_back("unmapImageP1", event);

    return VDKResultSuccess;
}

void* CLWrapper::mallocSVM(size_t sizeInByte, size_t align, cl_mem_flags flags)
{
    XCHECK(mContext());

    int ret = CL_SUCCESS;
    cl_device_svm_capabilities validSVM = mDevices.front().getInfo<CL_DEVICE_SVM_CAPABILITIES>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS && (validSVM & CL_DEVICE_SVM_FINE_GRAIN_BUFFER), nullptr, "clError: %s",
                    clErrorInfo(ret).c_str());

    void* data = clSVMAlloc(mContext(), flags, sizeInByte, align);
    XCHECK_WITH_RET(data != nullptr, nullptr);

    return data;
}

int CLWrapper::freeSVM(void* data)
{
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);
    XCHECK_WITH_RET(data != nullptr, VDKResultEInvalidParam);
    clSVMFree(mContext(), data);
    data = nullptr;
    return VDKResultSuccess;
}

CLWrapper& CLWrapper::setNDRange(cl::NDRange global, cl::NDRange local, cl::NDRange offset)
{
    XCHECK(global.dimensions() > 0);
    XCHECK(global.dimensions() == local.dimensions());
    XCHECK(global.dimensions() == offset.dimensions());

    size_t g[3] = {1, 1, 1};
    size_t l[3] = {1, 1, 1};

    for (size_t i = 0; i < global.dimensions(); ++i) {
        l[i] = (local[i] > 0) ? local[i] : 1;
        g[i] = ((global[i] + l[i] - 1) / l[i]) * l[i];
    }

    if (global.dimensions() == 1) {
        mGlobal = cl::NDRange(g[0]);
        mLocal  = cl::NDRange(l[0]);
    } else if (global.dimensions() == 2) {
        mGlobal = cl::NDRange(g[0], g[1]);
        mLocal  = cl::NDRange(l[0], l[1]);
    } else {
        mGlobal = cl::NDRange(g[0], g[1], g[2]);
        mLocal  = cl::NDRange(l[0], l[1], l[2]);
    }

    mOffset = offset;

    return *this;
}

int CLWrapper::flush()
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    int retFlush = mCommandQueue.flush();
    XCHECK_WITH_MSG(retFlush == CL_SUCCESS, retFlush, "clError: %s", clErrorInfo(retFlush).c_str());
    return VDKResultSuccess;
}

int CLWrapper::finish()
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    int retFinish = mCommandQueue.finish();
    XCHECK_WITH_MSG(retFinish == CL_SUCCESS, retFinish, "clError: %s", clErrorInfo(retFinish).c_str());

    // profiling event
    int retClearAndSyncEvents = clearAndSyncEvents();
    XCHECK_WITH_RET(retClearAndSyncEvents == VDKResultSuccess, retClearAndSyncEvents);

    return VDKResultSuccess;
}

int CLWrapper::wait(const cl::Event& event)
{
    int retEventWait = event.wait();
    XCHECK_WITH_MSG(retEventWait == CL_SUCCESS, retEventWait, "clError: %s", clErrorInfo(retEventWait).c_str());
    return VDKResultSuccess;
}

int CLWrapper::barrier()
{
    XCHECK_WITH_RET(mCommandQueue(), VDKResultEBadState);
    int retBarrier = mCommandQueue.enqueueBarrierWithWaitList();
    XCHECK_WITH_MSG(retBarrier == CL_SUCCESS, retBarrier, "clError: %s", clErrorInfo(retBarrier).c_str());
    return VDKResultSuccess;
}

int CLWrapper::clearAndSyncEvents()
{
    if (mEnableProfiling) {
        int ret = CL_SUCCESS;
        for (const auto& e : mEvents) {
            double enqueue = e.event.getProfilingInfo<CL_PROFILING_COMMAND_QUEUED>(&ret) / 1000000.0;
            XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

            double submit = e.event.getProfilingInfo<CL_PROFILING_COMMAND_SUBMIT>(&ret) / 1000000.0;
            XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

            double start = e.event.getProfilingInfo<CL_PROFILING_COMMAND_START>(&ret) / 1000000.0;
            XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

            double end = e.event.getProfilingInfo<CL_PROFILING_COMMAND_END>(&ret) / 1000000.0;
            XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

            XLOG_I("cl profiling => (enqueue)%.3f, (:submit)%.3f, (:start)%.3f, (:end)%.3f ms => [%s]\n", enqueue,
                   (submit - enqueue), (start - submit), (end - start), e.name.c_str());
        }
    }
    mEvents.clear();

    return VDKResultSuccess;
}

bool CLWrapper::isImageFormatSupported(const cl_channel_order& order, const cl_channel_type& type) const
{
    XCHECK_WITH_RET(mContext(), false);

    std::vector<cl::ImageFormat> formats;
    int retGetSupportedFormats = mContext.getSupportedImageFormats(CL_MEM_READ_WRITE, CL_MEM_OBJECT_IMAGE2D, &formats);
    XCHECK_WITH_MSG(retGetSupportedFormats == CL_SUCCESS, false, "clError: %s",
                    clErrorInfo(retGetSupportedFormats).c_str());

    for (const auto& format : formats) {
        if (format.image_channel_order == order && format.image_channel_data_type == type) {
            return true;
        }
    }

    return false;
}

int CLWrapper::querySupportedImageFormats(const cl_mem_object_type object, const cl_mem_flags flags) const
{
    XCHECK_WITH_RET(mContext(), VDKResultEBadState);

    std::vector<cl::ImageFormat> formats;
    int retGetSupportedFormats = mContext.getSupportedImageFormats(flags, CL_MEM_OBJECT_IMAGE2D, &formats);
    XCHECK_WITH_MSG(retGetSupportedFormats == CL_SUCCESS, retGetSupportedFormats, "clError: %s",
                    clErrorInfo(retGetSupportedFormats).c_str());

    std::set<std::string> listChannelOrder;
    std::set<std::string> listChannelDataType;

    for (const auto& format : formats) {
        auto orderIt = mapChannelOrder.find(format.image_channel_order);
        if (orderIt != mapChannelOrder.end()) {
            listChannelOrder.insert(orderIt->second);
        }

        auto dataTypeIt = mapChannelDataType.find(format.image_channel_data_type);
        if (dataTypeIt != mapChannelDataType.end()) {
            listChannelDataType.insert(dataTypeIt->second);
        }
    }

    XLOG_I("OpenCL supported channel order:\n");
    for (const auto& order : listChannelOrder) {
        XLOG_I("    %s\n", order.c_str());
    }

    XLOG_I("OpenCL supported channel data type:\n");
    for (const auto& type : listChannelDataType) {
        XLOG_I("    %s\n", type.c_str());
    }

    return VDKResultSuccess;
}

int CLWrapper::querySVMCapabilities() const
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEBadState);

    int ret = CL_SUCCESS;
    cl_device_svm_capabilities capsSVM = mDevices.front().getInfo<CL_DEVICE_SVM_CAPABILITIES>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    if (capsSVM & CL_DEVICE_SVM_COARSE_GRAIN_BUFFER) {
        XLOG_I("OpenCL supported SVM type: %s\n", "CL_DEVICE_SVM_COARSE_GRAIN_BUFFER");
    }
    if (capsSVM & CL_DEVICE_SVM_FINE_GRAIN_BUFFER) {
        XLOG_I("OpenCL supported SVM type: %s\n", "CL_DEVICE_SVM_FINE_GRAIN_BUFFER");
    }
    if (capsSVM & CL_DEVICE_SVM_FINE_GRAIN_SYSTEM) {
        XLOG_I("OpenCL supported SVM type: %s\n", "CL_DEVICE_SVM_FINE_GRAIN_SYSTEM");
    }
    if (capsSVM & CL_DEVICE_SVM_ATOMICS) {
        XLOG_I("OpenCL supported SVM type: %s\n", "CL_DEVICE_SVM_ATOMICS");
    }

    return VDKResultSuccess;
}

int CLWrapper::queryPrintfSupport() const
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEBadState);

    int retGetInfo = CL_SUCCESS;
    std::string extensions = mDevices.front().getInfo<CL_DEVICE_EXTENSIONS>(&retGetInfo);
    XCHECK_WITH_MSG(retGetInfo == CL_SUCCESS, retGetInfo, "clError: %s", clErrorInfo(retGetInfo).c_str());

    bool hasPrintfExtension = (extensions.find("printf") != std::string::npos);

    // cl_ulong printfBufferSize = mDevices.front().getInfo<CL_DEVICE_PRINTF_BUFFER_SIZE>();

    XLOG_I("OpenCL supported printf: %s\n", hasPrintfExtension ? "true" : "false");

    return VDKResultSuccess;
}

size_t CLWrapper::queryImageRowPitch(int width, int height, cl::ImageFormat format) const
{
    if (width <= 0 || height <= 0 || mDevices.empty())
        return 0;

    size_t rowPitch = 0;

    if (isImageFormatSupported(format.image_channel_order, format.image_channel_data_type) == false) {
        XLOG_W("CLWrapper::queryImageRowPitch-> cl::ImageFormat(order:%u, type:%u) is not supported!\n",
               format.image_channel_order, format.image_channel_data_type);
        return rowPitch;
    }

    if (mIsMediaTekPlatform) {
        cl_uint pitch_alignment = 0;
        int retGetImagePitchAlignment = clGetDeviceInfo(mDevices.front()(), CL_DEVICE_IMAGE_PITCH_ALIGNMENT,
                                                       sizeof(cl_uint), &pitch_alignment, nullptr);
        XCHECK_WITH_MSG(retGetImagePitchAlignment == CL_SUCCESS || pitch_alignment == 0, VDKResultEBadState,
                        "clError: %s", clErrorInfo(retGetImagePitchAlignment).c_str());

        rowPitch = math::ceilTo(width, pitch_alignment) * getElementSize(format);
        XCHECK_WITH_MSG(rowPitch != 0, VDKResultEBadState, "clError: failed to get cl_image_desc::image_row_pitch!");
    } else {
        // clGetDeviceImageInfoQCOM is a non-ICD-dispatch QCOM vendor extension; dlsym
        // via an ICD stub may fail on Adreno ROMs lacking a Shim layer. Resolve via
        // the spec-mandated path (OpenCL §9.2): clGetExtensionFunctionAddressForPlatform.
        static clGetDeviceImageInfoQCOM_fn pfnQCOM =
            !mPlatforms.empty()
                ? reinterpret_cast<clGetDeviceImageInfoQCOM_fn>(
                      clGetExtensionFunctionAddressForPlatform(mPlatforms.front()(), "clGetDeviceImageInfoQCOM"))
                : nullptr;

        if (pfnQCOM != nullptr) {
            cl_int retGetImageInfoQCOM = pfnQCOM(mDevices.front()(), width, height, &format, CL_IMAGE_ROW_PITCH,
                                                sizeof(size_t), &rowPitch, nullptr);
            XCHECK_WITH_MSG(retGetImageInfoQCOM == CL_SUCCESS, VDKResultEBadState, "clError: %s",
                            clErrorInfo(retGetImageInfoQCOM).c_str());
        } else {
            // Fallback: conservative 64-byte alignment covers all known Adreno generations.
            rowPitch = math::ceilTo(static_cast<size_t>(width) * getElementSize(format), 64);
            XLOG_W("clGetDeviceImageInfoQCOM not resolvable; fallback pitch=%zu\n", rowPitch);
        }
    }

    return rowPitch;
}

int CLWrapper::getPlatform()
{
    perf::XTracerScoped trace("CLWrapper::getPlatform");
    return cl::Platform::get(&mPlatforms);
}

int CLWrapper::getDevice()
{
    perf::XTracerScoped trace("CLWrapper::getDevice");

    XCHECK_WITH_RET(!mPlatforms.empty(), VDKResultEInvalidParam);
    return mPlatforms.front().getDevices(CL_DEVICE_TYPE_GPU, &mDevices);
}

int CLWrapper::createContext(cl_context_properties* properties)
{
    perf::XTracerScoped trace("CLWrapper::createContext");

    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    mContext = cl::Context(mDevices, properties, nullptr, nullptr, &ret);
    XCHECK_WITH_RET(ret == CL_SUCCESS, ret);

    return ret;
}

int CLWrapper::createCommandQueue(cl_command_queue_properties properties)
{
    perf::XTracerScoped trace("CLWrapper::createCommandQueue");

    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    mCommandQueue = cl::CommandQueue(mContext, mDevices.front(), properties, &ret);
    XCHECK_WITH_RET(ret == CL_SUCCESS, ret);

    return ret;
}

int CLWrapper::getKernel(cl::Kernel& kernel, const std::string& name)
{
    XCHECK_WITH_MSG(mKernels.find(name) != mKernels.end(), VDKResultEInvalidParam, "failed to find kernel[%s]!",
                    name.c_str());
    kernel = mKernels[name];
    return VDKResultSuccess;
}

int CLWrapper::requireKernel(cl::Kernel& kernel,
                              const std::string& nameKernel,
                              const std::vector<const char*> kernelsCommon,
                              const char* kernelSource,
                              const std::string optionsCompile)
{
    perf::XTracerScoped trace("CLWrapper::requireKernel");

    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    // 1.find in kernel cache
    if (mKernels.find(nameKernel) != mKernels.end()) {
        kernel = mKernels[nameKernel];
        XLOG_D("CLWrapper::requireKernel-> find kernel[%s] in cache\n", nameKernel.c_str());
        return VDKResultSuccess;
    }

    std::string hashDevice;
    int retGetKeyDevice = getDeviceKey(hashDevice);
    XCHECK_WITH_RET(retGetKeyDevice == VDKResultSuccess, retGetKeyDevice);

    std::string sourceFull;
    for (const char* common : kernelsCommon) {
        if (common) {
            sourceFull += common;
            sourceFull += "\n";
        }
    }
    sourceFull += "#line 1 \"" + nameKernel + ".cl\"\n";
    sourceFull += kernelSource;

    // need to hash(kernelsCommon + kernelSource + optionsCompile)
    std::string hashSource = std::to_string(std::hash<std::string>{}(sourceFull + optionsCompile));
    hashSource.resize(20, 'X');

    // nameFullBin: folder/nameKernel_hashDevice_hashSource.bin
    std::string pathFullBin = mFolderBinary + "/" + nameKernel + "_" + hashDevice + "_" + hashSource + ".bin";

    cl::Program program;

    // 2.find kernel.bin
    if (file::exists(pathFullBin)) {
        std::string stringBinary;
        int retLoadBuffer = file::read(pathFullBin, stringBinary);
        XCHECK_WITH_RET(retLoadBuffer == VDKResultSuccess, retLoadBuffer);
        XCHECK_WITH_RET(!stringBinary.empty(), VDKResultEFile);

        std::vector<unsigned char> bin(stringBinary.cbegin(), stringBinary.cend());
        cl::Program::Binaries bins = {bin};

        int ret = CL_SUCCESS;
        program = cl::Program(mContext, mDevices, bins, NULL, &ret);
        XCHECK_WITH_MSG(ret == CL_SUCCESS && !bins.empty(), ret, "clError: %s", clErrorInfo(ret).c_str());

        int retBuild = program.build(mDevices, optionsCompile.c_str());  // build is required, since it's a binary file,
                                                                      // compilation will be completed instantly
        if (retBuild != CL_SUCCESS) {
            XLOG_E("clError: %s\n", clErrorInfo(retBuild).c_str());

            int retGetProgramBuildLog = CL_SUCCESS;
            std::string log = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevices.front(), &retGetProgramBuildLog);
            if (retGetProgramBuildLog != CL_SUCCESS) {
                XLOG_E("clError: %s\n", clErrorInfo(retGetProgramBuildLog).c_str());
                return retGetProgramBuildLog;
            }
            XLOG_E("cl build log: %s\n", log.c_str());
        }

        XLOG_D("CLWrapper::requireKernel-> find kernel[%s] in binary\n", nameKernel.c_str());
    }

    // 3.build with kernel source
    if (program.get() == nullptr) {
        XLOG_I("CLWrapper::requireKernel-> JIT compiling kernel[%s]\n", nameKernel.c_str());

        int ret = CL_SUCCESS;
        program = cl::Program(mContext, sourceFull, false, &ret);
        XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

        int retBuild = program.build(mDevices, optionsCompile.c_str());
        if (retBuild != CL_SUCCESS) {
            std::string log = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevices.front(), &ret);
            XLOG_E("cl build log [%s]: %s\n", nameKernel.c_str(), log.c_str());
            return retBuild;
        }

        // save binary file
        cl::Program::Binaries bins = program.getInfo<CL_PROGRAM_BINARIES>(&ret);
        if (ret == CL_SUCCESS && !bins.empty()) {
            std::string binStr(bins.front().cbegin(), bins.front().cend());
            file::write(binStr, pathFullBin);
        }
    }

    // 4.create & stash kernel
    int retCreateKernel = CL_SUCCESS;
    kernel = cl::Kernel(program, nameKernel.c_str(), &retCreateKernel);
    XCHECK_WITH_MSG(retCreateKernel == CL_SUCCESS, retCreateKernel, "clError: %s",
                    clErrorInfo(retCreateKernel).c_str());

    mKernels[nameKernel] = kernel;

    /**
     * @note temp program will be destroy, but kernel will be retain in map
     */

    return VDKResultSuccess;
}

int CLWrapper::getDeviceKey(std::string& key)
{
    std::string nameDevice, vendorDevice, versionDevice, driverDevice;

    int retGetName = getDeviceName(nameDevice);
    XCHECK_WITH_RET(retGetName == VDKResultSuccess, retGetName);

    int retGetVendor = getDeviceVendor(vendorDevice);
    XCHECK_WITH_RET(retGetVendor == VDKResultSuccess, retGetVendor);

    int retGetVersion = getDeviceVersion(versionDevice);
    XCHECK_WITH_RET(retGetVersion == VDKResultSuccess, retGetVersion);

    int retGetDriver = getDeviceDriver(driverDevice);
    XCHECK_WITH_RET(retGetDriver == VDKResultSuccess, retGetDriver);

    std::string strDevice = nameDevice + vendorDevice + versionDevice + driverDevice;
    key = std::to_string(std::hash<std::string>{}(strDevice));
    key.resize(20, 'X');

    return VDKResultSuccess;
}

int CLWrapper::getDeviceName(std::string& name)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    name = mDevices.front().getInfo<CL_DEVICE_NAME>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return ret;
}

int CLWrapper::getDeviceVendor(std::string& vendor)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    vendor = mDevices.front().getInfo<CL_DEVICE_VENDOR>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return ret;
}

int CLWrapper::getDeviceVersion(std::string& version)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    version = mDevices.front().getInfo<CL_DEVICE_VERSION>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return ret;
}

int CLWrapper::getDeviceDriver(std::string& driver)
{
    XCHECK_WITH_RET(!mDevices.empty(), VDKResultEInvalidParam);

    int ret = CL_SUCCESS;
    driver = mDevices.front().getInfo<CL_DRIVER_VERSION>(&ret);
    XCHECK_WITH_MSG(ret == CL_SUCCESS, ret, "clError: %s", clErrorInfo(ret).c_str());

    return ret;
}

int CLWrapper::getKernelName(const cl::Kernel kernel, std::string& name)
{
    name.clear();
    size_t length = 0;
    cl_int retGetKernelNameLength = clGetKernelInfo(kernel.get(), CL_KERNEL_FUNCTION_NAME, 0, nullptr, &length);
    XCHECK_WITH_MSG(retGetKernelNameLength == CL_SUCCESS, retGetKernelNameLength, "clError: %s",
                    clErrorInfo(retGetKernelNameLength).c_str());

    name = std::string(length, '0');
    cl_int retGetKernelName =
        clGetKernelInfo(kernel.get(), CL_KERNEL_FUNCTION_NAME, length, (char*)name.c_str(), nullptr);
    XCHECK_WITH_MSG(retGetKernelName == CL_SUCCESS, retGetKernelName, "clError: %s",
                    clErrorInfo(retGetKernelName).c_str());

    return CL_SUCCESS;
}

}  // namespace gpu
}  // namespace au

// AURA_NS_WRAPPED
