/**
 * @file ximagef.cpp
 * @brief VImageF helper – implementation of the au::cv free-function ABI.
 *
 * Internal layout:
 *   - PlaneSpec / FormatDescriptor : compile-time metadata table that maps
 *     a (legacy or new) format ID to its plane count and the per-plane
 *     subsampling / bits-per-pixel rule. Acts as the single source of truth.
 *   - Allocation strategy          : ALL planes of an image are carved out of
 *     a SINGLE au::mm::alloc block. Planes are laid out back-to-back with no
 *     inter-plane padding; every plane therefore shares the block's fd and
 *     carries its real byte offset in fdOffset[i] (plane[0] is always 0).
 *   - Cache sync                   : a single dma-buf sync covers the whole
 *     block, Pss and DmaUncached are no-ops in the backend.
 *   - Compatibility conversions    : pure structural projections, no allocation.
 */

#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>

#include "cv/ximagef_api.h"
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
    uint8_t  hSubsample;    /* horizontal subsampling factor (1 or 2) */
    uint8_t  vSubsample;    /* vertical   subsampling factor (1 or 2) */
    uint16_t bitsPerPixel;  /* effective bits per sample on this plane */
    int (*rowBytesFn)(int); /* nullable; overrides bpp-based computation */
};

struct FormatDescriptor
{
    int       format;
    uint8_t   numPlanes;
    PlaneSpec planes[MAX_PLANES];
};

// — irregular row-bytes helpers ——————————————————————————————————————————————

int rowBytesMipi10Bit(int widthThisPlane)
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
//   format     — VImageFormat / VImageFormatF constant
//   numPlanes  — number of active planes (1–4)
//   hSub       — horizontal subsampling factor: plane_width  = ceil(image_width  / hSub)
//   vSub       — vertical   subsampling factor: plane_height = ceil(image_height / vSub)
//   bpp        — bits per pixel on this plane; used as ceil(bpp * plane_width / 8) to
//                compute raw row bytes. Set to 0 when rowFn is provided.
//   rowFn      — optional row-bytes override: fn(plane_width) → raw row bytes before
//                stride alignment. nullptr means the standard bpp formula is used.

