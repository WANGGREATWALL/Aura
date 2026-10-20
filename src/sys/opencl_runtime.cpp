#include "sys/opencl_runtime.h"
#include "sys/xdlib.h"
#include "log/xerror.h"
#include <string>
#include <vector>

int au::sys::opencl::loadLibrary(au::sys::XDLib& library) noexcept
{
    try {
        const std::vector<std::string> paths = {
#if AU_OS_WINDOWS
            "OpenCL.dll", "OpenCL64.dll", "OpenCL32.dll"
#elif AU_OS_APPLE
            "/System/Library/Frameworks/OpenCL.framework/OpenCL"
#elif AU_OS_ANDROID
            "libOpenCL.so",
            "/vendor/lib64/libOpenCL.so", "/system/vendor/lib64/libOpenCL.so", "/system/lib64/libOpenCL.so",
            "/vendor/lib/libOpenCL.so", "/system/vendor/lib/libOpenCL.so", "/system/lib/libOpenCL.so"
#elif AU_OS_LINUX
            "libOpenCL.so.1", "libOpenCL.so",
            "/usr/lib/x86_64-linux-gnu/libOpenCL.so.1", "/usr/lib/x86_64-linux-gnu/libOpenCL.so",
            "/usr/lib/libOpenCL.so.1", "/usr/lib/libOpenCL.so", "/usr/lib64/libOpenCL.so.1", "/usr/lib64/libOpenCL.so"
#else
            "libOpenCL.so", "OpenCL.dll"
#endif
        };
        return library.load(paths) == au::sys::XDLib::kSuccess ? 0 : au::err::kErrorNotSupported;
    } catch (...) { return au::err::kErrorNoMemory; }
}
