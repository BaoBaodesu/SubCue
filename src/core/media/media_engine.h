#pragma once

#include "common/media_time.h"
#include "media/media_types.h"

#include <QtCore/QSize>

namespace subcue {

class MediaEngine final {
public:
    [[nodiscard]] ProbeResult probe(const QString &path) const;
    [[nodiscard]] VideoFrameResult decodeFirstVideoFrame(const QString &path, QSize targetSize = {}) const;
    [[nodiscard]] AudioBufferResult decodeAudio(
        const QString &path,
        MediaTime maximumDuration = MediaTime::fromSeconds(1.0),
        int outputSampleRate = 48'000,
        int outputChannels = 2) const;
};

} // namespace subcue
