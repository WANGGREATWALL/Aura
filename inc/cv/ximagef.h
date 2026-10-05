/**
 * @file ximagef.h
 * @brief Header-only C++ RAII wrapper around the stable ABI in ximagef_api.h.
 *
 * Resides in namespace au::cv.
 *
 * @code
 *   #include "cv/ximagef.h"
 *
 *   // Owned allocation – default: DMA cached, stride=8B, scanline=2 rows
 *   au::cv::Image img(1920, 1080, kVIFormatNV21_F16);
 *   if (!img.isValid()) { ... }
 *
 *   // Owned allocation – all parameters exposed
 *   au::cv::Image img2(1920, 1080, kVIFormatNV21_F16,
 *                      au::mm::MemType::DmaCached,
 *                      kVIColorSpaceDefault, 64, 2,  // strideAlignBytes, scanlineAlignRows
 *                      "my_image");
 *
 *   // Access members directly via inherited VImageF fields
 *   void* planeY = img.data[0];
 *   int   w      = img.width;
 *
 *   // Non-owning view from a legacy VImage
 *   VImage        legacy = ...;
 *   au::cv::Image view(legacy);   // view does NOT own memory
 *
 *   // Implicit conversion back to VImageF and legacy types
 *   VImageF     f0 = img;
 *   VImage      v0 = img;
 *   VImageEx    v1 = img;
 *   VImageExV1  v2 = img;
 *
 *   // Cache management for DMA cached memory
 *   img.flushCache();       // CPU wrote, device will read
 *   img.invalidateCache(); // device wrote, CPU will read
 *
 *   // Memory released automatically when 'img' goes out of scope
 * @endcode
 *
 * Ownership rules:
 *   - Objects constructed with (width, height, format[, …]) OWN their memory (mNeedDestroy=true).
 *   - Copy-constructed from VImage / VImageEx / VImageExV1 / VImageF / Image:
 *     NON-OWNING VIEW (mNeedDestroy=false); must not outlive the source.
 *   - Move construction/assignment from Image: TRANSFERS ownership from the source,
 *     EXCEPT when source and destination are aliased (share the same underlying
 *     buffer pointer). An alias view CANNOT be promoted to owner via std::move;
 *     such operations are detected by the buffer-equality guard and become no-ops.
 *     This preserves the single-owner invariant required by RAII.
 */

#ifndef AURA_CV_XIMAGEF_H_
#define AURA_CV_XIMAGEF_H_

#include <initializer_list>
#include <string>
#include <utility>

#include "cv/ximagef_api.h"

namespace au {
namespace cv {

// ============================================================================
// Image – RAII owner of a VImageF, inherits VImageF for direct field access
// ============================================================================

/**
 * @brief Owning RAII wrapper for a VImageF, publicly inheriting VImageF so
 *        that all VImageF fields (data, width, height, stride, fd, …) are
 *        accessible directly on the Image object.
 *
 * Lifetime model:
 *   - Construct with (width, height, format[, …]): produces an owned image (mNeedDestroy=true).
 *   - Copy-construct / copy-assign from VImage / VImageEx / VImageExV1 / VImageF / Image:
 *     produces a NON-OWNING VIEW (mNeedDestroy=false).
 *   - Move-construct / move-assign from another Image: TRANSFERS OWNERSHIP.
 *   - Destructor calls destroyVImageF() if mNeedDestroy is true.
 *
 * @warning Do NOT call destroyVImageF() directly on an Image object: the
 *          destructor handles release. Calling it externally while the Image
 *          still has mNeedDestroy=true leads to a double-free.
 */
class Image final : public VImageF
{
public:
    // ------------------------------------------------------------------------
    // Constructors – owned allocation
    // ------------------------------------------------------------------------

    /**
     * @brief Allocates a new image with all knobs exposed.
     *        Parameters ordered by descending usage frequency.
     *
     * @param width              Image width in pixels.
     * @param height             Image height in pixels.
     * @param format             Pixel format (VImageFormat or VImageFormatF).
     * @param memType            Memory type (Pss / DmaUncached / DmaCached).
     * @param colorSpace         Color space tag stored in the descriptor.
     * @param strideAlignBytes   Row-stride alignment in bytes.
     * @param scanlineAlignRows  Scanline alignment in rows.
     * @param tag                Optional debug label (shallow copy of pointer).
     */
    Image(int             width,
          int             height,
          int             format,
          au::mm::MemType memType           = kDefaultMemType,
          int             colorSpace        = kDefaultColorSpace,
          int             strideAlignBytes  = kDefaultStrideAlignBytes,
          int             scanlineAlignRows = kDefaultScanlineAlignRows,
          const char*     tag               = nullptr) noexcept
        : VImageF(createVImageF(width, height, format, memType, colorSpace, strideAlignBytes, scanlineAlignRows, tag)),
          mNeedDestroy(::au::cv::isValid(*this))
    {
    }

    // ------------------------------------------------------------------------
    // Default constructor – empty, non-owning
    // ------------------------------------------------------------------------

    Image() noexcept : VImageF{}, mNeedDestroy(false) {}

    // ------------------------------------------------------------------------
    // Copy construction from legacy types – NON-OWNING VIEWS
    // ------------------------------------------------------------------------

    /**
     * @brief Copies all VImageF fields from @p other but does NOT take
     *        ownership of the memory (mNeedDestroy=false). The copy must not
     *        outlive @p other.
     */
    Image(const Image& other) noexcept : VImageF(other), mNeedDestroy(false) {}

    /** @brief Non-owning view over a VImage. Must not outlive @p src. */
    explicit Image(const VImage& src) noexcept : VImageF(fromVImage(src)), mNeedDestroy(false) {}

