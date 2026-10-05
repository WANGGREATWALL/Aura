#ifndef __VIVO_COMDEF_H__
#define __VIVO_COMDEF_H__

#include "vivo_comdef_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 我们在图像结构中引入色彩空间的概念，色彩空间和图像格式的区分见下表：
 *     +================+========================================+========================================+
 *     | 区分维度 |             色彩空间                  |                  图像格式               |
 *     +================+========================================+========================================+
 *     |   定义   | 数值如何转为视觉色彩                   | 数据在内存中的排列方式                   |
 *     +----------------+----------------------------------------+----------------------------------------+
 *     |  关注点  | 颜色解释（色域、白点、传递函数）       | 数据布局（通道深度、位深、内存对齐）     |
 *     +----------------+----------------------------------------+----------------------------------------+
 *     | 可变性   | 同一数值在不同色彩空间下表现不同颜色   | 同一颜色可用不同格式存储（如RGB、BGR）   |
 *     +----------------+----------------------------------------+----------------------------------------+
 *     | 典型示例 | sRGB、Adobe RGB、Rec.709、CIE Lab       | RGB24、RGBA32、YUV420、Gray8、Raw         |
 *     +----------------+----------------------------------------+----------------------------------------+
 *     | 数学关系 | 通过矩阵/曲线变换（如RGB -> XYZ）      | 通过内存操作转换（如RGB -> BGR）         |
 *     +================+========================================+========================================+
 * 后续的新图像类型，请参照上表的区分，合理选择在colorSpace或format上衍生新类型。
 */

/* 定义颜色值的解释和转换方式，包括但不限于：
 *   1. 色域；
 *   2. 白点；
 *   3. 传递函数；
 */
typedef enum VImageColorSpace {
    kVIColorSpaceGamutBase    = 0x1000,
    kVIColorSpaceTransferBase = 0x2000,

    // If you don't care the color space
    kVIColorSpaceDefault = 0x0000,

    // Color Gamut types
    kVIColorSpacePQ = kVIColorSpaceTransferBase | 0x200,
} VImageColorSpace;

/* 定义数据在内存中的排列和访问方式，包括但不限于：
 *   1. 排列方式：rgb, yuv, mipi, unpack, ubwc, p010, p016, single-plane or multi-plane, big-endian, little-endian and so on.
 *   2. 数据类型：U8(uint8_t), (S8)int8_t, U16(uint16_t), S16(int16_t) and so on.
 */
