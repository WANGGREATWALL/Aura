#ifndef AURA_PERF_XTRACER5_H_
#define AURA_PERF_XTRACER5_H_

/**
 * @file xtracer5.h
 * @brief Consolidated Android Perfetto / ftrace trace_marker tracer (v5).
 *
 * On Android:
 *  - A single process-wide @c trace_marker fd is opened lazily with
 *    @c O_CLOEXEC on first use; the kernel reclaims it on process exit
 *    (no explicit close).
 *  - Each scope writes "B|pid|name" on construction and "E|pid" on
 *    destruction. Per-write atomicity is guaranteed by ftrace up to one
 *    page, so no userspace lock is required.
 *  - @c sub(name) / @c sub() emit additional begin/end pairs nested
 *    inside the current scope, allowing inline phase markers without
 *    nesting C++ lifetimes.
 *
 * On non-Android targets every body compiles away to nothing, leaving
 * no @c .text footprint and zero runtime cost.
 *
 * Decoupling note:
 *  @c XTracer5Scoped is intentionally independent of @c XTimer5Scoped.
 *  The composite macro @c AU_PERF5_SCOPE declares both with the same
 *  label, so the perfetto slice and the hierarchical log entry stay
 *  aligned without coupling the two implementations.
 *
 * Activation rules (evaluated once at construction):
 *  - global isEnabled() must be true
 *  - the scope's depth must be ≤ getTracerLevel()
 *  - the depth must be < @c kHardMaxDepth5 (internal safety net)
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "perf/xtimer5.h"

namespace au {
namespace perf {

/**
 * @brief RAII scoped Perfetto / ftrace tracer (Android-only payload).
 *
 * Name lifetime: the constructor copies @p name into a fixed inline
 * buffer; the caller may safely pass a temporary @c std::string.
 */
class XTracer5Scoped
{
public:
    explicit XTracer5Scoped(const std::string& name) noexcept;
    ~XTracer5Scoped() noexcept;

    XTracer5Scoped(const XTracer5Scoped&)            = delete;
    XTracer5Scoped& operator=(const XTracer5Scoped&) = delete;

    /// End the previous sub-slice (if any) and open a new one named @p name.
    /// No-op when this scope is inactive.
    void sub(const std::string& name) noexcept;

    /// End the previous sub-slice (if any). No-op otherwise.
    void sub() noexcept;

private:
    void begin(const std::string& name) noexcept;

    bool  mActive;
    bool  mSubOpen;

    /// Fixed inline buffer; longer names are truncated to fit.
    static constexpr std::size_t kMaxName = 128;
    char                         mName[kMaxName];
    uint8_t                      mNameLen;
};

}  // namespace perf
}  // namespace au

#endif  // AURA_PERF_XTRACER5_H_
