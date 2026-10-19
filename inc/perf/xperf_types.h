#ifndef AURA_PERF_XPERF_TYPES_H_
#define AURA_PERF_XPERF_TYPES_H_

/**
 * @file xperf_types.h
 * @brief Shared primitive types and compile-time constants for the au::perf subsystem.
 *
 * This header is intentionally C-compatible so that the C ABIs declared in
 * xtimer_api.h and xtracer_api.h can be consumed from pure-C translation units
 * without modification.
 *
 * Layout contract
 * ---------------
 * @c AuPerfScope is a 64-byte opaque POD that the caller allocates on the stack
 * (or as a class member). The implementation writes its private state into this
 * block via placement-new; callers must never inspect the raw bytes.
 *
 * The block is sized via @c AU_PERF_SCOPE_BYTES. Shrinking this value is an
 * ABI-breaking change; growing it is safe for source but breaks binary
 * compatibility with pre-built consumers.
 *
 * The underlying array uses @c uint64_t elements to guarantee 8-byte alignment,
 * which is required by the internal @c ScopeState objects that the implementation
 * placement-new's into it.
 */

#include <stdint.h>

// ----------------------------------------------------------------------------
//  Compile-time knobs — shared by C and C++ consumers
// ----------------------------------------------------------------------------

/// Hard-off sentinel — suppresses all scope output.
#define AU_PERF_LEVEL_OFF (-1)

/// Always-on sentinel — every scope passes the level gate.
#define AU_PERF_LEVEL_ALL (INT32_MAX)

/// Maximum nesting depth — scopes beyond this limit are silently dropped.
#define AU_PERF_HARD_MAX_DEPTH (512u)

/// Byte size of the opaque scope block (must be a multiple of 8).
#define AU_PERF_SCOPE_BYTES (64u)

// ----------------------------------------------------------------------------
//  AuPerfScope — caller-allocated opaque state block
// ----------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque, stack-allocatable state block for a single timer or tracer scope.
 *
 * Callers allocate one per scope and pass its address to the C ABI begin/end
 * functions. The implementation placement-new's its internal state into this
 * block; callers must never inspect the raw bytes.
 *
 * @c uint64_t elements guarantee 8-byte alignment on all platforms, satisfying
 * the @c std::chrono::time_point requirements of the internal @c ScopeState.
 *
 * @note sizeof(AuPerfScope) == AU_PERF_SCOPE_BYTES == 64 bytes.
 */
typedef struct AuPerfScope_
{
    uint64_t opaque[AU_PERF_SCOPE_BYTES / 8u]; ///< 8-byte aligned; never inspect directly.
} AuPerfScope;

#ifdef __cplusplus
} // extern "C"

// Bring the constants into the au::perf namespace for C++ consumers.
#include <cstdint>

namespace au {
namespace perf {

constexpr int32_t  LEVEL_OFF      = AU_PERF_LEVEL_OFF;
constexpr int32_t  LEVEL_ALL      = AU_PERF_LEVEL_ALL;
constexpr uint32_t HARD_MAX_DEPTH = AU_PERF_HARD_MAX_DEPTH;

/// C++ alias for the C ABI scope type. Use this in C++ code.
using PerfScope = AuPerfScope;

} // namespace perf
} // namespace au

#endif // __cplusplus

#endif // AURA_PERF_XPERF_TYPES_H_
