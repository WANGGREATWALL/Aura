#ifndef AURA_CV_XIMAGE_TYPES_H_
#define AURA_CV_XIMAGE_TYPES_H_

// Aura image identifiers are independent of vendor SDK numeric values.
// Once published, these explicit values must not be reused for other formats.
namespace au {
namespace cv {

enum XImageFormat : int
{
    kImageFormatInvalid        = 0,
    kImageFormatGrayU8         = 0x100,
    kImageFormatGrayU16        = 0x101,
    kImageFormatGrayS16        = 0x102,
    kImageFormatGrayU32        = 0x103,
    kImageFormatGrayS32        = 0x104,
    kImageFormatGrayF32        = 0x105,
    kImageFormatUVU8           = 0x106,
    kImageFormatBGRU8          = 0x200,
    kImageFormatRGBU8          = 0x201,
    kImageFormatBGRAU8         = 0x202,
    kImageFormatARGBU8         = 0x203,
    kImageFormatRGBAU8         = 0x204,
    kImageFormatRGBU16         = 0x205,
    kImageFormatRGBF32         = 0x206,
    kImageFormatRGBF16         = 0x207,
    kImageFormatBGRF16         = 0x208,
    kImageFormatNV12           = 0x300,
    kImageFormatNV21           = 0x301,
    kImageFormatI420           = 0x302,
    kImageFormatYV12           = 0x303,
    kImageFormatP010           = 0x304,
    kImageFormatP016           = 0x305,
    kImageFormatNV12F16        = 0x306,
    kImageFormatNV21F16        = 0x307,
    kImageFormatMipiRGGB10     = 0x400,
    kImageFormatMipiGRBG10     = 0x401,
    kImageFormatMipiBGGR10     = 0x402,
    kImageFormatMipiGBRG10     = 0x403,
    kImageFormatRawPackedU10   = 0x404,
    kImageFormatRawU16         = 0x405,
    kImageFormatUnpackedRGGB10 = 0x406,
    kImageFormatUnpackedGRBG10 = 0x407,
    kImageFormatUnpackedBGGR10 = 0x408,
    kImageFormatUnpackedGBRG10 = 0x409,
    kImageFormatUnpackedRGGB12 = 0x40A,
    kImageFormatUnpackedGRBG12 = 0x40B,
    kImageFormatUnpackedBGGR12 = 0x40C,
    kImageFormatUnpackedGBRG12 = 0x40D,
    kImageFormatUnpackedRGGB14 = 0x40E,
    kImageFormatUnpackedGRBG14 = 0x40F,
    kImageFormatUnpackedBGGR14 = 0x410,
    kImageFormatUnpackedGBRG14 = 0x411,
    kImageFormatUnpackedRGGB16 = 0x412,
    kImageFormatUnpackedGRBG16 = 0x413,
    kImageFormatUnpackedBGGR16 = 0x414,
    kImageFormatUnpackedGBRG16 = 0x415,
};

// A color space describes interpretation, not the plane memory layout.
// Unspecified is appropriate for raw sensor data and unknown metadata.
enum XImageColorSpace : int
{
    kImageColorSpaceUnspecified = 0,
    kImageColorSpaceSRGB        = 1,
    kImageColorSpaceLinearSRGB  = 2,
    kImageColorSpaceRec709      = 3,
    kImageColorSpaceDisplayP3   = 4,
    kImageColorSpaceRec2020PQ   = 5,
};

// Plain image descriptor with no automatic resource management. createImage()
// returns a managed allocation that must be destroyed with destroyImage();
// XImage::view() returns a borrowed copy whose owner must outlive its use.
// stride and dataSize are bytes; scanline is a row count. fd == -1 means
// the plane has no file descriptor. A valid descriptor has format-specific
// active planes with sufficient stride, scanline and dataSize.
struct Image
{
    int            colorSpace  = kImageColorSpaceUnspecified;
    int            format      = kImageFormatInvalid;
    int            width       = 0;
    int            height      = 0;
    unsigned char* data[4]     = {};
    int            stride[4]   = {};
    int            scanline[4] = {};
    int            dataSize[4] = {};
    int            fd[4]       = {-1, -1, -1, -1};
    int            fdOffset[4] = {};
};

}  // namespace cv
}  // namespace au

#endif  // AURA_CV_XIMAGE_TYPES_H_
