#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QImage>

#include <variant>

namespace subcue {

enum class MediaStreamType {
    Unknown,
    Video,
    Audio,
    Subtitle
};

struct MediaStreamInfo final {
    int index = -1;
    MediaStreamType type = MediaStreamType::Unknown;
    QString codecName;
    QString language;
    MediaTime duration = MediaTime::fromMicroseconds(-1);
    int timeBaseNumerator = 0;
    int timeBaseDenominator = 1;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    int channels = 0;
    double averageFrameRate = 0.0;
    double realFrameRate = 0.0;
    bool variableFrameRate = false;
};

struct MediaInfo final {
    QString path;
    QString formatName;
    MediaTime duration = MediaTime::fromMicroseconds(-1);
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    bool variableFrameRate = false;
    QVector<MediaStreamInfo> streams;
};

struct AudioBuffer final {
    QVector<float> samples;
    int sampleRate = 0;
    int channels = 0;
    MediaTime startTime = MediaTime::fromMicroseconds(-1);
};

template<typename T>
using MediaResult = std::variant<T, AppError>;

using ProbeResult = MediaResult<MediaInfo>;
using VideoFrameResult = MediaResult<QImage>;
using AudioBufferResult = MediaResult<AudioBuffer>;

} // namespace subcue
