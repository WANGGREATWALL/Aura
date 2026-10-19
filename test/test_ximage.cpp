#if ENABLE_TEST_XIMAGE

#include <climits>
#include <type_traits>
#include <utility>

#include "cv/ximage.h"
#include "gtest/gtest.h"
#include "mm/xmemory.h"

static_assert(std::is_copy_constructible<au::cv::XImage>::value, "an image owner must be copyable");
static_assert(std::is_copy_assignable<au::cv::XImage>::value, "an image owner must support copy assignment");
static_assert(std::is_move_constructible<au::cv::XImage>::value, "an image owner must be movable");
static_assert(std::is_move_assignable<au::cv::XImage>::value, "an image owner must support move assignment");
static_assert(std::is_copy_constructible<au::cv::Image>::value, "a borrowed descriptor must be copyable");
static_assert(std::is_constructible<au::cv::XImage, const au::cv::Image&>::value,
              "an image descriptor must support borrowing");
static_assert(!std::is_constructible<au::cv::XImage, au::cv::Image&&>::value,
              "an Image rvalue cannot prove unique ownership");
static_assert(std::is_assignable<au::cv::XImage&, const au::cv::Image&>::value,
              "an image descriptor must support borrow assignment");
static_assert(!std::is_assignable<au::cv::XImage&, au::cv::Image&&>::value,
              "an Image rvalue cannot transfer ownership");
static_assert(std::is_same<decltype(au::cv::kImageFormatNV12), au::cv::XImageFormat>::value,
              "pixel formats use XImageFormat");
static_assert(std::is_same<decltype(au::cv::kImageColorSpaceRec709), au::cv::XImageColorSpace>::value,
              "color spaces use XImageColorSpace");

TEST(XImage, DefaultsAndOwnedPlaneLayout)
{
    const au::cv::Image empty{};
    EXPECT_FALSE(au::cv::isValid(empty));
    EXPECT_EQ(empty.format, au::cv::kImageFormatInvalid);
    EXPECT_EQ(empty.colorSpace, au::cv::kImageColorSpaceUnspecified);
    EXPECT_EQ(empty.fd[0], -1);

    unsigned char* base = nullptr;
    {
        au::cv::XImage image(5, 3, au::cv::kImageFormatNV12, au::mm::MemType::Pss, au::cv::kImageColorSpaceRec709);
        ASSERT_TRUE(image.isValid());
        const au::cv::Image view = image.view();
        base                     = view.data[0];
        ASSERT_NE(base, nullptr);
        EXPECT_TRUE(au::mm::isManaged(base));
        EXPECT_EQ(view.colorSpace, au::cv::kImageColorSpaceRec709);
        EXPECT_EQ(view.stride[0], 8);
        EXPECT_EQ(view.scanline[0], 4);
        EXPECT_EQ(view.dataSize[0], 32);
        EXPECT_EQ(view.stride[1], 8);
        EXPECT_EQ(view.scanline[1], 2);
        EXPECT_EQ(view.dataSize[1], 16);
        EXPECT_EQ(view.data[1], base + 32);
        EXPECT_EQ(view.fd[0], -1);
        EXPECT_EQ(view.fdOffset[1], 32);
        EXPECT_TRUE(image.isFormat(au::cv::kImageFormatNV12));
        EXPECT_TRUE(image.isFormatIn({au::cv::kImageFormatNV21, au::cv::kImageFormatNV12}));
        EXPECT_FALSE(image.info().empty());

        au::cv::XImage moved(std::move(image));
        EXPECT_FALSE(image.isValid());
        EXPECT_FALSE(image.ownsMemory());
        EXPECT_TRUE(moved.isValid());
        EXPECT_TRUE(moved.ownsMemory());
        EXPECT_EQ(moved.view().data[0], base);

        au::cv::XImage assigned;
        assigned = std::move(moved);
        EXPECT_FALSE(moved.isValid());
        EXPECT_TRUE(assigned.ownsMemory());
        EXPECT_EQ(assigned.view().data[0], base);
        assigned = std::move(assigned);
        EXPECT_EQ(assigned.view().data[0], base);
    }
    EXPECT_FALSE(au::mm::isManaged(base));
}

