#ifndef AURA_SYS_MEMORY_RUNTIME_H_
#define AURA_SYS_MEMORY_RUNTIME_H_
#include <cstddef>
#include <cstdint>
namespace au {
namespace sys {
namespace memory {
// Initialize only lightweight lifetime state; no allocation, thread or heap fd
// is opened. The owner registry constructs this before its own static lifetime.
void initializeAllocators() noexcept;
int allocatePss(std::size_t size, void** out) noexcept;
int releasePss(void* pointer, std::size_t size) noexcept;
int allocateDma(std::size_t size, bool cached, void** out, int* fd) noexcept;
int releaseDma(void* pointer, std::size_t size, int fd) noexcept;
struct FdIdentity {
    std::uint64_t mDevice = 0;
    std::uint64_t mInode = 0;
    bool operator<(const au::sys::memory::FdIdentity& other) const noexcept
    { return mDevice < other.mDevice || (mDevice == other.mDevice && mInode < other.mInode); }
};
int cpuAccess(int fd, unsigned int mode, bool start, int* nativeCode = nullptr) noexcept;
int duplicateFd(int fd, std::size_t size, int& owned, au::sys::memory::FdIdentity& identity) noexcept;
void closeFd(int fd) noexcept;
#ifdef AURA_MM_TESTING
typedef int (*AccessHook)(int, unsigned int, bool, int*) noexcept;
void setAccessHook(au::sys::memory::AccessHook hook) noexcept;
#endif
} // namespace memory
} // namespace sys
} // namespace au
#endif
