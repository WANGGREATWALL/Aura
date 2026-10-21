#include "flow/core.h"
#include "mm/registry.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
Core::CleanupExtent::CleanupExtent(Core* core, Node* node) noexcept : mFrame{node, gFrame}, mErrors(core, false)
{
    gFrame = node ? &mFrame : nullptr;
    ++gCleanupDepth;
}
Core::CleanupExtent::~CleanupExtent() noexcept
{
    --gCleanupDepth;
    gFrame = mFrame.mPrevious;
}
void initializeMemoryContext() noexcept
{
    // Construct first; structured static cleanup must outlive all Core work.
    (void)au::mm::Registry::instance();
}
ExecutionErrors::ExecutionErrors(Core* core, bool mayWait) noexcept
    : mPrevious(au::mm::detail::setAccessErrorSink(
          {[](void* payload, int code, int native, std::uint64_t cause, bool device) noexcept {
               static_cast<Core*>(payload)->scopedError(
                   code, device ? au::flow::ErrorDomain::OpenCL : au::flow::ErrorDomain::Memory, native, cause);
           },
           core, mayWait}))
{
}
ExecutionErrors::~ExecutionErrors() noexcept { restore(); }
void ExecutionErrors::restore() noexcept
{
    if (!mInstalled)
        return;
    au::mm::detail::setAccessErrorSink(mPrevious);
    mInstalled = false;
}
thread_local Frame*       gFrame           = nullptr;
thread_local int          gWorker          = -1;
thread_local unsigned int gDepth           = 0;
thread_local unsigned int gTileDepth       = 0;
thread_local TileContext* gTileContext     = nullptr;
thread_local unsigned int gCleanupDepth    = 0;
thread_local Node*        gFinish          = nullptr;
thread_local bool         gFinishing       = false;
thread_local Node*        gReclaim         = nullptr;
thread_local bool         gReclaiming      = false;
thread_local unsigned int gCompletionBurst = 0;

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
