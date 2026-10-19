/**
 * @file opencl_registry.h
 * @brief Process-wide weak cache mapping a dmabuf fd to its OpenCLBuffer.
 *
 * The import atom is one dmabuf (one fd), not one plane: a multi-plane image
 * that shares a single fd is imported exactly once and its planes are addressed
 * by offset inside the kernel. The registry keys on the raw fd, so the same
 * logic transparently handles both single-fd (shared) and per-plane-fd images —
 * shared fd collapses to one entry; independent fds map to several.
 *
 * Entries are std::weak_ptr: the registry never extends lifetime. When no
 * shared_ptr<OpenCLBuffer> is held by callers or by the wrapper's pending list the
 * slot expires and is reclaimed lazily on the next lookup.
 */

#ifndef AU_GPU_OPENCL_REGISTRY_H_
#define AU_GPU_OPENCL_REGISTRY_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace au {
namespace cv {
class Image;
}
namespace gpu {

class OpenCLBuffer;

/// Singleton fd-keyed cache. Concurrent lookups for the same fd de-duplicate.
class OpenCLRegistry
{
public:
    static OpenCLRegistry& get() noexcept;

    /**
     * @brief Returns a shared OpenCLBuffer for @p fd; creates one on first lookup.
     *
     * @param fd      dmabuf file descriptor (must be > 0).
     * @param hostPtr mapped host pointer (plane 0 base) for cache sync; may be null.
     * @param size    total dmabuf byte size to import.
     * @return non-null on success; nullptr if fd invalid or dup() fails.
     */
    std::shared_ptr<OpenCLBuffer> getOrCreate(int fd, void* hostPtr, std::size_t size) noexcept;

    /// Drops expired entries. Optional; getOrCreate self-prunes. Exposed for tests.
    void purgeExpired() noexcept;

    /// Number of currently-tracked entries (alive or expired). For tests.
    std::size_t size() const noexcept;

private:
    OpenCLRegistry()  = default;
    ~OpenCLRegistry() = default;

    OpenCLRegistry(const OpenCLRegistry&)            = delete;
    OpenCLRegistry& operator=(const OpenCLRegistry&) = delete;

    mutable std::mutex                                   mMutex;
    std::unordered_map<int, std::weak_ptr<OpenCLBuffer>>   mMap;
};

/**
 * @brief Convenience: import @p img (VImageF) as a single dmabuf-backed OpenCLBuffer.
 *
 * Requires img.fd[0] > 0. The import size is the full dmabuf span, computed as
 * the max over planes of (fdOffset[i] + dataSize[i]); this is correct for both
 * single-fd multi-plane and single-plane images.
 *
 * @return non-null shared handle on success; nullptr for non-DMA images.
 */
std::shared_ptr<OpenCLBuffer> importImage(const cv::Image& img) noexcept;

} // namespace gpu
} // namespace au

#endif // AU_GPU_OPENCL_REGISTRY_H_
