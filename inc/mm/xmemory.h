#ifndef AURA_XMEMORY_H_
#define AURA_XMEMORY_H_

/**
 * @file memory/xmemory.h
 * @brief Unified memory allocation API for Aura, with two interchangeable backends.
 *
 * Two backends share the same public API and may coexist in a single process:
 *   - Pool   : wraps libvivo.mempool.so (CombineMemPool); preserves vendor-tuned paths.
 *   - Native : zero third-party deps; posix_memalign / mmap / dma-heap UAPI.
 *
 * Backend resolution priority (highest to lowest):
 *   1. au::mm::setBackend(BackendId)            : explicit runtime override.
 *   2. default                                  : Pool (falls back to Native if vendor SO unavailable).
 *
 * setBackend() only affects future allocations. Each MemBlock carries the BackendId it was allocated from,
 * so free / sync / query always dispatch to the originating backend even after a switch.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "sys/xsystem.h"  // AU_API

namespace au {
namespace mm {

/// Memory provenance categories the public API exposes.
enum class MemType : int
{
    Pss         = 0,  ///< Pageable system storage; CPU-only, fd == -1.
    DmaCached   = 1,  ///< dma-buf with CPU cache; needs explicit sync.
    DmaUncached = 2,  ///< dma-buf bypassing CPU cache; sync is a no-op.
};

/// Selector for the backend that performs the allocation.
enum class BackendId : int
{
    Auto   = 0,  ///< Pool if available, otherwise Native.
    Pool   = 1,  ///< libvivo.mempool.so backend.
    Native = 2,  ///< NDK + Linux UAPI backend.
};

/// Plain view of a managed allocation. Returned by query() and stored inside XMemory.
struct MemBlock
{
    void*     ptr     = nullptr;
    size_t    size    = 0;
    int       fd      = -1;
    MemType   type    = MemType::Pss;
    BackendId backend = BackendId::Auto;
};

// ---------------------------------------------------------------------------
//  Free functions
// ---------------------------------------------------------------------------

/// Allocate a block. On success, @p out is filled and the block is registered.
/// @return au::err::kSuccess or a negative error code.
AU_API int alloc(size_t size, MemType type, MemBlock& out) noexcept;

/// Release a block previously returned by alloc(). Safe to call with nullptr (returns kErrorNullPointer).
AU_API int free(void* ptr) noexcept;

/// Flush CPU cache lines for @p ptr so the device sees the latest CPU writes.
AU_API int syncCpuToDevice(void* ptr) noexcept;

/// Invalidate CPU cache lines for @p ptr so the next CPU read fetches device-written data.
AU_API int syncDeviceToCpu(void* ptr) noexcept;

/// Look up a managed pointer's metadata.
AU_API int query(void* ptr, MemBlock& out) noexcept;

/// True if @p ptr was returned by alloc() and has not been freed.
AU_API bool isManaged(void* ptr) noexcept;

// ---------------------------------------------------------------------------
//  Backend selection
// ---------------------------------------------------------------------------

/// Override the backend used for *future* allocations. BackendId::Auto reverts to the default resolution.
/// Already-allocated blocks keep their original backend; switching mid-stream is therefore safe.
AU_API int setBackend(BackendId id) noexcept;

/// Backend that the next alloc() call will use.
AU_API BackendId currentBackend() noexcept;

/// Stable, human-readable name (for logs). Returns "auto" / "pool" / "native" / "unknown".
AU_API const char* backendName(BackendId id) noexcept;

// ---------------------------------------------------------------------------
//  RAII wrapper (move-only)
// ---------------------------------------------------------------------------

/**
 * @brief Move-only RAII wrapper around a single MemBlock.
 *
 * Construction never throws. On allocation failure the object is left in the
 * default-constructed state with valid() == false; callers should check
 * valid() before using data().
 */
class AU_API XMemory
{
public:
    XMemory() = default;
    explicit XMemory(size_t size, MemType type = MemType::Pss);
    ~XMemory();

    XMemory(const XMemory&)            = delete;
    XMemory& operator=(const XMemory&) = delete;

    XMemory(XMemory&& other) noexcept;
    XMemory& operator=(XMemory&& other) noexcept;

    void*     data() const noexcept { return mBlock.ptr; }
    size_t    size() const noexcept { return mBlock.size; }
    int       fd() const noexcept { return mBlock.fd; }
    MemType   type() const noexcept { return mBlock.type; }
    BackendId backend() const noexcept { return mBlock.backend; }
    bool      valid() const noexcept { return mBlock.ptr != nullptr; }

    int syncCpuToDevice() noexcept;
    int syncDeviceToCpu() noexcept;

    std::string info() const;

private:
    MemBlock mBlock{};
};

}  // namespace mm
}  // namespace au

#endif  // AURA_XMEMORY_H_