#include "sys/opencl_memory.h"
#include "CL/cl_ext.h"
#include "log/xerror.h"
#include <string>
#include <vector>

int au::sys::opencl::queryImports(cl_platform_id platform, cl_device_id device, decltype(&::clGetDeviceInfo) deviceInfo,
    decltype(&::clGetExtensionFunctionAddressForPlatform) lookup,
    au::sys::opencl::ImportCapabilities& out, int* native) noexcept
{
    out = {}; if (native) *native = 0;
    try {
        std::size_t size = 0;
        cl_int code = deviceInfo(device, CL_DEVICE_EXTENSIONS, 0, nullptr, &size);
        if (code) { if (native) *native = code; return au::err::kErrorPlatformAPI; }
        if (!size) return 0;
        std::vector<char> bytes(size);
        code = deviceInfo(device, CL_DEVICE_EXTENSIONS, bytes.size(), bytes.data(), nullptr);
        if (code) { if (native) *native = code; return au::err::kErrorPlatformAPI; }
        if (bytes.back() != '\0') return au::err::kErrorBadFormat;
        const std::string extensions = " " + std::string(bytes.data()) + " ";
        if (extensions.find(" cl_arm_import_memory ") != std::string::npos &&
            extensions.find(" cl_arm_import_memory_dma_buf ") != std::string::npos)
            out.mArmImport = lookup(platform, "clImportMemoryARM");
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
int au::sys::opencl::importDmaBuf(const au::sys::opencl::ImportCapabilities& capabilities, cl_context context,
    int fd, std::size_t size, cl_mem* out, int* native) noexcept
{
    if (native) *native = 0;
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    if (fd < 0 || !size) return au::err::kErrorInvalidParam;
    if (!capabilities.mArmImport) return au::err::kErrorNotSupported;
    const auto import = reinterpret_cast<clImportMemoryARM_fn>(capabilities.mArmImport);
    const cl_import_properties_arm properties[]{CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
        CL_IMPORT_DMA_BUF_DATA_CONSISTENCY_WITH_HOST_ARM, CL_FALSE, 0};
    cl_int code = CL_SUCCESS;
    *out = import(context, CL_MEM_READ_WRITE, properties, &fd, size, &code);
    if (native) *native = code;
    if (!code && *out) return 0;
    return code == CL_OUT_OF_HOST_MEMORY ? au::err::kErrorNoMemory : au::err::kErrorPlatformAPI;
}
