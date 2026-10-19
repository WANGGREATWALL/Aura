#ifndef AURA_CV_XIMAGE_H_
#define AURA_CV_XIMAGE_H_

#include "cv/ximage_api.h"

namespace au {
namespace cv {

// C++11 image handle. Copies borrow the same storage; moving an owning handle
// transfers ownership. A borrowed handle must not outlive the storage owner.
// The wrapper is header-only and has no exported C++ class ABI.
class XImage final
{
public:
    XImage() noexcept = default;

    XImage(int width, int height, int format, au::mm::MemType memType = kDefaultImageMemType,
           int colorSpace = kDefaultImageColorSpace, int strideAlignBytes = kDefaultImageStrideAlignBytes,
           int scanlineAlignRows = kDefaultImageScanlineAlignRows, const char* tag = nullptr) noexcept
        : mImage(createImage(width, height, format, memType, colorSpace, strideAlignBytes, scanlineAlignRows, tag)),
          mOwnsMemory(mImage.data[0] != nullptr)
    {
    }

    ~XImage() noexcept { reset(); }

    // Copies borrow every plane, including when the source XImage owns them.
    explicit XImage(const Image& other) noexcept : mImage(other) {}
    XImage(const XImage& other) noexcept : mImage(other.mImage) {}

    XImage& operator=(const Image& other) noexcept
    {
        // Save the descriptor first: other may be a view of our allocation.
        const Image borrowed = other;
        if (borrowed.data[0] != mImage.data[0]) {
            reset();
            mOwnsMemory = false;
        }
        mImage = borrowed;
        return *this;
    }

    XImage& operator=(const XImage& other) noexcept
    {
        if (this != &other) {
            *this = other.mImage;
        }
        return *this;
    }

    XImage(XImage&& other) noexcept : mImage(other.mImage), mOwnsMemory(other.mOwnsMemory) { other.clearWithoutFree(); }

    XImage& operator=(XImage&& other) noexcept
    {
        if (this != &other) {
            const Image incoming      = other.mImage;
            const bool  incomingOwns  = other.mOwnsMemory;
            const bool  sameStorage   = incoming.data[0] != nullptr && incoming.data[0] == mImage.data[0];
            const bool  keepOwnership = sameStorage && mOwnsMemory;
            if (!sameStorage) {
                reset();
            }
            mImage      = incoming;
            mOwnsMemory = incomingOwns || keepOwnership;
            other.clearWithoutFree();
        }
        return *this;
    }

    // An Image descriptor carries no proof of unique ownership. Rvalue
    // descriptors therefore cannot transfer storage into this RAII handle.
    XImage(Image&& other)            = delete;
    XImage& operator=(Image&& other) = delete;

    void reset() noexcept
    {
        if (mOwnsMemory && mImage.data[0] != nullptr) {
            (void)destroyImage(mImage);
        }
        clearWithoutFree();
    }

    // The returned descriptor borrows this object's data and file descriptor.
    Image view() const noexcept { return mImage; }
    bool  ownsMemory() const noexcept { return mOwnsMemory; }

    bool isValid() const noexcept { return au::cv::isValid(mImage); }
    bool isFormat(int format) const noexcept { return au::cv::isFormat(mImage, format); }
    bool isFormatIn(std::initializer_list<int> formats) const noexcept { return au::cv::isFormatIn(mImage, formats); }
    bool isSameWith(const Image& other) const noexcept { return au::cv::isSameWith(mImage, other); }
    bool isSameSizeWith(const Image& other) const noexcept { return au::cv::isSameSizeWith(mImage, other); }
    bool isSameFormatWith(const Image& other) const noexcept { return au::cv::isSameFormatWith(mImage, other); }
    bool isSameSizeAndFormatWith(const Image& other) const noexcept
    {
        return au::cv::isSameSizeAndFormatWith(mImage, other);
    }

    int         invalidateCache() const noexcept { return au::cv::invalidateImageCache(mImage); }
    int         flushCache() const noexcept { return au::cv::flushImageCache(mImage); }
    std::string info() const noexcept { return au::cv::info(mImage); }

private:
    void clearWithoutFree() noexcept
    {
        mImage      = Image{};
        mOwnsMemory = false;
    }

    Image mImage{};
    bool  mOwnsMemory = false;
};

}  // namespace cv
}  // namespace au

#endif  // AURA_CV_XIMAGE_H_
