#include "sys/xdlib.h"

namespace au {
namespace sys {

XDLib::~XDLib() { unload(); }

XDLib::XDLib(XDLib&& other) noexcept
    : mHandle(other.mHandle)
    , mSymbolCache(std::move(other.mSymbolCache))
{
    other.mHandle = nullptr;
}

XDLib& XDLib::operator=(XDLib&& other) noexcept
{
    if (this != &other) {
        unload();
        mHandle       = other.mHandle;
        mSymbolCache  = std::move(other.mSymbolCache);
        other.mHandle = nullptr;
    }
    return *this;
}

int XDLib::load(const std::string& path, int flags)
{
    if (mHandle != nullptr) {
        unload();
    }

    if (path.empty()) {
        XLOG_E("XDLib: load called with empty path\n");
        return kErrorOpenFailed;
    }

#if AU_OS_WINDOWS
    (void)flags;  // Windows does not support dlopen flags
    mHandle = LoadLibraryA(path.c_str());
    if (mHandle == nullptr) {
        XLOG_E("XDLib: failed to load \"%s\" (error=%lu)\n",
               path.c_str(), static_cast<unsigned long>(GetLastError()));
        return kErrorOpenFailed;
    }
#else
    mHandle = dlopen(path.c_str(), flags);
    if (mHandle == nullptr) {
        XLOG_E("XDLib: failed to load \"%s\" (%s)\n", path.c_str(), dlerror());
        return kErrorOpenFailed;
    }
#endif

    XLOG_I("XDLib: loaded \"%s\"\n", path.c_str());
    return kSuccess;
}

int XDLib::load(const std::vector<std::string>& paths, int flags)
{
    if (mHandle != nullptr) {
        unload();
    }

    for (const auto& path : paths) {
        if (path.empty()) {
            continue;
        }
#if AU_OS_WINDOWS
        (void)flags;  // Windows does not support dlopen flags
        auto handle = LoadLibraryA(path.c_str());
#else
        auto handle = dlopen(path.c_str(), flags);
#endif
        if (handle != nullptr) {
            mHandle = handle;
            XLOG_I("XDLib: loaded \"%s\"\n", path.c_str());
            return kSuccess;
        }
    }

    XLOG_E("XDLib: failed to load from %zu candidate paths\n", paths.size());
    return kErrorOpenFailed;
}

int XDLib::unload()
{
    if (mHandle == nullptr) {
        return kSuccess;
    }

#if AU_OS_WINDOWS
    if (!FreeLibrary(mHandle)) {
        XLOG_E("XDLib: FreeLibrary failed (error=%lu)\n",
               static_cast<unsigned long>(GetLastError()));
        return kErrorInvalidHandle;
    }
#else
    if (dlclose(mHandle) != 0) {
        XLOG_E("XDLib: dlclose failed (%s)\n", dlerror());
        return kErrorInvalidHandle;
    }
#endif

    mHandle = nullptr;
    std::lock_guard<std::mutex> lock(mMutex);
    mSymbolCache.clear();
    return kSuccess;
}

bool XDLib::isLoaded() const noexcept { return mHandle != nullptr; }

}  // namespace sys

}  // namespace au

// AURA_NS_WRAPPED
