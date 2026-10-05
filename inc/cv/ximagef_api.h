/**
 * @file ximagef_api.h
 * @brief VImageF helper – stable ABI free-function layer.
 *
 * Free-function API over the @c VImageF data structure defined in vivo_comdef.h.
 * Provides:
 *  - allocation / release through the au::mm backend (Pss, DmaUncached, DmaCached);
 *  - format-aware stride / scanline computation with caller-specified alignment;
 *  - DMA cache synchronisation primitives (invalidate / flush);
 *  - bidirectional conversion to / from the legacy V* image structures
 *    (@c VImage, @c VImageEx, @c VImageExV1);
 *  - a rich set of predicates (isValid / isFormat / isFormatIn / isSameXxx)
 *    and a compact info() string for diagnostics.
 *
 * All symbols reside in @c namespace @c au::cv.
 *
 * For an RAII C++ wrapper that owns the underlying memory automatically,
 * include "cv/ximagef.h" instead.
 *
 * Memory type mapping (au::mm::MemType → allocation behaviour):
 *  - MemType::Pss         : CPU-only pageable memory; fd == 0, fdOffset == 0.
 *  - MemType::DmaUncached : DMA-BUF bypassing CPU cache; sync calls are no-ops.
 *  - MemType::DmaCached   : DMA-BUF with CPU cache; requires explicit invalidate /
 *                          flush around CPU <=> device transfers.
 *
 * Ownership contract (read carefully):
 *  - createVImageF(...)   produces a VImageF that OWNS its memory.
 *                        The caller MUST eventually call destroyVImageF().
 *  - fromV*(...)         produces a VImageF that DOES NOT OWN its memory
 *                        (it is a view into the source structure).
 *                        NEVER call destroyVImageF() on such a VImageF.
 *  - destroyVImageF(...)  is idempotent against zero-cleared structures
 *                        but is otherwise undefined for foreign memory.
 *
 * @code
 *   using namespace au::cv;
 *
 *   // 1. Simple allocation (default; DMA cached, stride=8B, scanline=2 rows)
 *   VImageF img = createVImageF(1920, 1080, kVIFormatNV21_F16);
 *
 *   // 2. CPU writes -> device reads
 *   fillPixels(img);
 *   flushImageCache(img);
 *
 *   // 3. Hand img.fd[*] / img.fdOffset[*] to GPU / ISP / NPU
 *   sendToDevice(img);
 *
 *   // 4. Device writes -> CPU reads
 *   invalidateImageCache(img);
 *   readPixels(img);
 *
 *   // 5. Release
 *   destroyVImageF(img);
 * @endcode
 */

#ifndef AURA_CV_XIMAGEF_API_H_
#define AURA_CV_XIMAGEF_API_H_

#include <initializer_list>
#include <string>

#include "mm/xmemory.h"   // au::mm::MemType
#include "sys/xsystem.h"  // AU_API
#include "vivo_comdef.h"  // VImageF, VImage, VImageEx, VImageExV1, VImageFormat, VImageFormatF, VImageColorSpace, VDKResult

