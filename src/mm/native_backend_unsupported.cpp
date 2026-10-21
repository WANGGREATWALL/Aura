#include "mm/native_backend.h"

#include "log/xerror.h"

namespace au {
namespace mm {
namespace {

class NativeBackendUnavailable final : public IBackend
{
public:
    BackendId id() const noexcept override { return BackendId::Native; }
    const char* name() const noexcept override { return "native"; }
    bool available() const noexcept override { return false; }

    int alloc(size_t, MemType, MemBlock& out) noexcept override
    {
        out = MemBlock{};
        return au::err::kErrorNotSupported;
    }
    int release(const MemBlock&) noexcept override { return au::err::kErrorNotSupported; }
    int syncCpuToDevice(const MemBlock&) noexcept override
    {
        return au::err::kErrorNotSupported;
    }
    int syncDeviceToCpu(const MemBlock&) noexcept override
    {
        return au::err::kErrorNotSupported;
    }
};

}  // namespace

IBackend& nativeBackend() noexcept
{
    static NativeBackendUnavailable instance;
    return instance;
}

}  // namespace mm
}  // namespace au
