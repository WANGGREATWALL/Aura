#include "mm/native_backend.h"
#include "sys/memory_runtime.h"
#include "log/xerror.h"

namespace {
class NativeBackend final : public au::mm::IBackend {
public:
    au::mm::BackendId id() const noexcept override { return au::mm::BackendId::Native; }
    const char* name() const noexcept override { return "native"; }
    bool available() const noexcept override { return true; }
    int alloc(std::size_t size, au::mm::MemType type, au::mm::MemBlock& out) noexcept override
    {
        void* pointer = nullptr; int fd = -1;
        int code;
        switch (type) {
        case au::mm::MemType::Pss: code = au::sys::memory::allocatePss(size, &pointer); break;
        case au::mm::MemType::DmaCached: case au::mm::MemType::DmaUncached:
            code = au::sys::memory::allocateDma(size, type == au::mm::MemType::DmaCached, &pointer, &fd); break;
        default: return au::err::kErrorInvalidParam;
        }
        if (!code) out = au::mm::MemBlock{pointer, size, fd, type, au::mm::BackendId::Native};
        return code;
    }
    int release(const au::mm::MemBlock& block) noexcept override
    {
        switch (block.type) {
        case au::mm::MemType::Pss: return au::sys::memory::releasePss(block.ptr, block.size);
        case au::mm::MemType::DmaCached: case au::mm::MemType::DmaUncached:
            return au::sys::memory::releaseDma(block.ptr, block.size, block.fd);
        default: return au::err::kErrorInvalidParam;
        }
    }
    int syncCpuToDevice(const au::mm::MemBlock& block) noexcept override
    { return block.type == au::mm::MemType::Pss ? 0 : au::sys::memory::cpuAccess(block.fd, 3, false); }
    int syncDeviceToCpu(const au::mm::MemBlock& block) noexcept override
    { return block.type == au::mm::MemType::Pss ? 0 : au::sys::memory::cpuAccess(block.fd, 3, true); }
};
}
au::mm::IBackend& au::mm::nativeBackend() noexcept
{
    au::sys::memory::initializeAllocators();
    // Stable static identity, destroyed after Registry. No intentionally leaked
    // singleton allocation remains when an algorithm .so is unloaded.
    static NativeBackend instance;
    return instance;
}
