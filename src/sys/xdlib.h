#ifndef XDLIB_H
#define XDLIB_H

/**
 * @file xdlib.h
 * @brief Cross-platform dynamic library loader with thread-safe symbol caching.
 *
 * Supports Windows (LoadLibrary), Linux/macOS/Android (dlopen).
 * Compared with XDLib, this variant exposes a typed get<Func>() template
 * directly (thread-safe, cached) instead of the raw getSymbol() + call<>() pair.
 *
 * @example
 *   sys::XDLib lib;
 *   lib.load("/usr/lib/libfoo.so");
 *
 *   // Resolve a symbol by type:
 *   auto fp = lib.get<decltype(someFunc)>("someFunc");
 *   if (fp) fp(arg1, arg2);
 *
 *   // Or use the convenience macro:
 *   auto fp2 = XDLIB_GET(lib, someFunc);
 */

#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log/xlogger.h"
#include "sys/xsystem.h"

#if AU_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace au {
namespace sys {

class XDLib
{
#if AU_OS_WINDOWS
    using NativeHandle = HMODULE;
#else
    using NativeHandle = void*;
#endif

public:
    static constexpr int kSuccess = 0;
    static constexpr int kErrorOpenFailed = -1;
    static constexpr int kErrorInvalidHandle = -2;

#if AU_OS_WINDOWS
    static constexpr int kDefaultLoadFlags = 0;
#else
    static constexpr int kDefaultLoadFlags = RTLD_NOW | RTLD_LOCAL;
#endif

    XDLib() = default;
    ~XDLib();

    XDLib(XDLib&& other) noexcept;
    XDLib& operator=(XDLib&& other) noexcept;

    XDLib(const XDLib&) = delete;
    XDLib& operator=(const XDLib&) = delete;

    /**
     * @brief Load a dynamic library from the given path.
     * @param path Absolute or relative path to the shared library.
     * @param flags dlopen flags (Linux/macOS/Android only); ignored on Windows.
     *              Default: RTLD_NOW | RTLD_LOCAL.
     *              Use RTLD_NODELETE to prevent unloading at process exit (useful
     *              for vendor libraries that spawn background threads).
     * @return kSuccess on success, kErrorOpenFailed on failure.
     */
    int load(const std::string& path, int flags = kDefaultLoadFlags);

    /**
     * @brief Try loading from multiple candidate paths, stop on first success.
     * @param paths Ordered list of candidate library paths.
     * @param flags dlopen flags (Linux/macOS/Android only); ignored on Windows.
     * @return kSuccess on success, kErrorOpenFailed if all paths fail.
     *
     * @example
     *   lib.load({"/vendor/lib64/libOpenCL.so", "/system/lib64/libOpenCL.so"});
     */
    int load(const std::vector<std::string>& paths, int flags = kDefaultLoadFlags);

    /**
     * @brief Convenience overload: accept a brace-enclosed list of candidate paths.
     *
     * Delegates to load(const std::vector<std::string>&).
     * Allows call-sites to pass a braced initializer without constructing a vector explicitly.
     *
     * @param paths Brace-enclosed list of candidate library paths.
     * @param flags dlopen flags (Linux/macOS/Android only); ignored on Windows.
     * @return kSuccess on success, kErrorOpenFailed if all paths fail.
     *
     * @example
     *   lib.load({"/vendor/lib64/libOpenCL.so", "/system/lib64/libOpenCL.so"});
     */
    int load(std::initializer_list<std::string> paths, int flags = kDefaultLoadFlags)
    {
        return load(std::vector<std::string>(paths), flags);
    }

    /**
     * @brief Unload the library and clear the symbol cache.
     *
     * Safe to call when no library is loaded (returns kSuccess immediately).
     *
     * @return kSuccess on success, kErrorInvalidHandle on close failure.
     */
    int unload();

    /** @brief Check if a library is currently loaded. */
    bool isLoaded() const noexcept;

    /**
     * @brief Resolve a symbol from the loaded library (thread-safe, cached).
     * @tparam Func Function signature type, e.g. decltype(clGetPlatformIDs).
     * @param name The symbol name to look up (null-terminated).
     * @return Typed function pointer, or nullptr if not found.
     *
     * @example
     *   auto fp = lib.get<decltype(clGetPlatformIDs)>("clGetPlatformIDs");
     *   if (fp) fp(num_entries, platforms, num_platforms);
     */
    template <typename Func>
    Func* get(const char* name)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mHandle == nullptr) {
            return nullptr;
        }
        auto [it, inserted] = mSymbolCache.try_emplace(name, nullptr);
        if (inserted) {
#if AU_OS_WINDOWS
            it->second = reinterpret_cast<void*>(GetProcAddress(mHandle, name));
#else
            it->second = dlsym(mHandle, name);
#endif
            if (it->second == nullptr) {
                XLOG_E("XDLib: failed to resolve symbol \"%s\"\n", name);
                mSymbolCache.erase(it);
                return nullptr;
            }
        }
        return reinterpret_cast<Func*>(it->second);
    }

private:
    NativeHandle mHandle = nullptr;
    std::unordered_map<std::string, void*> mSymbolCache;
    std::mutex mMutex;
};

}  // namespace sys

}  // namespace au

/**
 * @brief Convenience macro: resolve a symbol with automatic name stringification.
 * @example auto fp = XDLIB_GET(lib, clGetPlatformIDs);
 */
#define XDLIB_GET(lib, func) (lib).get<decltype(func)>(#func)

#endif  // XDLIB_H

// AURA_NS_WRAPPED
