#pragma once

#include "common/app_error.h"
#include "media/media_types.h"
#include "waveform/waveform_pyramid.h"

#include <QtCore/QString>

#include <atomic>
#include <cstdint>
#include <memory>

namespace subcue {

class WaveformGenerator final {
public:
    static constexpr int kDefaultSampleRate = 8'000;

    using Result = MediaResult<std::shared_ptr<const WaveformPyramid>>;

    [[nodiscard]] Result generate(
        const QString &path,
        int streamIndex = -1,
        int sampleRate = kDefaultSampleRate,
        const std::atomic<bool> *cancel = nullptr,
        const std::atomic<quint64> *generation = nullptr,
        quint64 expectedGeneration = 0) const;
};

} // namespace subcue