    /** @brief Non-owning view over a VImageF. Must not outlive @p src. */
    explicit Image(const VImageF& src) noexcept : VImageF(src), mNeedDestroy(false) {}

    /** @brief Non-owning view over a VImageEx. Must not outlive @p src. */
    explicit Image(const VImageEx& src) noexcept : VImageF(fromVImageEx(src)), mNeedDestroy(false) {}

    /** @brief Non-owning view over a VImageExV1. Must not outlive @p src. */
    explicit Image(const VImageExV1& src) noexcept : VImageF(fromVImageExV1(src)), mNeedDestroy(false) {}

    // ------------------------------------------------------------------------
    // Copy assignment from legacy types – NON-OWNING VIEWS
    // ------------------------------------------------------------------------

    Image& operator=(const Image& other) noexcept
    {
        if (this == &other || (data[0] != nullptr && data[0] == other.data[0])) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = other;
        mNeedDestroy                = false;
        return *this;
    }

    Image& operator=(const VImage& src) noexcept
    {
        if (data[0] != nullptr && data[0] == src.data[0]) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = fromVImage(src);
        mNeedDestroy                = false;
        return *this;
    }

    Image& operator=(const VImageF& src) noexcept
    {
        if (data[0] != nullptr && data[0] == src.data[0]) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = src;
        mNeedDestroy                = false;
        return *this;
    }

    Image& operator=(const VImageEx& src) noexcept
    {
        if (data[0] != nullptr && data[0] == src.image.data[0]) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = fromVImageEx(src);
        mNeedDestroy                = false;
        return *this;
    }

    Image& operator=(const VImageExV1& src) noexcept
    {
        if (data[0] != nullptr && data[0] == src.image.data[0]) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = fromVImageExV1(src);
        mNeedDestroy                = false;
        return *this;
    }

    // ------------------------------------------------------------------------
    // Move construction / assignment from Image – TRANSFERS ownership
    // ------------------------------------------------------------------------

    Image(Image&& other) noexcept : VImageF(other), mNeedDestroy(other.mNeedDestroy)
    {
        static_cast<VImageF&>(other) = VImageF{};
        other.mNeedDestroy         = false;
    }

    Image& operator=(Image&& other) noexcept
    {
        if (this == &other || (data[0] != nullptr && data[0] == other.data[0])) {
            return *this;
        }
        reset();
        static_cast<VImageF&>(*this) = other;
        mNeedDestroy               = other.mNeedDestroy;
        static_cast<VImageF&>(other) = VImageF{};
        other.mNeedDestroy         = false;
        return *this;
    }

    // ------------------------------------------------------------------------
    // Destructor
    // ------------------------------------------------------------------------

    ~Image() noexcept { reset(); }

    // ------------------------------------------------------------------------
    // Lifecycle control
    // ------------------------------------------------------------------------

    /** Releases owned memory (if any) and zero-clears all VImageF fields. */
    void reset() noexcept
    {
        if (mNeedDestroy) {
            (void)destroyVImageF(static_cast<VImageF&>(*this));
            mNeedDestroy = false;
        }
        static_cast<VImageF&>(*this) = VImageF{};
    }

    // ------------------------------------------------------------------------
    // Cache management (DmaCached only; no-op for Pss and DmaUncached)
    // ------------------------------------------------------------------------

    /** @brief See invalidateImageCache(). Returns au::err::kSuccess on success. */
    int invalidateCache() const noexcept { return invalidateImageCache(*this); }

    /** @brief See flushImageCache(). Returns au::err::kSuccess on success. */
    int flushCache() const noexcept { return flushImageCache(*this); }

    // ------------------------------------------------------------------------
    // Predicates
    // ------------------------------------------------------------------------

    bool isValid() const noexcept { return ::au::cv::isValid(*this); }

    bool isFormat(int fmt) const noexcept { return ::au::cv::isFormat(*this, fmt); }

    bool isFormatIn(std::initializer_list<int> formats) const noexcept { return ::au::cv::isFormatIn(*this, formats); }

    bool isSameWith(const VImageF& other) const noexcept { return ::au::cv::isSameWith(*this, other); }

    /** @brief Geometry equality; when formats match, per-plane stride is also
     *         compared. See isSameSizeWith() for the full contract. */
    bool isSameSizeWith(const VImageF& other) const noexcept { return ::au::cv::isSameSizeWith(*this, other); }

    bool isSameFormatWith(const VImageF& other) const noexcept { return ::au::cv::isSameFormatWith(*this, other); }

    bool isSameSizeAndFormatWith(const VImageF& other) const noexcept
    {
        return ::au::cv::isSameSizeAndFormatWith(*this, other);
    }

    // ------------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------------

    std::string info() const noexcept { return ::au::cv::info(*this); }

    // ------------------------------------------------------------------------
    // Implicit conversion to legacy types (non-owning view)
    // ------------------------------------------------------------------------

    /** @brief Implicit conversion to VImage (unowned view). */
    operator VImage() const noexcept
    {  // NOLINT(google-explicit-constructor)
        return toVImage(*this);
    }

    /** @brief Implicit conversion to VImageEx (unowned view). */
    operator VImageEx() const noexcept
    {  // NOLINT(google-explicit-constructor)
        return toVImageEx(*this);
    }

    /** @brief Implicit conversion to VImageExV1 (unowned view). */
    operator VImageExV1() const noexcept
    {  // NOLINT(google-explicit-constructor)
        return toVImageExV1(*this);
    }

private:
    bool mNeedDestroy{false};
};

} /* namespace cv */
} /* namespace au */

#endif /* AURA_CV_XIMAGEF_H_ */
