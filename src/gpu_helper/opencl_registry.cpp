#include "opencl_registry.h"

#include <unistd.h>
#include <algorithm>

#include "opencl_buffer.h"
#include "cv/ximagef.h"
#include "log/xlogger.h"

namespace au {
namespace gpu {

OpenCLRegistry& OpenCLRegistry::get() noexcept
{
    static OpenCLRegistry instance;
    return instance;
}

std::shared_ptr<OpenCLBuffer> OpenCLRegistry::getOrCreate(int fd, void* hostPtr, std::size_t size) noexcept
{
    if (fd <= 0 || size == 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mMutex);

    auto it = mMap.find(fd);
    if (it != mMap.end()) {
        if (auto sp = it->second.lock()) {
            return sp; // alive -> reuse
        }
        // expired -> re-create below and overwrite the slot
    }

    int duped = ::dup(fd);
    if (duped < 0) {
        XLOG_E("OpenCLRegistry::getOrCreate dup(%d) failed\n", fd);
        return nullptr;
    }

    std::shared_ptr<OpenCLBuffer> sp(new OpenCLBuffer(duped, hostPtr, size));

    if (it != mMap.end()) {
        it->second = sp;
    } else {
        mMap.emplace(fd, sp);
    }
    return sp;
}

void OpenCLRegistry::purgeExpired() noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto it = mMap.begin(); it != mMap.end();) {
        if (it->second.expired()) {
            it = mMap.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t OpenCLRegistry::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mMap.size();
}

std::shared_ptr<OpenCLBuffer> importImage(const cv::Image& img) noexcept
{
    if (!img.isValid() || img.fd[0] <= 0) {
        return nullptr;
    }

    // Full dmabuf span: the farthest plane end. Correct for single-plane and for
    // single-fd multi-plane layouts alike.
    std::size_t span = 0;
    for (int i = 0; i < 4; ++i) {
        if (img.data[i] && img.dataSize[i] > 0) {
            span = std::max(span, static_cast<std::size_t>(img.fdOffset[i]) +
                                  static_cast<std::size_t>(img.dataSize[i]));
        }
    }
    if (span == 0) {
        return nullptr;
    }

    return OpenCLRegistry::get().getOrCreate(img.fd[0], img.data[0], span);
}

} // namespace gpu
} // namespace au