const FormatDescriptor FORMAT_TABLE[] = {
    // — Gray family —
    {kVIFormatGray, 1, {{1, 1, 8, nullptr}, {}, {}, {}}},
    {kVIFormatGrayU16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatGrayS16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatGrayU32, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kVIFormatGrayS32, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kVIFormatGrayFLOAT, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},

    // — RGB family (interleaved) —
    {kVIFormatB8G8R8, 1, {{1, 1, 24, nullptr}, {}, {}, {}}},
    {kVIFormatR8G8B8, 1, {{1, 1, 24, nullptr}, {}, {}, {}}},
    {kVIFormatB8G8R8A8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kVIFormatA8R8G8B8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kVIFormatR8G8B8A8, 1, {{1, 1, 32, nullptr}, {}, {}, {}}},
    {kVIFormatR16G16B16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},
    {kVIFormatRF32GF32BF32, 1, {{1, 1, 96, nullptr}, {}, {}, {}}},
    {kVIFormatRGB_F16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},
    {kVIFormatBGR_F16, 1, {{1, 1, 48, nullptr}, {}, {}, {}}},

    // — YUV 4:2:0 semi-planar —
    {kVIFormatNV12, 2, {{1, 1, 8, nullptr}, {2, 2, 16, nullptr}, {}, {}}},
    {kVIFormatNV21, 2, {{1, 1, 8, nullptr}, {2, 2, 16, nullptr}, {}, {}}},
    {kVIFormatI420, 3, {{1, 1, 8, nullptr}, {2, 2, 8, nullptr}, {2, 2, 8, nullptr}, {}}},
    {kVIFormatYV12, 3, {{1, 1, 8, nullptr}, {2, 2, 8, nullptr}, {2, 2, 8, nullptr}, {}}},

    // — YUV wide bit-depth —
    {kVIFormatP010, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kVIFormatP016, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kVIFormatNV12_F16, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},
    {kVIFormatNV21_F16, 2, {{1, 1, 16, nullptr}, {2, 2, 32, nullptr}, {}, {}}},

    // — Raw : MIPI packed 10-bit —
    {kVIFormatMipiRGGB10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kVIFormatMipiGRBG10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kVIFormatMipiBGGR10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},
    {kVIFormatMipiGBRG10, 1, {{1, 1, 0, rowBytesMipi10Bit}, {}, {}, {}}},

    // — Raw : unpacked 10 / 12 / 14 / 16-bit (one sample = 2 bytes) —
    {kVIFormatUnpackedRGGB10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGRBG10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedBGGR10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGBRG10, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedRGGB12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGRBG12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedBGGR12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGBRG12, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedRGGB14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGRBG14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedBGGR14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGBRG14, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedRGGB16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGRBG16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedBGGR16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
    {kVIFormatUnpackedGBRG16, 1, {{1, 1, 16, nullptr}, {}, {}, {}}},
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

inline int alignUp(int value, int align) noexcept
{
    return (align <= 1) ? value : (value + align - 1) / align * align;
}

inline int ceilDiv(int a, int b) noexcept
{
    return (a + b - 1) / b;
}

const char* shortFormatName(int format) noexcept
{
    switch (format) {
        case kVIFormatGray: return "Gray8";
        case kVIFormatGrayU16: return "GrayU16";
        case kVIFormatGrayS16: return "GrayS16";
        case kVIFormatGrayU32: return "GrayU32";
        case kVIFormatGrayS32: return "GrayS32";
        case kVIFormatGrayFLOAT: return "GrayF32";
        case kVIFormatR8G8B8: return "RGB8";
        case kVIFormatB8G8R8: return "BGR8";
        case kVIFormatR8G8B8A8: return "RGBA8";
        case kVIFormatB8G8R8A8: return "BGRA8";
        case kVIFormatA8R8G8B8: return "ARGB8";
        case kVIFormatR16G16B16: return "RGB16";
        case kVIFormatRF32GF32BF32: return "RGBF32";
        case kVIFormatRGB_F16: return "RGB_F16";
        case kVIFormatBGR_F16: return "BGR_F16";
        case kVIFormatNV12: return "NV12";
        case kVIFormatNV21: return "NV21";
        case kVIFormatI420: return "I420";
        case kVIFormatYV12: return "YV12";
        case kVIFormatP010: return "P010";
        case kVIFormatP016: return "P016";
        case kVIFormatNV12_F16: return "NV12_F16";
        case kVIFormatNV21_F16: return "NV21_F16";
        case kVIFormatMipiRGGB10: return "Mipi.RGGB10";
        case kVIFormatMipiGRBG10: return "Mipi.GRBG10";
        case kVIFormatMipiBGGR10: return "Mipi.BGGR10";
        case kVIFormatMipiGBRG10: return "Mipi.GBRG10";
        case kVIFormatUnpackedRGGB10: return "Unpk.RGGB10";
        case kVIFormatUnpackedGRBG10: return "Unpk.GRBG10";
        case kVIFormatUnpackedBGGR10: return "Unpk.BGGR10";
        case kVIFormatUnpackedGBRG10: return "Unpk.GBRG10";
        case kVIFormatUnpackedRGGB12: return "Unpk.RGGB12";
        case kVIFormatUnpackedGRBG12: return "Unpk.GRBG12";
        case kVIFormatUnpackedBGGR12: return "Unpk.BGGR12";
        case kVIFormatUnpackedGBRG12: return "Unpk.GBRG12";
        case kVIFormatUnpackedRGGB14: return "Unpk.RGGB14";
        case kVIFormatUnpackedGRBG14: return "Unpk.GRBG14";
        case kVIFormatUnpackedBGGR14: return "Unpk.BGGR14";
        case kVIFormatUnpackedGBRG14: return "Unpk.GBRG14";
        case kVIFormatUnpackedRGGB16: return "Unpk.RGGB16";
        case kVIFormatUnpackedGRBG16: return "Unpk.GRBG16";
        case kVIFormatUnpackedBGGR16: return "Unpk.BGGR16";
        case kVIFormatUnpackedGBRG16: return "Unpk.GBRG16";
        default: return "?";
    }
}

// — Allocation helpers ———————————————————————————————————————————————————————

const FormatDescriptor* prepareGeometry(int width, int height, int format, VImageF& out) noexcept
{
    // VImageF is a plain C struct (no ctor); value-init to zero all fields so
    // every early-return path returns a deterministic, isValid()==false image.
    out = VImageF{};
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
    int stride   = 0; // aligned row stride in bytes
    int scanline = 0; // aligned height in rows
    int dataSize = 0; // stride * scanline
    int offset   = 0; // byte offset of this plane within the shared block
};

// Plan every plane's geometry and placement WITHOUT allocating. On success
// fills @p out[0..numPlanes-1] and returns the total block size (>=0); on
// failure returns -1. Layout is strictly back-to-back (no inter-plane padding):
// offset[p] = offset[p-1] + dataSize[p-1], plane[0] offset = 0.
int planPlanes(const FormatDescriptor* desc,
               int                     width,
               int                     height,
               int                     strideAlignBytes,
               int                     scanlineAlignRows,
               PlaneLayout             out[MAX_PLANES]) noexcept
{
    int offset = 0;
    for (int p = 0; p < desc->numPlanes; ++p) {
        const PlaneSpec& spec = desc->planes[p];

        const int wPlane = ceilDiv(width, spec.hSubsample);
        const int hPlane = ceilDiv(height, spec.vSubsample);

        const int rowBytesRaw =
            (spec.rowBytesFn != nullptr) ? spec.rowBytesFn(wPlane) : ceilDiv(spec.bitsPerPixel * wPlane, 8);

        const int strideAligned   = alignUp(rowBytesRaw, strideAlignBytes);
        const int scanlineAligned = alignUp(hPlane, scanlineAlignRows);
        const int dataSize        = strideAligned * scanlineAligned;

        if (strideAligned <= 0 || scanlineAligned <= 0 || dataSize <= 0) {
            return -1;
        }

        out[p].stride   = strideAligned;
        out[p].scanline = scanlineAligned;
        out[p].dataSize = dataSize;
        out[p].offset   = offset;

        offset += dataSize; // back-to-back, no padding
    }
    return offset; // total block size
}

// — Cache-sync helpers ———————————————————————————————————————————————————————

int forEachPlaneSync(const VImageF& img, const std::function<int(void*)>& fn) noexcept
{
    if (img.data[0] == nullptr) {
        return au::err::kSuccess;
    }
    return fn(img.data[0]);
}

// — Legacy-conversion helpers ————————————————————————————————————————————————

inline int fallbackScanline(int candidate, int imageHeight) noexcept
{
    return candidate > 0 ? candidate : imageHeight;
}

} // anonymous namespace

// ============================================================================
// Allocation / release
// ============================================================================

VImageF createVImageF(int             width,
                     int             height,
                     int             format,
                     au::mm::MemType memType,
                     int             colorSpace,
                     int             strideAlignBytes,
                     int             scanlineAlignRows,
                     const char*     /*tag*/) noexcept
{
    VImageF                 img{}; // The {} is C++ zero-inits
    const FormatDescriptor* desc = prepareGeometry(width, height, format, img);
    if (desc == nullptr) {
        return img;
    }
    img.colorSpace = colorSpace;

    // Plan every plane's geometry + placement without allocating.
    PlaneLayout layout[MAX_PLANES];
    const int   totalSize = planPlanes(desc, width, height, strideAlignBytes, scanlineAlignRows, layout);
    if (totalSize <= 0) {
        return img;
    }

    // One allocation for the whole image. au::mm::alloc registers the block
    // under block.ptr (== data[0]); on Registry-insert failure it rolls
    // back internally, so a null ptr here needs no further cleanup.
    au::mm::MemBlock block;
    const int       rc = au::mm::alloc(static_cast<size_t>(totalSize), memType, block);
    if (rc != au::err::kSuccess || block.ptr == nullptr) {
        return img;
    }

    // Carve planes out of the block. Every plane shares the same fd and
    // carries its real byte offset; plane[0] starts at the block base.
    unsigned char* const base = static_cast<unsigned char*>(block.ptr);
    for (int p = 0; p < desc->numPlanes; ++p) {
        img.data[p]     = base + layout[p].offset;
        img.stride[p]   = layout[p].stride;
        img.scanline[p] = layout[p].scanline;
        img.dataSize[p] = layout[p].dataSize;
        // VImageF convention: fd == 0 means invalid; au::mm uses fd = -1 for Pss.
        img.fd[p]       = (block.fd > 0) ? block.fd : 0;
        img.fdOffset[p] = layout[p].offset;
    }

    return img;
}

bool destroyVImageF(VImageF& img) noexcept
{
    bool releasedAny = false;
    if (img.data[0] != nullptr) {
        releasedAny = (au::mm::free(img.data[0]) == au::err::kSuccess);
    }
    img = VImageF{};
    return releasedAny;
}

// ============================================================================
// Cache synchronisation
// ============================================================================

int invalidateImageCache(const VImageF& img) noexcept
{
    // Device wrote → CPU reads: invalidate CPU cache lines.
    return forEachPlaneSync(img, [](void* addr) { return au::mm::syncDeviceToCpu(addr); });
}

int flushImageCache(const VImageF& img) noexcept
{
    // CPU wrote → device reads: flush CPU cache lines.
    return forEachPlaneSync(img, [](void* addr) { return au::mm::syncCpuToDevice(addr); });
}

// ============================================================================
// Predicates
// ============================================================================

bool isValid(const VImageF& img) noexcept
{
    if (img.width <= 0 || img.height <= 0) {
        return false;
    }
    if (lookupDescriptor(img.format) == nullptr) {
        return false;
    }
    if (img.data[0] == nullptr || img.stride[0] <= 0 || img.scanline[0] <= 0) {
        return false;
    }
    return true;
}

bool isFormat(const VImageF& img, int format) noexcept
{
    return img.format == format;
}

bool isFormatIn(const VImageF& img, std::initializer_list<int> formats) noexcept
{
    for (int f : formats) {
        if (img.format == f) {
            return true;
        }
    }
    return false;
}

bool isSameSizeWith(const VImageF& a, const VImageF& b) noexcept
{
    if (a.width != b.width || a.height != b.height) {
        return false;
    }
    if (a.format == b.format) {
        return a.stride[0] == b.stride[0];
    }
    return true;
}

bool isSameFormatWith(const VImageF& a, const VImageF& b) noexcept
{
    return a.format == b.format;
}

bool isSameSizeAndFormatWith(const VImageF& a, const VImageF& b) noexcept
{
    return isSameSizeWith(a, b) && isSameFormatWith(a, b);
}

bool isSameWith(const VImageF& a, const VImageF& b) noexcept
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

std::string info(const VImageF& img) noexcept
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "[%dx%d|%d,%d,%d,%d], fmt:%s, data:[%p,%p,%p,%p], fd:[%d,%d], offset:[%d,%d]",
                  img.width, img.height, img.stride[0], img.stride[1], img.stride[2], img.stride[3],
                  shortFormatName(img.format), img.data[0], img.data[1], img.data[2], img.data[3], img.fd[0], img.fd[1],
                  img.fdOffset[0], img.fdOffset[1]);
    return std::string(buf);
}

