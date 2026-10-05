#ifndef CL_SYMBOLS_H
#define CL_SYMBOLS_H

/**
 * @file cl_symbols.h
 * @brief Lightweight OpenCL dynamic library singleton for on-demand symbol resolution.
 *
 * CLSymbols wraps XDLib and exposes a get() singleton that holds the loaded
 * OpenCL library handle. Callers resolve individual symbols on demand via the
 * XDLIB_GET macro or XDLib::get().
 *
 * This design suits callers that only need a small, ad-hoc subset of OpenCL symbols.
 * For full symbol tables with pre-resolved function-pointer members and global
 * CL_API_CALL forwarding, use CLSymbols (cl_symbols.h) instead.
 *
 * @code
 *   // Resolve and call a symbol on demand:
 *   auto fp = XDLIB_GET(gpu::CLSymbols::lib(), clGetPlatformIDs);
 *   if (fp) fp(0, nullptr, &n);
 * @endcode
 */

#include <string>
#include <vector>

#include "log/xlogger.h"
#include "sys/xdlib.h"
#include "sys/xsystem.h"

// ----------------------------------------------------------------------------
// OpenCL version configuration
// Must be defined before including any CL header.
// ----------------------------------------------------------------------------
#if AU_OS_APPLE
#undef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#undef CL_HPP_TARGET_OPENCL_VERSION
#define CL_HPP_TARGET_OPENCL_VERSION 120
#undef CL_HPP_MINIMUM_OPENCL_VERSION
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#else
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 200
#endif
#ifndef CL_HPP_TARGET_OPENCL_VERSION
#define CL_HPP_TARGET_OPENCL_VERSION 200
#endif
#ifndef CL_HPP_MINIMUM_OPENCL_VERSION
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#endif
#endif
#include "CL/cl2.hpp"

namespace au {
namespace gpu {

/**
 * @brief Singleton that owns the dynamically loaded OpenCL library handle.
 *
 * Prefer CLSymbols (cl_symbols.h) for modules that use many OpenCL symbols
 * and benefit from the X-Macro pre-resolved function-pointer table.
 * Use CLSymbols when only a handful of symbols are resolved on demand.
 */
class CLSymbols
{
public:
    /** @brief Returns the singleton instance. */
    static sys::XDLib& lib()
    {
        static CLSymbols instance;
        return instance.mLib;
    }

    ~CLSymbols() = default;

    CLSymbols(const CLSymbols&)            = delete;
    CLSymbols& operator=(const CLSymbols&) = delete;

    /** @brief Returns true if the OpenCL library was loaded successfully. */
    bool isLoaded() const noexcept { return mLib.isLoaded(); }

private:
    CLSymbols()
    {
        if (mLib.load(mLibPaths) != 0) {
            XLOG_E("CLSymbols: failed to load any OpenCL library\n");
        }
    }

    sys::XDLib mLib;

    std::vector<std::string> mLibPaths = {
#if AU_OS_WINDOWS
        "OpenCL.dll", "OpenCL64.dll", "OpenCL32.dll"
#elif AU_OS_APPLE
        "/System/Library/Frameworks/OpenCL.framework/OpenCL"
#elif AU_OS_ANDROID
        "libOpenCL.so",

        // 64-bit
        "/vendor/lib64/libOpenCL.so", "/system/vendor/lib64/libOpenCL.so", "/system/lib64/libOpenCL.so",

        // 32-bit
        "/vendor/lib/libOpenCL.so", "/system/vendor/lib/libOpenCL.so", "/system/lib/libOpenCL.so"
#elif AU_OS_LINUX
        "libOpenCL.so.1", "libOpenCL.so",

        // Debian/Ubuntu
        "/usr/lib/x86_64-linux-gnu/libOpenCL.so.1", "/usr/lib/x86_64-linux-gnu/libOpenCL.so",

        // Arch/Fedora/CentOS
        "/usr/lib/libOpenCL.so.1", "/usr/lib/libOpenCL.so", "/usr/lib64/libOpenCL.so.1", "/usr/lib64/libOpenCL.so"
#else
        "libOpenCL.so", "OpenCL.dll"
#endif
    };
};

} // namespace gpu

} // namespace au

#endif // CL_SYMBOLS_H

// AURA_NS_WRAPPED