typedef enum VImageFormatF {
    // format base: NOTE! should not use these enumeration as format value of an image!
    kVIFormatGrayBase = 0x8000,
    kVIFormatRgbBase  = 0x9000,
    kVIFormatYuvBase  = 0xA000,
    kVIFormatRawBase  = 0xB000,

    // Rgb formats
    // kVIFormatRGB_S8  = kVIFormatRgbBase | 0x100, //
    // kVIFormatBGR_S8  = kVIFormatRgbBase | 0x101, //
    // kVIFormatRGB_U8  = kVIFormatRgbBase | 0x102, // kVIFormatR8G8B8
    // kVIFormatBGR_U8  = kVIFormatRgbBase | 0x103, // kVIFormatB8G8R8
    // kVIFormatRGB_S16 = kVIFormatRgbBase | 0x104,
    // kVIFormatBGR_S16 = kVIFormatRgbBase | 0x105,
    // kVIFormatRGB_U16 = kVIFormatRgbBase | 0x106, // kVIFormatR16G16B16
    // kVIFormatBGR_U16 = kVIFormatRgbBase | 0x107, // kVIFormatB16G16R16
    // kVIFormatRGB_S32 = kVIFormatRgbBase | 0x108,
    // kVIFormatBGR_S32 = kVIFormatRgbBase | 0x109,
    // kVIFormatRGB_U32 = kVIFormatRgbBase | 0x10A, // kVIFormatR32G32B32
    // kVIFormatBGR_U32 = kVIFormatRgbBase | 0x10B, // kVIFormatB32G32R32
    kVIFormatRGB_F16 = kVIFormatRgbBase | 0x10C,
    kVIFormatBGR_F16 = kVIFormatRgbBase | 0x10D,
    // kVIFormatRGB_F32 = kVIFormatRgbBase | 0x10E, // kVIFormatRF32GF32BF32
    // kVIFormatBGR_F32 = kVIFormatRgbBase | 0x10F, // kVIFormatBF32GF32RF32

    // Yuv formats
    kVIFormatNV21_F16 = kVIFormatYuvBase | 0x100,
    kVIFormatNV12_F16 = kVIFormatYuvBase | 0x101,

    // Raw mipi formats
    kVIFormatMipiRGGB10 = kVIFormatRawBase | 0x100,
    kVIFormatMipiGRBG10 = kVIFormatRawBase | 0x101,
    kVIFormatMipiBGGR10 = kVIFormatRawBase | 0x102,
    kVIFormatMipiGBRG10 = kVIFormatRawBase | 0x103,
    // Raw unpacked formats
    kVIFormatUnpackedRGGB10 = kVIFormatRawBase | 0x200,
    kVIFormatUnpackedGRBG10 = kVIFormatRawBase | 0x201,
    kVIFormatUnpackedBGGR10 = kVIFormatRawBase | 0x202,
    kVIFormatUnpackedGBRG10 = kVIFormatRawBase | 0x203,
    kVIFormatUnpackedRGGB12 = kVIFormatRawBase | 0x204,
    kVIFormatUnpackedGRBG12 = kVIFormatRawBase | 0x205,
    kVIFormatUnpackedBGGR12 = kVIFormatRawBase | 0x206,
    kVIFormatUnpackedGBRG12 = kVIFormatRawBase | 0x207,
    kVIFormatUnpackedRGGB14 = kVIFormatRawBase | 0x208,
    kVIFormatUnpackedGRBG14 = kVIFormatRawBase | 0x209,
    kVIFormatUnpackedBGGR14 = kVIFormatRawBase | 0x20A,
    kVIFormatUnpackedGBRG14 = kVIFormatRawBase | 0x20B,
    kVIFormatUnpackedRGGB16 = kVIFormatRawBase | 0x20C,
    kVIFormatUnpackedGRBG16 = kVIFormatRawBase | 0x20D,
    kVIFormatUnpackedBGGR16 = kVIFormatRawBase | 0x20E,
    kVIFormatUnpackedGBRG16 = kVIFormatRawBase | 0x20F,
} VImageFormatF;

/* Defined to extend VImage and replace VImageEx, VImageExV1, VRawImage, VRawImageV1.
 * VImageF means VImage Full, and the next of E (VImageExV1) is F in alphabet table.
 */
typedef struct VImageF {
    int            colorSpace;  // see VImageColorSpace
    int            format;      // see VImageFormat, VImageFormatF
    int            width;       // image width
    int            height;      // image height
    unsigned char* data[4];     // ptr of image data, NULL means invalid
    int            stride[4];   // col-alignment bytes of image memory data
    int            scanline[4]; // row-alignment bytes of image memory data
    int            dataSize[4]; // data size of every image data plane
    int            fd[4];       // file descriptors, ZERO means invalid
    int            fdOffset[4]; // data offset from mmap on fd
} VImageF, *PVImageF;

/* 用于算法暴露内部生成式AI模型的类别，说明如下：
 *   1.各个算法应该通过GetParam接口（key由各个算法自行定义），值类型应为uint64_t;
 *   2.这里定义的枚举值代表信息在uint64_t结果的哪一位上，比如kVAlgoModelTypeSD为0，则表示返回的值的第0位标识算法是否使用了SD模型;
 */
typedef enum VAlgoModelType {
    kVAlgoModelTypeSD = 0,
} VAlgoModelType;

#ifdef __cplusplus
}
#endif

#endif /* _VIVO_COMDEF_H_ */
