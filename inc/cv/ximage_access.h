#ifndef AURA_CV_XIMAGE_ACCESS_H_
#define AURA_CV_XIMAGE_ACCESS_H_
#include "cv/ximage.h"
#include "mm/xaccess.h"

namespace au {
namespace cv {
// Validate geometry without retaining storage. begin() resolves every range to
// a registered allocation and checks the real capacity before any CPU access.
AU_API au::mm::Access imageAccess(const au::cv::Image& image, au::mm::AccessMode mode) noexcept;
inline au::mm::Access readAccess(const au::cv::Image& image) noexcept
{ return au::cv::imageAccess(image, au::mm::AccessMode::Read); }
inline au::mm::Access writeAccess(au::cv::Image& image) noexcept
{ return au::cv::imageAccess(image, au::mm::AccessMode::Write); }
inline au::mm::Access readWriteAccess(au::cv::Image& image) noexcept
{ return au::cv::imageAccess(image, au::mm::AccessMode::ReadWrite); }
inline au::mm::Access readAccess(const au::cv::XImage& image) noexcept
{ return au::cv::imageAccess(image.view(), au::mm::AccessMode::Read); }
inline au::mm::Access writeAccess(au::cv::XImage& image) noexcept
{ return au::cv::imageAccess(image.view(), au::mm::AccessMode::Write); }
inline au::mm::Access readWriteAccess(au::cv::XImage& image) noexcept
{ return au::cv::imageAccess(image.view(), au::mm::AccessMode::ReadWrite); }
} // namespace cv
} // namespace au
#endif
