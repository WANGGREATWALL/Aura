#include "gpu/xopencl_api.h"
#include <cstddef>
#include <cstdint>
#include "log/xerror.h"
#include "flow/completion.h"
namespace {
int unavailable(bool startsOrWaits = false) noexcept
{
    if (startsOrWaits && au::flow::detail::cleanupActive()) {
        au::flow::detail::recordExternalError({au::flow::kErrorWouldDeadlock, 0, au::flow::ErrorDomain::Flow});
        return au::flow::kErrorWouldDeadlock;
    }
    if (au::flow::detail::currentTaskId()) au::flow::detail::recordExternalError(
        {au::err::kErrorNotSupported, 0, au::flow::ErrorDomain::OpenCL});
    return au::err::kErrorNotSupported;
}
}
extern "C" std::uint32_t au_gpu_opencl_version() noexcept { return AURA_GPU_OPENCL_ABI_VERSION; }
extern "C" int au_gpu_available(std::uint32_t) noexcept { return 0; }
extern "C" int au_gpu_context(std::uint32_t, void** context) noexcept
{ if (context) *context = nullptr; return unavailable(); }
extern "C" int au_gpu_init(std::uint32_t, const char*, int) noexcept { return unavailable(true); }
extern "C" int au_gpu_deinit(std::uint32_t) noexcept { return unavailable(true); }
extern "C" int au_gpu_prepare(std::uint32_t, const char*, const char*, const char*) noexcept { return unavailable(true); }
extern "C" int au_gpu_enqueue(std::uint32_t, const ::au_gpu_range*, const char*, const char*, const ::au_gpu_arg*,
                               std::size_t, void** event) noexcept
{ if (event) *event = nullptr; return unavailable(true); }
extern "C" int au_gpu_flush(std::uint32_t) noexcept { return unavailable(); }
extern "C" int au_gpu_finish(std::uint32_t) noexcept { return unavailable(true); }
extern "C" int au_gpu_event_release(std::uint32_t, void*) noexcept { return unavailable(); }
