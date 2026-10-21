#include "gpu/xopencl_api.h"
#include "mm/registry.h"
#include "log/xerror.h"
extern "C" int au_gpu_buffer_offset(const ::au_mm_access_set* access, std::size_t* offset) noexcept
{
    if (!offset || !access) return au::err::kErrorNullPointer;
    *offset = 0;
    if (access->mStructSize != sizeof(*access)) return au::err::kErrorVersionMismatch;
    if (access->mCode) return access->mCode;
    if (!access->mCount || access->mCount > 4) return au::err::kErrorInvalidParam;
    au::mm::detail::MemoryLease first;
    for (std::size_t index = 0; index < access->mCount; ++index) {
        au::mm::detail::MemoryLease lease;
        const int code = au::mm::Registry::instance().acquire(access->mRanges[index].mData, access->mRanges[index].mSize, lease);
        if (code) return code;
        if (!index) first = std::move(lease);
        else if (lease.control() != first.control()) return au::err::kErrorNotSupported;
    }
    *offset = first.offset(); return 0;
}
