#ifndef AURA_MM_ACCESS_CONTEXT_H_
#define AURA_MM_ACCESS_CONTEXT_H_
#include <cstdint>
namespace au {
namespace mm {
namespace detail {
// Installed by an executor for a dynamic callable extent. mm has no dependency
// on flow or OpenCL; standalone users receive the same returned error codes.
struct AccessErrorSink {
    void (*mReport)(void*, int, int, std::uint64_t, bool) noexcept = nullptr;
    void* mPayload = nullptr;
    bool mMayWait = true; // Cleanup extents reject public access that would wait on an unfinished fence.
};
au::mm::detail::AccessErrorSink setAccessErrorSink(const au::mm::detail::AccessErrorSink& sink) noexcept;
bool accessMayWait() noexcept;
void reportAccessError(int code, int nativeCode = 0, std::uint64_t causeTaskId = 0, bool deviceError = false) noexcept;
} // namespace detail
} // namespace mm
} // namespace au
#endif
