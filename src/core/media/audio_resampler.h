#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QVector>

namespace subcue {

class AudioResampler final {
public:
    [[nodiscard]] bool configure(
        const AVCodecContext &decoder,
        int outputSampleRate = 48'000,
        int outputChannels = 2,
        AppError *error = nullptr);
    [[nodiscard]] QVector<float> convert(const AVFrame &frame, AppError *error = nullptr);

    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }

private:
    SwrContextPtr context_;
    int sampleRate_ = 0;
    int channels_ = 0;
};

} // namespace subcue
