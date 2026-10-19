#ifndef AURA_CV_XIMAGE_API_H_
#define AURA_CV_XIMAGE_API_H_

#include <initializer_list>
#include <string>

#include "cv/ximage_types.h"
#include "mm/xmemory.h"
#include "sys/xsystem.h"

namespace au {
namespace cv {

constexpr au::mm::MemType kDefaultImageMemType           = au::mm::MemType::DmaCached;
constexpr int             kDefaultImageColorSpace        = kImageColorSpaceUnspecified;
constexpr int             kDefaultImageStrideAlignBytes  = 8;
constexpr int             kDefaultImageScanlineAlignRows = 2;

// Allocates a single managed block for all active planes. The caller owns the
// returned image and must call destroyImage(). Allocation and geometry failure
// return an empty descriptor. tag is reserved for future diagnostics.
// The default DMA cached storage requires an available DMA heap. Select Pss
// explicitly for CPU-only images.
AU_API Image createImage(int width, int height, int format, au::mm::MemType memType = kDefaultImageMemType,
                         int colorSpace = kDefaultImageColorSpace, int strideAlignBytes = kDefaultImageStrideAlignBytes,
                         int scanlineAlignRows = kDefaultImageScanlineAlignRows, const char* tag = nullptr) noexcept;

// Only pass a descriptor obtained from createImage(). This clears img even
// when the backend reports a release error; a borrowed Image must not be
// passed here. Returns true when the managed allocation was released.
AU_API bool destroyImage(Image& img) noexcept;

// Cache synchronization is relevant for DMA cached storage. These functions
// operate on Aura-managed blocks and return an au::err code.
AU_API int invalidateImageCache(const Image& img) noexcept;
AU_API int flushImageCache(const Image& img) noexcept;

// Validates every active plane against the format's minimum row bytes and
// height. It cannot prove the capacity of a foreign allocation beyond the
// caller-provided dataSize values.
AU_API bool isValid(const Image& img) noexcept;
AU_API bool isFormat(const Image& img, int format) noexcept;
AU_API bool isFormatIn(const Image& img, std::initializer_list<int> formats) noexcept;
AU_API bool isSameWith(const Image& a, const Image& b) noexcept;
AU_API bool isSameSizeWith(const Image& a, const Image& b) noexcept;
AU_API bool isSameFormatWith(const Image& a, const Image& b) noexcept;
AU_API bool isSameSizeAndFormatWith(const Image& a, const Image& b) noexcept;

// Diagnostic operation; may allocate and is not intended for hot paths.
// Returns an empty string on allocation failure.
AU_API std::string info(const Image& img) noexcept;

}  // namespace cv
}  // namespace au

#endif  // AURA_CV_XIMAGE_API_H_