namespace au {
namespace cv {

// ============================================================================
// Default parameter values
//
// Single source of truth shared by both the free-function createVImageF() and
// the RAII Image ctor, so their default behaviour can never drift apart.
// ============================================================================

constexpr au::mm::MemType kDefaultMemType           = au::mm::MemType::DmaCached;
constexpr int            kDefaultColorSpace        = kVIColorSpaceDefault;
constexpr int            kDefaultStrideAlignBytes  = 8;
constexpr int            kDefaultScanlineAlignRows = 2;

// ============================================================================
// Allocation / release
// ============================================================================

/**
 * @brief Creates a VImageF with all tunable knobs exposed.
 *
 * Parameters sorted by descending usage frequency:
 *
 * @param width              image width in pixels  (must be > 0)
 * @param height             image height in pixels (must be > 0)
 * @param format             one of @c VImageFormat or @c VImageFormatF
 * @param memType            au::mm::MemType::Pss / DmaUncached / DmaCached
 * @param colorSpace         one of @c VImageColorSpace; does not affect layout
 * @param strideAlignBytes   per-row byte alignment; values <= 1 disable padding
 * @param scanlineAlignRows  row-count alignment; values <= 1 disable padding
 * @param tag                optional profiling label; nullptr -> default
 *
 * @return zero-cleared VImageF on failure (isValid() == false).
 *
 * @note Single-block allocation: ALL planes are carved out of one contiguous
 *       au::mm block laid out back-to-back. Every plane therefore shares the
 *       block's fd (fd[i] == fd[0]) and carries its real byte offset in
 *       fdOffset[i]; plane[0] is always at offset 0. Pss images have fd[i]==0
 *       (no DMA-BUF) but the per-plane data pointers / offsets remain valid.
 */
AU_API VImageF createVImageF(int             width,
                            int             height,
                            int             format,
                            au::mm::MemType memType           = kDefaultMemType,
                            int             colorSpace        = kDefaultColorSpace,
                            int             strideAlignBytes  = kDefaultStrideAlignBytes,
                            int             scanlineAlignRows = kDefaultScanlineAlignRows,
                            const char*     tag               = nullptr) noexcept;

/**
 * @brief Releases memory previously acquired by createVImageF().
 *
 * Idempotent against zero-cleared structures (planes whose data[i] is null
 * are silently skipped). All numeric fields are reset to zero on success.
 *
 * @warning Calling destroyVImageF() on a VImageF that did not originate from
 *          createVImageF() (e.g. one returned by fromV*()) is undefined
 *          behaviour; it would attempt to free memory the helper does not own.
 *
 * @return true if at least one plane was freed; false on null/invalid input.
 */
AU_API bool destroyVImageF(VImageF& img) noexcept;

// ============================================================================
// CPU cache synchronisation (DMA cached memory only)
// ============================================================================

/**
 * @brief Invalidate CPU cache for every plane of @p img.
 *
 * MUST be called BEFORE the CPU reads memory that a device has just written;
 * otherwise the CPU may observe stale cache lines.
 *
 * No-op (returns au::err::kSuccess) for Pss and DmaUncached memory.
 *
 * @return au::err::kSuccess on success, negative error code on failure.
 */
AU_API int invalidateImageCache(const VImageF& img) noexcept;

/**
 * @brief Flush (clean) CPU cache for every plane of @p img.
 *
 * MUST be called AFTER the CPU has written memory and BEFORE a device reads
 * it; otherwise the device may observe stale memory because dirty lines
 * still live in the CPU cache.
 *
 * No-op (returns au::err::kSuccess) for Pss and DmaUncached memory.
 *
 * @return au::err::kSuccess on success, negative error code on failure.
 */
AU_API int flushImageCache(const VImageF& img) noexcept;

// ============================================================================
// Predicates
// ============================================================================

/** @brief Minimal structural validity: positive size, registered format,
 *         plane[0] non-null with positive stride / scanline. */
AU_API bool isValid(const VImageF& img) noexcept;

/** @brief @c img.format == @p format. */
AU_API bool isFormat(const VImageF& img, int format) noexcept;

/** @brief @c img.format equals one of @p formats. */
AU_API bool isFormatIn(const VImageF& img, std::initializer_list<int> formats) noexcept;

/** @brief Strict identity: width, height, format, colorSpace, and per-plane
 *         stride / scanline / dataSize / fd / fdOffset / data pointer all equal. */
AU_API bool isSameWith(const VImageF& a, const VImageF& b) noexcept;

/**
 * @brief Geometry equality with format-aware stride checking.
 *
 * - If @p a.format == @p b.format: compares width, height AND per-plane
 *   stride. Same format implies a comparable memory layout; a stride
 *   mismatch would break plane-wise copy / in-place processing.
 * - If formats differ: compares width and height only; stride is not
 *   comparable across formats.
 *
 * Callers that need a copy-safe / in-place-safe destination MUST additionally
 * verify the format (use isSameFormatWith() or isSameSizeAndFormatWith()).
 */
AU_API bool isSameSizeWith(const VImageF& a, const VImageF& b) noexcept;

/** @brief Format equal (colorSpace not considered). */
AU_API bool isSameFormatWith(const VImageF& a, const VImageF& b) noexcept;

/** @brief Width, height and format all equal. */
AU_API bool isSameSizeAndFormatWith(const VImageF& a, const VImageF& b) noexcept;

// ============================================================================
// Diagnostics
// ============================================================================

/**
 * @brief Compact single-line description of @p img.
 *
 * Format:
 *   [WxH|stride0,stride1,stride2,stride3], fmt:string, data:[ptr0,ptr1,ptr2,ptr3], fd:[fd0,fd1], offset:[off0,off1]
 *
 * Example:
 *   [1920x1080|3840,1920,0,0], fmt:NV12, data:[0x7fab1000,0x7fab2000,(null),(null)], fd:[12,13], offset:[0,0]
 */
AU_API std::string info(const VImageF& img) noexcept;

// ============================================================================
// Compatibility conversions – VImageF -> legacy V* (unowned views)
// ============================================================================

// All to*() functions return a structure that VIEWS into @p src. The returned
// structure shares its data pointers / fds with @p src; do not free it
// independently and do not let it outlive @p src.

/** @brief Project a VImageF onto the minimal @c VImage layout. */
AU_API VImage toVImage(const VImageF& src) noexcept;

/** @brief Project a VImageF onto @c VImageEx (pNativeHandle=null). */
AU_API VImageEx toVImageEx(const VImageF& src) noexcept;

/** @brief Project a VImageF onto @c VImageExV1 (with fdOffset, pNativeHandle=null). */
AU_API VImageExV1 toVImageExV1(const VImageF& src) noexcept;

// ============================================================================
// Compatibility conversions – legacy V* -> VImageF (unowned views)
// ============================================================================

// All from*() functions return a VImageF that VIEWS into @p src. The returned
// VImageF DOES NOT own its memory. NEVER call destroyVImageF() on it.

AU_API VImageF fromVImage(const VImage& src) noexcept;
AU_API VImageF fromVImageEx(const VImageEx& src) noexcept;
AU_API VImageF fromVImageExV1(const VImageExV1& src) noexcept;

} /* namespace cv */
} /* namespace au */

#endif /* AURA_CV_XIMAGEF_API_H_ */
