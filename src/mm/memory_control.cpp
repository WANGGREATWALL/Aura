#include "mm/memory_control.h"
#include "mm/backend.h"
#include "log/xlogger.h"

au::mm::detail::MemoryControl::~MemoryControl() noexcept
{
    if (!mOwnsStorage) return;
    const int code = au::mm::forBackend(mBlock.backend).release(mBlock);
    mReport->mCode = code;
    mReport->mDone.store(true, std::memory_order_release); // Publish cleanup outcome after backing storage release.
    if (code) XLOG_E("au::mm deferred release failed allocation=%llu code=%d\n",
                    static_cast<unsigned long long>(mIdentity.mAllocationId), code);
}
const au::mm::MemBlock& au::mm::detail::MemoryLease::block() const noexcept
{
    static const au::mm::MemBlock empty;
    return mControl ? mControl->mBlock : empty;
}