// ============================================================================
// Compatibility conversions – VImageF → legacy
// ============================================================================

VImage toVImage(const VImageF& src) noexcept
{
    VImage dst{};
    dst.format = src.format;
    dst.width  = src.width;
    dst.height = src.height;
    for (int p = 0; p < 4; ++p) {
        dst.data[p]   = src.data[p];
        dst.stride[p] = src.stride[p];
    }
    return dst;
}

VImageEx toVImageEx(const VImageF& src) noexcept
{
    VImageEx dst{};
    dst.image         = toVImage(src);
    dst.pNativeHandle = nullptr;
    for (int p = 0; p < 4; ++p) {
        dst.scanline[p] = src.scanline[p];
        dst.dataSize[p] = src.dataSize[p];
        dst.fd[p]       = src.fd[p];
    }
    return dst;
}

VImageExV1 toVImageExV1(const VImageF& src) noexcept
{
    VImageExV1 dst{};
    dst.image         = toVImage(src);
    dst.pNativeHandle = nullptr;
    for (int p = 0; p < 4; ++p) {
        dst.scanline[p] = src.scanline[p];
        dst.dataSize[p] = src.dataSize[p];
        dst.fd[p]       = src.fd[p];
        dst.fdOffset[p] = src.fdOffset[p];
    }
    return dst;
}

