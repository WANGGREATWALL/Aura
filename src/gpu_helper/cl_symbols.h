#ifndef CL_SYMBOLS_H_
#define CL_SYMBOLS_H_

#include "sys/xsystem.h"

// Include this header before other OpenCL headers in each translation unit.
// Apple uses OpenCL 1.2; other platforms default to 2.0 with a 1.2 minimum.
#if AU_OS_APPLE
#if defined(CL_HPP_) && CL_HPP_TARGET_OPENCL_VERSION != 120
#error "Include cl_symbols.h before OpenCL C++ headers on Apple platforms"
#endif
#undef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#undef CL_HPP_TARGET_OPENCL_VERSION
#define CL_HPP_TARGET_OPENCL_VERSION 120
#undef CL_HPP_MINIMUM_OPENCL_VERSION
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#else
#ifndef CL_TARGET_OPENCL_VERSION
#ifdef CL_HPP_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION CL_HPP_TARGET_OPENCL_VERSION
#else
#define CL_TARGET_OPENCL_VERSION 200
#endif
#endif
#ifndef CL_HPP_TARGET_OPENCL_VERSION
#define CL_HPP_TARGET_OPENCL_VERSION CL_TARGET_OPENCL_VERSION
#endif
#ifndef CL_HPP_MINIMUM_OPENCL_VERSION
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#endif
#endif

#if CL_TARGET_OPENCL_VERSION < CL_HPP_TARGET_OPENCL_VERSION
#error "CL_TARGET_OPENCL_VERSION must cover CL_HPP_TARGET_OPENCL_VERSION"
#endif

#include "CL/opencl.hpp"
#include "sys/xdlib.h"

namespace au {
namespace gpu {

/**
 * @brief Process-wide OpenCL loader, initialized once on first use.
 *
 * The library is immutable after construction and owned until static teardown.
 * OpenCL objects must be destroyed before that teardown. No reload/unload API
 * is exposed, so cached pointers remain valid throughout normal operation.
 * Each C trampoline caches its own typed pointer, including a missing symbol.
 * These entry points must not be called recursively while the loader initializes.
 */
class CLSymbols final
{
public:
    static bool isLoaded() noexcept;

    // Cold path only: wrappers call this once per symbol. XDLib contains the
    // synchronized name cache; allocation/lookup failures cannot cross the C ABI.
    template <typename Func>
    static Func* get(const char* name) noexcept
    {
        if (name == nullptr || *name == '\0') {
            return nullptr;
        }
        try {
            CLSymbols& loader = instance();
            return loader.mLoaded ? loader.mLib.get<Func>(name) : nullptr;
        } catch (...) {
            return nullptr;
        }
    }

    CLSymbols(const CLSymbols&) = delete;
    CLSymbols& operator=(const CLSymbols&) = delete;
    CLSymbols(CLSymbols&&) = delete;
    CLSymbols& operator=(CLSymbols&&) = delete;

private:
    CLSymbols();
    ~CLSymbols() = default;
    static CLSymbols& instance();

    sys::XDLib mLib;
    bool mLoaded = false;
};

} // namespace gpu
} // namespace au

// Compatibility with the existing gpu_helper namespace. New code uses au::gpu.
namespace gpu {
using CLSymbols = ::au::gpu::CLSymbols;
}

#endif // CL_SYMBOLS_H_
