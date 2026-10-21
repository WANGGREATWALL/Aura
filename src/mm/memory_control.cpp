#include "mm/memory_control.h"
#include "mm/backend.h"
#include "mm/access_context.h"
#include "log/xlogger.h"
#include "log/xerror.h"

au::mm::detail::MemoryControl::~MemoryControl() noexcept
{
    mFence.reset();
    mExternalOwner.reset();
    const int code = mOwnsStorage ? au::mm::forBackend(mBlock.backend).release(mBlock) : 0;
    au::sys::memory::closeFd(mOwnedFd);
    mReport->mCode = code;
    mReport->mDone.store(true, std::memory_order_release); // Publish cleanup outcome after backing storage release.
    au::mm::detail::reportAccessError(code);
}
au::mm::detail::ExternalOwner::~ExternalOwner() noexcept
{
    if (!mRelease) return;
    try { mRelease(mPayload); }
    catch (...) { XLOG_E("au::mm external owner release threw; SDK boundary contained it\n"); }
}
const au::mm::MemBlock& au::mm::detail::MemoryLease::block() const noexcept
{
    static const au::mm::MemBlock empty;
    return mControl ? mControl->mBlock : empty;
}
