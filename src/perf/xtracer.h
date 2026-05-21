#ifndef AURA_PERF_XTRACER_H_
#define AURA_PERF_XTRACER_H_

/**
 * @file xtracer.h
 * @brief Android Perfetto / ftrace trace_marker tracer.
 *
 * On Android:
 *  - A single process-wide @c trace_marker fd is opened lazily with
 *    @c O_CLOEXEC on first use; the kernel reclaims it on process exit.
 *  - Each scope writes "B|pid|name" on construction and "E|pid" on
 *    destruction. Per-write atomicity is guaranteed by ftrace (≤ 1 page).
 *  - @c sub(name) / @c sub() emit additional begin/end pairs nested
 *    inside the current scope.
 *
 * On non-Android targets every body compiles away to nothing.
 *
 * @c XTracerScoped is intentionally independent of @c XTimerScoped.
 * The composite macro @c AU_PERF_SCOPE declares both with the same label.
 *
 * Activation gates: global @c isEnabled(), depth ≤ @c getTracerLevel(),
 * depth < @c kHardMaxDepth.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "perf/xtimer.h"

namespace au {
namespace perf {

/**
 * @brief RAII scoped Perfetto / ftrace tracer (Android-only payload).
 */
class XTracerScoped
{
public:
    explicit XTracerScoped(const std::string& name) noexcept;
    ~XTracerScoped() noexcept;

    XTracerScoped(const XTracerScoped&)            = delete;
    XTracerScoped& operator=(const XTracerScoped&) = delete;

    void sub(const std::string& name) noexcept;
    void sub() noexcept;

private:
    void begin(const std::string& name) noexcept;

    bool  mActive;
    bool  mSubOpen;

    static constexpr std::size_t kMaxName = 128;
    char                         mName[kMaxName];
    uint8_t                      mNameLen;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTRACER_H_
