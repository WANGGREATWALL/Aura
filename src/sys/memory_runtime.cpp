#include "sys/memory_runtime.h"
#include "log/xerror.h"
#include "sys/xsystem.h"
#include <atomic>
#include <cerrno>
#if AU_OS_LINUX || AU_OS_ANDROID
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
#ifdef AURA_MM_TESTING
std::atomic<au::sys::memory::AccessHook> gAccessHook{nullptr};
#endif
[[maybe_unused]] int platformError(int code) noexcept
{
    if (code == ENOMEM) return au::err::kErrorNoMemory;
    if (code == EACCES || code == EPERM) return au::err::kErrorPermissionDenied;
    if (code == ENOTTY || code == ENOSYS) return au::err::kErrorNotSupported;
    return au::err::kErrorPlatformAPI;
}
}

int au::sys::memory::cpuAccess(int fd, unsigned int mode, bool start, int* nativeCode) noexcept
{
    if (nativeCode) *nativeCode = 0;
    if (mode < 1 || mode > 3 || fd < 0) return au::err::kErrorInvalidParam;
#ifdef AURA_MM_TESTING
    if (auto hook = gAccessHook.load(std::memory_order_acquire)) // Publish test configuration before access.
        return hook(fd, mode, start, nativeCode);
#endif
#if AU_OS_LINUX || AU_OS_ANDROID
    dma_buf_sync sync{};
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
        ((mode & 1u) ? DMA_BUF_SYNC_READ : 0) | ((mode & 2u) ? DMA_BUF_SYNC_WRITE : 0);
    int result;
    do { result = ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    if (result == 0) return 0;
    const int error = errno;
    if (nativeCode) *nativeCode = error;
    return platformError(error);
#else
    (void)start;
    return au::err::kErrorNotSupported;
#endif
}

int au::sys::memory::duplicateFd(int fd, std::size_t size, int& owned,
                                au::sys::memory::FdIdentity& identity) noexcept
{
    owned = -1;
#if AU_OS_LINUX || AU_OS_ANDROID
    int duplicate;
    do { duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0); } while (duplicate < 0 && errno == EINTR);
    if (duplicate < 0) return platformError(errno);
    struct stat info{};
    if (::fstat(duplicate, &info) != 0) { const int error = errno; ::close(duplicate); return platformError(error); }
    // dma-buf exporters report their allocation capacity through st_size.
    if (info.st_size <= 0 || size > static_cast<std::uint64_t>(info.st_size)) {
        ::close(duplicate); return au::err::kErrorOutOfRange;
    }
    identity.mDevice = static_cast<std::uint64_t>(info.st_dev);
    identity.mInode = static_cast<std::uint64_t>(info.st_ino);
    owned = duplicate;
    return 0;
#else
    (void)fd; (void)size; (void)identity;
    return au::err::kErrorNotSupported;
#endif
}
void au::sys::memory::closeFd(int fd) noexcept
{
#if AU_OS_LINUX || AU_OS_ANDROID
    if (fd >= 0) (void)::close(fd); // close is never retried: the descriptor may already have been reused.
#else
    (void)fd;
#endif
}
#ifdef AURA_MM_TESTING
void au::sys::memory::setAccessHook(au::sys::memory::AccessHook hook) noexcept
{ gAccessHook.store(hook, std::memory_order_release); }
#endif
