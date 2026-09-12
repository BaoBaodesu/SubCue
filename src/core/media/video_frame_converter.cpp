#include "media/video_frame_converter.h"

#include <algorithm>

namespace subcue {

QImage VideoFrameConverter::convert(const AVFrame &frame, QSize targetSize, AppError *error)
{
    const int width = targetSize.isValid() ? targetSize.width() : frame.width;
    const int height = targetSize.isValid() ? targetSize.height() : frame.height;
    if (frame.width <= 0 || frame.height <= 0 || width <= 0 || height <= 0) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("视频帧尺寸无效"));
        }
        return {};
    }

    context_.reset(sws_getCachedContext(
        context_.release(),
        frame.width,
        frame.height,
        static_cast<AVPixelFormat>(frame.format),
        width,
        height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));
    if (!context_) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法创建视频转换器"));
        }
        return {};
    }

    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频图像"));
        }
        return {};
    }
    uint8_t *destination[] = {image.bits(), nullptr, nullptr, nullptr};
    int destinationLines[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    const int converted = sws_scale(
        context_.get(), frame.data, frame.linesize, 0, frame.height, destination, destinationLines);
    if (converted != height) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, converted, QStringLiteral("视频帧转换失败"));
        }
        return {};
    }
    return image;
}

} // namespace subcue
