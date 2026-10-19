#ifndef AURA_CV_XIMAGE_IO_H_
#define AURA_CV_XIMAGE_IO_H_

/**
 * @file ximage_io.h
 * @brief XImage load/save utilities.
 *
 * Supported formats: JPEG, PNG, NV21/NV12, gray, raw.
 *
 * @example
 *   auto img = au::cv::XImageIO::load("/data/photo.jpg");
 *   au::cv::XImageIO::dump(img, "/data/output");
 */

#include "cv/ximage.h"

namespace au { namespace cv {

class XImageIO {
public:
    static void setDumpEnable(bool enable);
    static void setDumpFolder(const std::string& folder);
    static void updateDumpTimestamp();

    /**
     * @brief Load an image from file.
     * @param pathFull Full path with extension, e.g. "/data/image.jpg", "/data/img_512x512.nv12".
     */
    static au::cv::XImage load(const std::string& pathFull);

    /**
     * @brief Dump image to file.
     * @param image The image to save.
     * @param nameWithoutFormat Filename without extension (format auto-determined).
     */
    static int dump(const au::cv::Image& image, const std::string& nameWithoutFormat);
};

}}  // namespace au::cv

#endif // AURA_CV_XIMAGE_IO_H_
