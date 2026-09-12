#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

namespace subcue {

class VideoFrameConverter final {
public:
    [[nodiscard]] QImage convert(const AVFrame &frame, QSize targetSize = {}, AppError *error = nullptr);

private:
    SwsContextPtr context_;
};

} // namespace subcue
