#ifndef AURA_MM_BACKEND_H_
#define AURA_MM_BACKEND_H_

#include "mm/xmemory.h"

namespace au {
namespace mm {

/// Internal abstraction; never exposed via inc/.
class IBackend
{
public:
    virtual ~IBackend() = default;

    virtual BackendId id() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual bool available() const noexcept = 0;

    virtual int alloc(size_t size, MemType type, MemBlock& out) noexcept = 0;
    virtual int release(const MemBlock& block) noexcept = 0;
    virtual int syncCpuToDevice(const MemBlock& block) noexcept = 0;
    virtual int syncDeviceToCpu(const MemBlock& block) noexcept = 0;
};

/// Backend that the next alloc() should use (resolves Auto to Pool if available, else Native).
IBackend& resolve() noexcept;

/// Backend that owns @p block (used by free/sync/query to dispatch by tag).
IBackend& forBackend(BackendId id) noexcept;

/// Apply explicit runtime override. Auto reverts to the default resolution logic.
void setSelection(BackendId id) noexcept;

/// Read the currently-effective selector (after auto resolution).
BackendId effectiveSelection() noexcept;

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_BACKEND_H_
