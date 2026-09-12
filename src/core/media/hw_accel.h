#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QString>
#include <QtCore/QVector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace subcue {

enum class HwAccelType {
    D3D11VA,
    D3D12VA,
    QSV,
    CUDA,
    Software
};

struct HwAccelAttempt final {
    HwAccelType type = HwAccelType::Software;
    bool succeeded = false;
    QString details;
};

[[nodiscard]] QVector<HwAccelType> hwAccelFallbackOrder();
[[nodiscard]] QString hwAccelName(HwAccelType type);
[[nodiscard]] AVHWDeviceType avDeviceType(HwAccelType type) noexcept;
[[nodiscard]] bool codecSupportsHwAccel(const AVCodec *codec, HwAccelType type, AVPixelFormat *pixelFormat);
[[nodiscard]] BufferRefPtr createHwDevice(HwAccelType type, AppError *error = nullptr);
[[nodiscard]] bool transferFrameToSoftware(const AVFrame &source, AVFrame &destination, AppError *error = nullptr);

} // namespace subcue
