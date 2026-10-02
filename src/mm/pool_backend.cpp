#include "mm/pool_backend.h"

#include "log/xerror.h"
#include "log/xlogger.h"

namespace au {
namespace mm {
namespace {

int unsupported(const char* operation) noexcept
{
    XLOG_E("au::mm::Pool backend %s is not implemented; use BackendId::Native or Auto\n",
           operation);
    return au::err::kErrorNotSupported;
}

class PoolBackend final : public IBackend
{
public:
    BackendId id() const noexcept override { return BackendId::Pool; }
    const char* name() const noexcept override { return "pool"; }
    bool available() const noexcept override { return false; }

    int alloc(size_t, MemType, MemBlock& out) noexcept override
    {
        out = MemBlock{};
        return unsupported("allocation");
    }

    int release(const MemBlock&) noexcept override { return unsupported("release"); }
    int syncCpuToDevice(const MemBlock&) noexcept override
    {
        return unsupported("CPU-to-device sync");
    }
    int syncDeviceToCpu(const MemBlock&) noexcept override
    {
        return unsupported("device-to-CPU sync");
    }
};

}  // namespace

IBackend& poolBackend() noexcept
{
    static PoolBackend instance;
    return instance;
}

}  // namespace mm
}  // namespace au