// ============================================================================
// Compatibility conversions – legacy → VImageF
// ============================================================================

VImageF fromVImage(const VImage& src) noexcept
{
    VImageF dst{};
    dst.format     = src.format;
    dst.colorSpace = kVIColorSpaceDefault;
    dst.width      = src.width;
    dst.height     = src.height;
    for (int p = 0; p < 4; ++p) {
        dst.data[p]     = src.data[p];
        dst.stride[p]   = src.stride[p];
        dst.scanline[p] = (src.data[p] != nullptr) ? src.height : 0;
        dst.dataSize[p] = (src.data[p] != nullptr) ? src.stride[p] * src.height : 0;
        dst.fd[p]       = 0;
        dst.fdOffset[p] = 0;
    }
    return dst;
}

VImageF fromVImageEx(const VImageEx& src) noexcept
{
    VImageF dst = fromVImage(src.image);
    for (int p = 0; p < 4; ++p) {
        dst.scanline[p] = fallbackScanline(src.scanline[p], src.image.height);
        dst.dataSize[p] = (src.dataSize[p] > 0) ? src.dataSize[p] : dst.dataSize[p];
        dst.fd[p]       = src.fd[p];
        dst.fdOffset[p] = 0;
    }
    return dst;
}

VImageF fromVImageExV1(const VImageExV1& src) noexcept
{
    VImageF dst = fromVImage(src.image);
    for (int p = 0; p < 4; ++p) {
        dst.scanline[p] = fallbackScanline(src.scanline[p], src.image.height);
        dst.dataSize[p] = (src.dataSize[p] > 0) ? src.dataSize[p] : dst.dataSize[p];
        dst.fd[p]       = src.fd[p];
        dst.fdOffset[p] = src.fdOffset[p];
    }
    return dst;
}

} // namespace cv
} // namespace au
