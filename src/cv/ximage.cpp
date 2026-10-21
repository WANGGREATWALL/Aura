/**
 * @file ximage.cpp
 * @brief Image helper – implementation of the au::cv free-function ABI.
 *
 * Internal layout:
 *   - PlaneSpec / FormatDescriptor : compile-time metadata table that maps
 *     an Aura format ID to its plane count and the per-plane
 *     subsampling / bits-per-pixel rule. Acts as the single source of truth.
 *   - Allocation strategy          : ALL planes of an image are carved out of
 *     a SINGLE au::mm::alloc block. Planes are laid out back-to-back with no
 *     inter-plane padding; every plane therefore shares the block's fd and
 *     carries its real byte offset in fdOffset[i] (plane[0] is always 0).
 *   - Cache sync                   : a single dma-buf sync covers the whole
 *     block, Pss and DmaUncached are no-ops in the backend.
 */

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>

#include "cv/ximage_api.h"
#include "log/xerror.h"
#include "mm/xmemory.h"

namespace au {
namespace cv {

// ============================================================================
// Internal helpers (translation-unit local)
// ============================================================================

namespace {

// — FormatDescriptor : metadata types ————————————————————————————————————————

constexpr int MAX_PLANES = 4;

struct PlaneSpec
{
    uint8_t  hSubsample;            /* horizontal subsampling factor (1 or 2) */
    uint8_t  vSubsample;            /* vertical   subsampling factor (1 or 2) */
    uint16_t bitsPerPixel;          /* effective bits per sample on this plane */
    int64_t (*rowBytesFn)(int64_t); /* nullable; overrides bpp-based computation */
};

struct FormatDescriptor
{
    int       format;
    uint8_t   numPlanes;
    PlaneSpec planes[MAX_PLANES];
};

// — irregular row-bytes helpers ——————————————————————————————————————————————

int64_t rowBytesMipi10Bit(int64_t widthThisPlane)
{
    // 4 pixels packed into 5 bytes (10-bit MIPI CSI-2).
    return ((widthThisPlane + 3) / 4) * 5;
}

// — descriptor table —————————————————————————————————————————————————————————
//
// Column layout (one row = one FormatDescriptor):
//
// { format, numPlanes, {{hSub, vSub, bpp, rowFn}, {hSub, vSub, bpp, rowFn}, ...} }
//
//   format     — XImageFormat constant
//   numPlanes  — number of active planes (1–4)
//   hSub       — horizontal subsampling factor: plane_width  = ceil(image_width  / hSub)
//   vSub       — vertical   subsampling factor: plane_height = ceil(image_height / vSub)
//   bpp        — bits per pixel on this plane; used as ceil(bpp * plane_width / 8) to
//                compute raw row bytes. Set to 0 when rowFn is provided.
//   rowFn      — optional row-bytes override: fn(plane_width) → raw row bytes before
//                stride alignment. nullptr means the standard bpp formula is used.

const FormatDescriptor FORMAT_TABLE[] = {
    // — Gray family —
    {kImageFormatGrayU8, 1, {{1, 1, 8, nullptr}, {}, {}, {}}},
    {kImageFormatGrayU16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatGrayS16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatGrayU32, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatGrayS32, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatGrayF32, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatUVU8, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},

    // — RGB family (interleaved) —
    {kImageFormatBGRU8, 1, {{1, 1, 24, nullptr}, {}, {}, {}}},
    {kImageFormatRGBU8, 1, {{1, 1, 24, nullptr}, {}, {}, {}}},
    {kImageFormatBGRAU8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatARGBU8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatRGBAU8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kImageFormatRGBU16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},
    {kImageFormatRGBF32, 1, {{1, 1, 96, nullptr}, {}, {}, {}}},
    {kImageFormatRGBF16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},
    {kImageFormatBGRF16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},

    // — YUV 4:2:0 semi-planar —
    {kImageFormatNV12, 2, {{1, 1, 8, nullptr}, {2, 2, 16, nullptr}, {}, {}}},
    {kImageFormatNV21, 2, {{1, 1, 8, nullptr}, {2, 2, 16, nullptr}, {}, {}}},
    {kImageFormatI420, 3, {{1, 1, 8, nullptr}, {2, 2, 8, nullptr}, {2, 2, 8, nullptr}, {}}},
    {kImageFormatYV12, 3, {{1, 1, 8, nullptr}, {2, 2, 8, nullptr}, {2, 2, 8, nullptr}, {}}},

    // — YUV wide bit-depth —
    {kImageFormatP010, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kImageFormatP016, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kImageFormatNV12F16, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kImageFormatNV21F16, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},

    // — Raw : MIPI packed 10-bit —
    {kImageFormatMipiRGGB10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kImageFormatMipiGRBG10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kImageFormatMipiBGGR10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kImageFormatMipiGBRG10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kImageFormatRawPackedU10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kImageFormatRawU16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},

    // — Raw : unpacked 10 / 12 / 14 / 16-bit (one sample = 2 bytes) —
    {kImageFormatUnpackedRGGB10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGRBG10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedBGGR10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGBRG10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedRGGB12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGRBG12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedBGGR12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGBRG12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedRGGB14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGRBG14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedBGGR14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGBRG14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedRGGB16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGRBG16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedBGGR16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kImageFormatUnpackedGBRG16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
};

const int FORMAT_TABLE_SIZE = static_cast<int>(sizeof(FORMAT_TABLE) / sizeof(FORMAT_TABLE[0]));

const FormatDescriptor* lookupDescriptor(int format) noexcept
{
    for (int i = 0; i < FORMAT_TABLE_SIZE; ++i) {
        if (FORMAT_TABLE[i].format == format) {
            return &FORMAT_TABLE[i];
        }
    }
    return nullptr;
}

// — Geometry helpers —————————————————————————————————————————————————————————

inline int64_t alignUp(int64_t value, int align) noexcept
{
    return (align <= 1) ? value : (value + align - 1) / align * align;
}

inline int64_t ceilDiv(int64_t a, int b) noexcept { return a / b + (a % b != 0); }

int64_t rowBytes(const PlaneSpec& spec, int64_t planeWidth) noexcept
{
    return spec.rowBytesFn != nullptr ? spec.rowBytesFn(planeWidth)
                                      : ceilDiv(static_cast<int64_t>(spec.bitsPerPixel) * planeWidth, 8);
}

bool knownColorSpace(int colorSpace) noexcept
{
    switch (colorSpace) {
        case kImageColorSpaceUnspecified:
        case kImageColorSpaceSRGB:
        case kImageColorSpaceLinearSRGB:
        case kImageColorSpaceRec709:
        case kImageColorSpaceDisplayP3:
        case kImageColorSpaceRec2020PQ: return true;
        default: return false;
    }
}

const char* shortFormatName(int format) noexcept
{
    switch (format) {
        case kImageFormatGrayU8: return "Gray8";
        case kImageFormatGrayU16: return "GrayU16";
        case kImageFormatGrayS16: return "GrayS16";
        case kImageFormatGrayU32: return "GrayU32";
        case kImageFormatGrayS32: return "GrayS32";
        case kImageFormatGrayF32: return "GrayF32";
        case kImageFormatUVU8: return "UV8";
        case kImageFormatRGBU8: return "RGB8";
        case kImageFormatBGRU8: return "BGR8";
        case kImageFormatRGBAU8: return "RGBA8";
        case kImageFormatBGRAU8: return "BGRA8";
        case kImageFormatARGBU8: return "ARGB8";
        case kImageFormatRGBU16: return "RGB16";
        case kImageFormatRGBF32: return "RGBF32";
        case kImageFormatRGBF16: return "RGB_F16";
        case kImageFormatBGRF16: return "BGR_F16";
        case kImageFormatNV12: return "NV12";
        case kImageFormatNV21: return "NV21";
        case kImageFormatI420: return "I420";
        case kImageFormatYV12: return "YV12";
        case kImageFormatP010: return "P010";
        case kImageFormatP016: return "P016";
        case kImageFormatNV12F16: return "NV12_F16";
        case kImageFormatNV21F16: return "NV21_F16";
        case kImageFormatMipiRGGB10: return "Mipi.RGGB10";
        case kImageFormatMipiGRBG10: return "Mipi.GRBG10";
        case kImageFormatMipiBGGR10: return "Mipi.BGGR10";
        case kImageFormatMipiGBRG10: return "Mipi.GBRG10";
        case kImageFormatRawPackedU10: return "RawPackedU10";
        case kImageFormatRawU16: return "RawU16";
        case kImageFormatUnpackedRGGB10: return "Unpk.RGGB10";
        case kImageFormatUnpackedGRBG10: return "Unpk.GRBG10";
        case kImageFormatUnpackedBGGR10: return "Unpk.BGGR10";
        case kImageFormatUnpackedGBRG10: return "Unpk.GBRG10";
        case kImageFormatUnpackedRGGB12: return "Unpk.RGGB12";
        case kImageFormatUnpackedGRBG12: return "Unpk.GRBG12";
        case kImageFormatUnpackedBGGR12: return "Unpk.BGGR12";
        case kImageFormatUnpackedGBRG12: return "Unpk.GBRG12";
        case kImageFormatUnpackedRGGB14: return "Unpk.RGGB14";
        case kImageFormatUnpackedGRBG14: return "Unpk.GRBG14";
        case kImageFormatUnpackedBGGR14: return "Unpk.BGGR14";
        case kImageFormatUnpackedGBRG14: return "Unpk.GBRG14";
        case kImageFormatUnpackedRGGB16: return "Unpk.RGGB16";
        case kImageFormatUnpackedGRBG16: return "Unpk.GRBG16";
        case kImageFormatUnpackedBGGR16: return "Unpk.BGGR16";
        case kImageFormatUnpackedGBRG16: return "Unpk.GBRG16";
        default: return "?";
    }
}

// — Allocation helpers ———————————————————————————————————————————————————————

const FormatDescriptor* prepareGeometry(int width, int height, int format, Image& out) noexcept
{
    // Every early return produces a deterministic invalid descriptor.
    out = Image{};
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    const FormatDescriptor* desc = lookupDescriptor(format);
    if (desc == nullptr) {
        return nullptr;
    }
    out.width  = width;
    out.height = height;
    out.format = format;
    return desc;
}

// — Allocation helpers ———————————————————————————————————————————————————————

// Computed layout of one plane, independent of any allocation. The whole image
// is planned first, then carved out of a single contiguous block.
struct PlaneLayout
{
    int stride   = 0;  // aligned row stride in bytes
    int scanline = 0;  // aligned height in rows
    int dataSize = 0;  // stride * scanline
    int offset   = 0;  // byte offset of this plane within the shared block
};

// Plan every plane's geometry and placement WITHOUT allocating. On success
// fills @p out[0..numPlanes-1] and returns the total block size; on
// failure returns -1. Layout is strictly back-to-back (no inter-plane padding):
// offset[p] = offset[p-1] + dataSize[p-1], plane[0] offset = 0.
int planPlanes(const FormatDescriptor* desc, int width, int height, int strideAlignBytes, int scanlineAlignRows,
               PlaneLayout out[MAX_PLANES]) noexcept
{
    if (strideAlignBytes <= 0 || scanlineAlignRows <= 0) {
        return -1;
    }
    int64_t       offset   = 0;
    const int64_t maxField = std::numeric_limits<int>::max();
    for (int p = 0; p < desc->numPlanes; ++p) {
        const PlaneSpec& spec = desc->planes[p];

        const int64_t wPlane          = ceilDiv(width, spec.hSubsample);
        const int64_t hPlane          = ceilDiv(height, spec.vSubsample);
        const int64_t rowBytesRaw     = rowBytes(spec, wPlane);
        const int64_t strideAligned   = alignUp(rowBytesRaw, strideAlignBytes);
        const int64_t scanlineAligned = alignUp(hPlane, scanlineAlignRows);

        // Check before multiplication and addition, then narrow to the
        // public descriptor's int fields.
        if (rowBytesRaw <= 0 || strideAligned <= 0 || strideAligned > maxField || scanlineAligned <= 0 ||
            scanlineAligned > maxField || strideAligned > maxField / scanlineAligned) {
            return -1;
        }
        const int64_t dataSize = strideAligned * scanlineAligned;
        if (offset > maxField - dataSize) {
            return -1;
        }

        out[p].stride   = static_cast<int>(strideAligned);
        out[p].scanline = static_cast<int>(scanlineAligned);
        out[p].dataSize = static_cast<int>(dataSize);
        out[p].offset   = static_cast<int>(offset);

        offset += dataSize;  // back-to-back, no padding
    }
    return static_cast<int>(offset);  // total block size
}

// — Cache-sync helpers ———————————————————————————————————————————————————————

}  // anonymous namespace

// ============================================================================
// Allocation / release
// ============================================================================

Image createImage(int width, int height, int format, au::mm::MemType memType, int colorSpace, int strideAlignBytes,
                  int scanlineAlignRows, const char* /*tag*/) noexcept
{
    Image                   img{};
    const FormatDescriptor* desc = prepareGeometry(width, height, format, img);
    if (desc == nullptr || !knownColorSpace(colorSpace)) {
        return Image{};
    }
    img.colorSpace = colorSpace;

    // Plan every plane's geometry + placement without allocating.
    PlaneLayout layout[MAX_PLANES];
    const int   totalSize = planPlanes(desc, width, height, strideAlignBytes, scanlineAlignRows, layout);
    if (totalSize <= 0) {
        return Image{};
    }

    // One allocation for the whole image. au::mm::alloc registers the block
    // under block.ptr (== data[0]); on Registry-insert failure it rolls
    // back internally, so a null ptr here needs no further cleanup.
    au::mm::MemBlock block;
    const int        rc = au::mm::alloc(static_cast<size_t>(totalSize), memType, block);
    if (rc != au::err::kSuccess || block.ptr == nullptr) {
        return Image{};
    }

    // Carve planes out of the block. Every plane shares the same fd and
    // carries its real byte offset; plane[0] starts at the block base.
    unsigned char* const base = static_cast<unsigned char*>(block.ptr);
    for (int p = 0; p < desc->numPlanes; ++p) {
        img.data[p]     = base + layout[p].offset;
        img.stride[p]   = layout[p].stride;
        img.scanline[p] = layout[p].scanline;
        img.dataSize[p] = layout[p].dataSize;
        img.fd[p]       = block.fd;
        img.fdOffset[p] = layout[p].offset;
    }

    return img;
}

bool destroyImage(Image& img) noexcept
{
    bool releasedAny = false;
    if (img.data[0] != nullptr) {
        releasedAny = (au::mm::free(img.data[0]) == au::err::kSuccess);
    }
    img = Image{};
    return releasedAny;
}

// ============================================================================
// Cache synchronisation
// ============================================================================

int invalidateImageCache(const Image& img) noexcept
{
    // Device wrote → CPU reads: invalidate CPU cache lines.
    return img.data[0] != nullptr ? au::mm::syncDeviceToCpu(img.data[0]) : au::err::kSuccess;
}

int flushImageCache(const Image& img) noexcept
{
    // CPU wrote → device reads: flush CPU cache lines.
    return img.data[0] != nullptr ? au::mm::syncCpuToDevice(img.data[0]) : au::err::kSuccess;
}

// ============================================================================
// Predicates
// ============================================================================

bool isValid(const Image& img) noexcept
{
    if (img.width <= 0 || img.height <= 0) {
        return false;
    }
    const FormatDescriptor* desc = lookupDescriptor(img.format);
    if (desc == nullptr || !knownColorSpace(img.colorSpace)) {
        return false;
    }
    for (int p = 0; p < desc->numPlanes; ++p) {
        const PlaneSpec& spec            = desc->planes[p];
        const int64_t    planeWidth      = ceilDiv(img.width, spec.hSubsample);
        const int64_t    planeHeight     = ceilDiv(img.height, spec.vSubsample);
        const int64_t    minimumRowBytes = rowBytes(spec, planeWidth);
        if (img.data[p] == nullptr || img.stride[p] < minimumRowBytes || img.scanline[p] < planeHeight ||
            img.dataSize[p] <= 0 || static_cast<int64_t>(img.stride[p]) * img.scanline[p] > img.dataSize[p] ||
            img.fdOffset[p] < 0) {
            return false;
        }
    }
    for (int p = desc->numPlanes; p < MAX_PLANES; ++p) {
        if (img.data[p] != nullptr) {
            return false;
        }
    }
    return true;
}

bool isFormat(const Image& img, int format) noexcept { return img.format == format; }

bool isFormatIn(const Image& img, std::initializer_list<int> formats) noexcept
{
    for (int f : formats) {
        if (img.format == f) {
            return true;
        }
    }
    return false;
}

bool isSameSizeWith(const Image& a, const Image& b) noexcept
{
    if (a.width != b.width || a.height != b.height) {
        return false;
    }
    if (a.format == b.format) {
        const FormatDescriptor* desc = lookupDescriptor(a.format);
        if (desc == nullptr) {
            return false;
        }
        for (int p = 0; p < desc->numPlanes; ++p) {
            if (a.stride[p] != b.stride[p]) {
                return false;
            }
        }
    }
    return true;
}

bool isSameFormatWith(const Image& a, const Image& b) noexcept { return a.format == b.format; }

bool isSameSizeAndFormatWith(const Image& a, const Image& b) noexcept
{
    return isSameSizeWith(a, b) && isSameFormatWith(a, b);
}

bool isSameWith(const Image& a, const Image& b) noexcept
{
    if (!isSameSizeAndFormatWith(a, b))
        return false;
    if (a.colorSpace != b.colorSpace)
        return false;
    for (int p = 0; p < 4; ++p) {
        if (a.data[p] != b.data[p])
            return false;
        if (a.stride[p] != b.stride[p])
            return false;
        if (a.scanline[p] != b.scanline[p])
            return false;
        if (a.dataSize[p] != b.dataSize[p])
            return false;
        if (a.fd[p] != b.fd[p])
            return false;
        if (a.fdOffset[p] != b.fdOffset[p])
            return false;
    }
    return true;
}

// ============================================================================
// Diagnostics
// ============================================================================

std::string info(const Image& img) noexcept
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "[%dx%d|%d,%d,%d,%d], fmt:%s, data:[%p,%p,%p,%p], fd:[%d,%d], offset:[%d,%d]",
                  img.width, img.height, img.stride[0], img.stride[1], img.stride[2], img.stride[3],
                  shortFormatName(img.format), static_cast<void*>(img.data[0]), static_cast<void*>(img.data[1]),
                  static_cast<void*>(img.data[2]), static_cast<void*>(img.data[3]), img.fd[0], img.fd[1],
                  img.fdOffset[0], img.fdOffset[1]);
    try {
        return std::string(buf);
    } catch (...) {
        return std::string();
    }
}

}  // namespace cv
}  // namespace au
