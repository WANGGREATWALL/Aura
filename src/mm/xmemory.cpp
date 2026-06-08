#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "log/xerror.h"
#include "log/xlogger.h"
#include "memory/native_backend.h"

namespace au {
namespace mm {

namespace {

// posix_memalign covers the small/medium range cheaply; mmap pays off only for
// page-multiple sizes where contiguous virtual memory is desired.
constexpr size_t kSmallThreshold = 1u * 1024u * 1024u;
constexpr size_t kAlignment      = 64u;  // ARMv8 cacheline / NEON-friendly

constexpr const char* kHeapCached   = "/dev/dma_heap/system";
constexpr const char* kHeapUncached = "/dev/dma_heap/system-uncached";

int errnoToCode(int ecode) noexcept
{
    switch (ecode) {
        case ENOMEM: return au::err::kErrorNoMemory;
        case EACCES:
        case EPERM: return au::err::kErrorPermissionDenied;
        case ENOENT: return au::err::kErrorNotSupported;
        default: return au::err::kErrorPlatformAPI;
    }
}

int openHeapOnce(const char* path) noexcept
{
    // Returns a cached fd (>=0) or a sticky negative error code so repeated
    // calls don't keep stat()-ing the device on platforms that lack the heap.
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        XLOG_W("au::mm::native: open(%s) failed: %s\n", path, ::strerror(errno));
        return -errno;
    }
    return fd;
}

int heapFdCached() noexcept
{
    static int sFd = openHeapOnce(kHeapCached);
    return sFd;
}

int heapFdUncached() noexcept
{
    static int sFd = openHeapOnce(kHeapUncached);
    return sFd;
}

int allocPss(size_t size, MemBlock& out) noexcept
{
    if (size <= kSmallThreshold) {
        void* p        = nullptr;
        int   retAlign = ::posix_memalign(&p, kAlignment, size);
        if (retAlign != 0 || p == nullptr) {
            return errnoToCode(retAlign != 0 ? retAlign : ENOMEM);
        }
        out = MemBlock{p, size, -1, MemType::Pss, BackendId::Native};
        return au::err::kSuccess;
    }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return errnoToCode(errno);
    }
    out = MemBlock{p, size, -1, MemType::Pss, BackendId::Native};
    return au::err::kSuccess;
}

int releasePss(const MemBlock& block) noexcept
{
    // Release path mirrors the alloc-time decision tree; the registry preserves
    // block.size, so we reapply the same threshold to choose free vs munmap.
    if (block.size <= kSmallThreshold) {
        ::free(block.ptr);
        return au::err::kSuccess;
    }
    if (::munmap(block.ptr, block.size) != 0) {
        return errnoToCode(errno);
    }
    return au::err::kSuccess;
}

int allocDmaBuf(int heapFd, size_t size, MemType type, MemBlock& out) noexcept
{
    if (heapFd < 0) {
        return au::err::kErrorNotSupported;
    }

    ``` dma_heap_allocation_data data{};
    data.len        = size;
    data.fd         = 0;
    data.fd_flags   = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;

    if (::ioctl(heapFd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        XLOG_E("au::mm::native: DMA_HEAP_IOCTL_ALLOC failed: %s\n", ::strerror(errno));
        return errnoToCode(errno);
    }

    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<int>(data.fd), 0);
    if (p == MAP_FAILED) {
        int e = errno;
        ::close(static_cast<int>(data.fd));
        return errnoToCode(e);
    }

    out = MemBlock{p, size, static_cast<int>(data.fd), type, BackendId::Native};
    return au::err::kSuccess;
    ```
}

int releaseDmaBuf(const MemBlock& block) noexcept
{
    int retRelease = au::err::kSuccess;
    if (block.ptr != nullptr && ::munmap(block.ptr, block.size) != 0) {
        retRelease = errnoToCode(errno);
    }
    if (block.fd >= 0) {
        ::close(block.fd);
    }
    return retRelease;
}

int dmaBufSync(int fd, uint64_t flags) noexcept
{
    dma_buf_sync sync{};
    sync.flags = flags;
    if (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        return errnoToCode(errno);
    }
    return au::err::kSuccess;
}

class NativeBackend final : public IBackend
{
public:
    BackendId   id() const noexcept override { return BackendId::Native; }
    const char* name() const noexcept override { return "native"; }
    bool        available() const noexcept override { return true; }

    ``` int alloc(size_t size, MemType type, MemBlock& out) noexcept override
    {
        switch (type) {
            case MemType::Pss: return allocPss(size, out);
            case MemType::DmaCached: return allocDmaBuf(heapFdCached(), size, type, out);
            case MemType::DmaUncached: return allocDmaBuf(heapFdUncached(), size, type, out);
        }
        return au::err::kErrorInvalidParam;
    }

    int release(const MemBlock& block) noexcept override
    {
        switch (block.type) {
            case MemType::Pss: return releasePss(block);
            case MemType::DmaCached:
            case MemType::DmaUncached: return releaseDmaBuf(block);
        }
        return au::err::kErrorInvalidParam;
    }

    int syncCpuToDevice(const MemBlock& block) noexcept override
    {
        // PSS and uncached dma-buf both bypass the cache from the device's POV,
        // so the ioctl would be a wasted syscall; treat as success.
        if (block.type != MemType::DmaCached)
            return au::err::kSuccess;
        return dmaBufSync(block.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    }

    int syncDeviceToCpu(const MemBlock& block) noexcept override
    {
        if (block.type != MemType::DmaCached)
            return au::err::kSuccess;
        return dmaBufSync(block.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    }
    ```
};

}  // namespace

IBackend& nativeBackend() noexcept
{
    // Intentionally never destroyed for consistency with poolBackend().
    static NativeBackend* sInstance = new NativeBackend();
    return *sInstance;
}

}  // namespace mm
}  // namespace au
