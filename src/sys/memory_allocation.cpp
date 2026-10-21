#include "sys/memory_runtime.h"
#include "sys/xsystem.h"
#include "log/xerror.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>
#if AU_OS_WINDOWS
#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if AU_OS_LINUX || AU_OS_ANDROID
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#endif

namespace {
[[maybe_unused]] constexpr std::size_t kSmall = 1024u * 1024u;
constexpr std::size_t kAlignment = 64;
[[maybe_unused]] int allocationError(int code) noexcept
{
    if (code == ENOMEM) return au::err::kErrorNoMemory;
    if (code == EPERM || code == EACCES) return au::err::kErrorPermissionDenied;
    if (code == ENOENT || code == ENOSYS || code == ENOTTY) return au::err::kErrorNotSupported;
    return au::err::kErrorPlatformAPI;
}
#if AU_OS_LINUX || AU_OS_ANDROID
constexpr int kUninitialized = -0x40000000, kInitializing = -0x40000001;
struct HeapDescriptors {
    HeapDescriptors() noexcept = default;
    HeapDescriptors(const HeapDescriptors&) = delete;
    HeapDescriptors& operator=(const HeapDescriptors&) = delete;
    HeapDescriptors(HeapDescriptors&&) = delete; // Stable sys singleton owns cached descriptors until module teardown.
    HeapDescriptors& operator=(HeapDescriptors&&) = delete;
    std::atomic<int> mCached{kUninitialized}, mUncached{kUninitialized};
    ~HeapDescriptors() noexcept
    {
        const int cached = mCached.load(std::memory_order_relaxed); // All users are quiescent before owner-registry teardown.
        const int uncached = mUncached.load(std::memory_order_relaxed);
        if (cached >= 0) ::close(cached);
        if (uncached >= 0) ::close(uncached);
    }
};
HeapDescriptors& descriptors() noexcept { static HeapDescriptors value; return value; }
int heapDescriptor(bool cached) noexcept
{
    auto& slot = cached ? descriptors().mCached : descriptors().mUncached;
    int value = slot.load(std::memory_order_acquire); // Observe the completed cold-path open.
    if (value == kUninitialized && slot.compare_exchange_strong(value, kInitializing, std::memory_order_acq_rel)) {
        int fd;
        do { fd = ::open(cached ? "/dev/dma_heap/system" : "/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC); }
        while (fd < 0 && errno == EINTR);
        value = fd < 0 ? -errno : fd;
        slot.store(value, std::memory_order_release); // Publish success or a sticky capability error, outside all application locks.
        return value;
    }
    while (value == kInitializing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Concurrent FIRST allocation only; never a tile hot path.
        value = slot.load(std::memory_order_acquire);
    }
    return value;
}
#endif
}

void au::sys::memory::initializeAllocators() noexcept
{
#if AU_OS_LINUX || AU_OS_ANDROID
    (void)descriptors();
#endif
}
int au::sys::memory::allocatePss(std::size_t size, void** out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    if (!size) return au::err::kErrorInvalidSize;
#if AU_OS_WINDOWS
    *out = ::_aligned_malloc(size, kAlignment);
    return *out ? 0 : au::err::kErrorNoMemory;
#else
    if (size <= kSmall) {
        const int code = ::posix_memalign(out, kAlignment, size);
        return code ? allocationError(code) : *out ? 0 : au::err::kErrorNoMemory;
    }
    void* pointer = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pointer == MAP_FAILED) return allocationError(errno);
    *out = pointer; return 0;
#endif
}
int au::sys::memory::releasePss(void* pointer, std::size_t size) noexcept
{
    if (!pointer) return au::err::kErrorNullPointer;
    if (!size) return au::err::kErrorInvalidSize;
#if AU_OS_WINDOWS
    ::_aligned_free(pointer); return 0;
#else
    if (size <= kSmall) { ::free(pointer); return 0; }
    return ::munmap(pointer, size) ? allocationError(errno) : 0;
#endif
}
int au::sys::memory::allocateDma(std::size_t size, bool cached, void** out, int* fd) noexcept
{
    if (!out || !fd) return au::err::kErrorNullPointer;
    *out = nullptr; *fd = -1;
    if (!size) return au::err::kErrorInvalidSize;
#if AU_OS_LINUX || AU_OS_ANDROID
    const int heap = heapDescriptor(cached);
    if (heap < 0) return allocationError(-heap);
    dma_heap_allocation_data allocation{};
    allocation.len = size; allocation.fd_flags = O_RDWR | O_CLOEXEC;
    int code;
    do { code = ::ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation); } while (code < 0 && errno == EINTR);
    if (code < 0) return allocationError(errno);
    const int handle = static_cast<int>(allocation.fd);
    void* pointer = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    if (pointer == MAP_FAILED) { const int error = errno; ::close(handle); return allocationError(error); }
    *out = pointer; *fd = handle; return 0;
#else
    (void)cached; return au::err::kErrorNotSupported;
#endif
}
int au::sys::memory::releaseDma(void* pointer, std::size_t size, int fd) noexcept
{
#if AU_OS_LINUX || AU_OS_ANDROID
    int code = pointer && ::munmap(pointer, size) ? allocationError(errno) : 0;
    if (fd >= 0) ::close(fd); // Never retry close: the number may already have been reused.
    return code;
#else
    (void)pointer; (void)size; (void)fd; return au::err::kErrorNotSupported;
#endif
}