TEST(XImage, ShallowCopiesBorrowWithoutFreeing)
{
    unsigned char* base = nullptr;
    au::cv::XImage copy;
    au::cv::XImage assigned;
    {
        au::cv::XImage owner(5, 3, au::cv::kImageFormatNV12, au::mm::MemType::Pss, au::cv::kImageColorSpaceRec709);
        ASSERT_TRUE(owner.isValid());
        base                    = owner.view().data[0];
        owner.view().data[0][0] = 17;

        copy = owner;
        EXPECT_FALSE(copy.ownsMemory());
        EXPECT_EQ(copy.view().data[0], base);
        EXPECT_EQ(copy.view().colorSpace, au::cv::kImageColorSpaceRec709);
        EXPECT_EQ(copy.view().data[1], owner.view().data[1]);
        au::cv::XImage movedBorrower(std::move(copy));
        EXPECT_FALSE(copy.isValid());
        EXPECT_FALSE(movedBorrower.ownsMemory());
        EXPECT_EQ(movedBorrower.view().data[0], base);
        copy = movedBorrower;

        au::cv::XImage old(2, 2, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
        ASSERT_TRUE(old.isValid());
        unsigned char* oldData = old.view().data[0];
        old                    = owner;
        EXPECT_FALSE(old.ownsMemory());
        EXPECT_EQ(old.view().data[0], base);
        EXPECT_FALSE(au::mm::isManaged(oldData));
        old = old;
        EXPECT_EQ(old.view().data[0], base);

        const au::cv::Image view = owner.view();
        au::cv::XImage      fromImage(view);
        EXPECT_FALSE(fromImage.ownsMemory());
        EXPECT_EQ(fromImage.view().data[0], base);
        assigned = view;
        EXPECT_FALSE(assigned.ownsMemory());
        EXPECT_EQ(assigned.view().data[0], base);
        fromImage.view().data[0][0] = 29;
        EXPECT_EQ(owner.view().data[0][0], 29);

        owner = owner;
        owner = view;
        EXPECT_TRUE(owner.ownsMemory());
        EXPECT_EQ(owner.view().data[0], base);
        owner = old;
        EXPECT_TRUE(owner.ownsMemory());
        EXPECT_EQ(owner.view().data[0], base);
    }
    EXPECT_FALSE(au::mm::isManaged(base));
    EXPECT_FALSE(copy.ownsMemory());
    EXPECT_FALSE(assigned.ownsMemory());
    copy.reset();
    assigned.reset();
}

TEST(XImage, MoveOwnershipBetweenXImages)
{
    au::cv::XImage owner(3, 2, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    ASSERT_TRUE(owner.isValid());
    unsigned char* base = owner.view().data[0];
    EXPECT_TRUE(owner.ownsMemory());

    au::cv::XImage borrowed(owner);
    EXPECT_FALSE(borrowed.ownsMemory());
    au::cv::XImage moved(std::move(owner));
    EXPECT_FALSE(owner.isValid());
    EXPECT_TRUE(moved.ownsMemory());
    EXPECT_EQ(moved.view().data[0], base);
    EXPECT_EQ(borrowed.view().data[0], base);

    au::cv::XImage destination(2, 2, au::cv::kImageFormatGrayU8, au::mm::MemType::Pss);
    ASSERT_TRUE(destination.isValid());
    unsigned char* oldData = destination.view().data[0];
    destination            = std::move(moved);
    EXPECT_FALSE(moved.isValid());
    EXPECT_TRUE(destination.ownsMemory());
    EXPECT_EQ(destination.view().data[0], base);
    EXPECT_FALSE(au::mm::isManaged(oldData));

    // Moving a borrower over its owner must not release the shared buffer.
    destination = std::move(borrowed);
    EXPECT_FALSE(borrowed.isValid());
    EXPECT_TRUE(destination.ownsMemory());
    EXPECT_EQ(destination.view().data[0], base);
    destination = std::move(destination);
    EXPECT_TRUE(destination.ownsMemory());
    destination.reset();
    EXPECT_FALSE(au::mm::isManaged(base));
}

TEST(XImage, ExternalImageCanOnlyBeBorrowed)
{
    unsigned char external[16] = {1, 2, 3, 77, 77, 77, 77, 77, 4, 5, 6, 88, 88, 88, 88, 88};
    au::cv::Image image{};
    image.width       = 3;
    image.height      = 2;
    image.format      = au::cv::kImageFormatGrayU8;
    image.data[0]     = external;
    image.stride[0]   = 8;
    image.scanline[0] = 2;
    image.dataSize[0] = 16;
    ASSERT_TRUE(au::cv::isValid(image));

    au::cv::XImage borrowed(image);
    EXPECT_FALSE(borrowed.ownsMemory());
    EXPECT_EQ(borrowed.view().data[0], external);
    au::cv::XImage assigned;
    assigned = image;
    EXPECT_FALSE(assigned.ownsMemory());
    EXPECT_EQ(assigned.view().data[0], external);
    external[0] = 42;
    EXPECT_EQ(borrowed.view().data[0][0], 42);

    assigned.reset();
    EXPECT_EQ(external[0], 42);
}

TEST(XImage, BorrowedViewValidationAndPredicates)
{
    au::cv::XImage image(5, 3, au::cv::kImageFormatNV12, au::mm::MemType::Pss);
    ASSERT_TRUE(image.isValid());
    const au::cv::Image original = image.view();
    au::cv::Image       view     = original;

    EXPECT_TRUE(au::cv::isSameWith(original, view));
    EXPECT_TRUE(au::cv::isSameSizeAndFormatWith(original, view));
    EXPECT_TRUE(au::cv::isFormatIn(view, {au::cv::kImageFormatNV21, au::cv::kImageFormatNV12}));
    EXPECT_FALSE(au::cv::isFormatIn(view, {}));

    view.data[1] = nullptr;
    EXPECT_FALSE(au::cv::isValid(view));
    view           = original;
    view.stride[1] = 5;
    EXPECT_FALSE(au::cv::isValid(view));
    EXPECT_FALSE(au::cv::isSameSizeWith(original, view));
    view             = original;
    view.scanline[1] = 1;
    EXPECT_FALSE(au::cv::isValid(view));
    view             = original;
    view.dataSize[1] = 8;
    EXPECT_FALSE(au::cv::isValid(view));
    view            = original;
    view.colorSpace = 999;
    EXPECT_FALSE(au::cv::isValid(view));
    EXPECT_FALSE(au::cv::isSameWith(original, view));
    view             = original;
    view.fdOffset[1] = -1;
    EXPECT_FALSE(au::cv::isValid(view));
}

TEST(XImage, InvalidInputsAndManualLifetime)
{
    EXPECT_FALSE(au::cv::XImage(0, 3, au::cv::kImageFormatNV12).isValid());
    EXPECT_FALSE(au::cv::XImage(5, 0, au::cv::kImageFormatNV12).isValid());
    EXPECT_FALSE(au::cv::XImage(5, 3, au::cv::kImageFormatInvalid).isValid());
    EXPECT_FALSE(au::cv::XImage(5, 3, au::cv::kImageFormatNV12, au::mm::MemType::Pss, 999).isValid());
    EXPECT_FALSE(
        au::cv::XImage(5, 3, au::cv::kImageFormatNV12, au::mm::MemType::Pss, au::cv::kImageColorSpaceUnspecified, 0)
            .isValid());
    EXPECT_FALSE(au::cv::XImage(INT_MAX, INT_MAX, au::cv::kImageFormatRGBAU8, au::mm::MemType::Pss).isValid());

    au::cv::Image owned = au::cv::createImage(3, 3, au::cv::kImageFormatI420, au::mm::MemType::Pss);
    ASSERT_TRUE(au::cv::isValid(owned));
    EXPECT_EQ(owned.stride[0], 8);
    EXPECT_EQ(owned.stride[1], 8);
    EXPECT_EQ(owned.stride[2], 8);
    EXPECT_EQ(owned.fdOffset[1], owned.dataSize[0]);
    EXPECT_EQ(owned.fdOffset[2], owned.dataSize[0] + owned.dataSize[1]);
    EXPECT_TRUE(au::cv::destroyImage(owned));
    EXPECT_FALSE(au::cv::isValid(owned));
    EXPECT_FALSE(au::cv::destroyImage(owned));
}

TEST(XImage, RegisteredFormatsAndColorSpaces)
{
    const int formats[] = {
        au::cv::kImageFormatGrayU8,         au::cv::kImageFormatGrayU16,        au::cv::kImageFormatGrayS16,
        au::cv::kImageFormatGrayU32,        au::cv::kImageFormatGrayS32,        au::cv::kImageFormatGrayF32,
        au::cv::kImageFormatUVU8,           au::cv::kImageFormatRGBU8,          au::cv::kImageFormatBGRU8,
        au::cv::kImageFormatRGBAU8,         au::cv::kImageFormatBGRAU8,         au::cv::kImageFormatARGBU8,
        au::cv::kImageFormatRGBU16,         au::cv::kImageFormatRGBF32,         au::cv::kImageFormatRGBF16,
        au::cv::kImageFormatBGRF16,         au::cv::kImageFormatNV12,           au::cv::kImageFormatNV21,
        au::cv::kImageFormatI420,           au::cv::kImageFormatYV12,           au::cv::kImageFormatP010,
        au::cv::kImageFormatP016,           au::cv::kImageFormatNV12F16,        au::cv::kImageFormatNV21F16,
        au::cv::kImageFormatMipiRGGB10,     au::cv::kImageFormatMipiGRBG10,     au::cv::kImageFormatMipiBGGR10,
        au::cv::kImageFormatMipiGBRG10,     au::cv::kImageFormatRawPackedU10,   au::cv::kImageFormatRawU16,
        au::cv::kImageFormatUnpackedRGGB10, au::cv::kImageFormatUnpackedGRBG10, au::cv::kImageFormatUnpackedBGGR10,
        au::cv::kImageFormatUnpackedGBRG10, au::cv::kImageFormatUnpackedRGGB12, au::cv::kImageFormatUnpackedGRBG12,
        au::cv::kImageFormatUnpackedBGGR12, au::cv::kImageFormatUnpackedGBRG12, au::cv::kImageFormatUnpackedRGGB14,
        au::cv::kImageFormatUnpackedGRBG14, au::cv::kImageFormatUnpackedBGGR14, au::cv::kImageFormatUnpackedGBRG14,
        au::cv::kImageFormatUnpackedRGGB16, au::cv::kImageFormatUnpackedGRBG16, au::cv::kImageFormatUnpackedBGGR16,
        au::cv::kImageFormatUnpackedGBRG16,
    };
    for (int format : formats) {
        au::cv::XImage image(3, 3, format, au::mm::MemType::Pss);
        EXPECT_TRUE(image.isValid()) << "format " << format;
    }
    const int colorSpaces[] = {
        au::cv::kImageColorSpaceUnspecified, au::cv::kImageColorSpaceSRGB,      au::cv::kImageColorSpaceLinearSRGB,
        au::cv::kImageColorSpaceRec709,      au::cv::kImageColorSpaceDisplayP3, au::cv::kImageColorSpaceRec2020PQ,
    };
    for (int colorSpace : colorSpaces) {
        au::cv::XImage image(1, 1, au::cv::kImageFormatRGBU8, au::mm::MemType::Pss, colorSpace);
        EXPECT_TRUE(image.isValid()) << "color space " << colorSpace;
    }
}

#endif  // ENABLE_TEST_XIMAGE
