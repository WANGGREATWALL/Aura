#include "mm/backend.h"

#include <atomic>

#include "log/xerror.h"
#include "log/xlogger.h"
#include "mm/native_backend.h"
#include "mm/pool_backend.h"

namespace au {
namespace mm {

namespace {

// Why atomic: setSelection / resolve race across threads; both must see a
// consistent BackendId without taking a lock on the hot path.
std::atomic<BackendId> gSelected{BackendId::Auto};

IBackend& resolveAuto() noexcept
{
    // Pool is currently a stub with available()==false, so Auto selects Native.
    if (poolBackend().available()) {
        return poolBackend();
    }
    return nativeBackend();
}

}  // namespace

IBackend& resolve() noexcept
{
    BackendId sel = gSelected.load(std::memory_order_acquire);
    if (sel == BackendId::Auto) {
        return resolveAuto();
    }
    if (sel == BackendId::Pool) {
        return poolBackend();
    }
    return nativeBackend();
}

IBackend& forBackend(BackendId id) noexcept
{
    // free/sync/query dispatch by the tag stored in MemBlock; if the originating
    // backend has been disabled at runtime we still reach into it directly so
    // resources owned by it are not leaked.
    if (id == BackendId::Pool) {
        return poolBackend();
    }
    return nativeBackend();
}

void setSelection(BackendId id) noexcept
{
    gSelected.store(id, std::memory_order_release);
}

BackendId effectiveSelection() noexcept
{
    return resolve().id();
}

}  // namespace mm
}  // namespace au
