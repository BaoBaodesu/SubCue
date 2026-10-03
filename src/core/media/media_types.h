#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
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
    int frameRateNumerator = 0;
    int frameRateDenominator = 1;
    MediaTime startTime;
    int pixelAspectNumerator = 1;
    int pixelAspectDenominator = 1;
};

struct MediaInfo final {
    QString path;
    QString formatName;
    MediaTime duration = MediaTime::fromMicroseconds(-1);
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;
    bool variableFrameRate = false;
    QVector<MediaStreamInfo> streams;
    bool cfrVerified = false;
};

inline QJsonObject mediaInfoToJson(const MediaInfo &info)
{
    QJsonArray streams;
    for (const auto &s : info.streams)
        streams.append(QJsonObject{{QStringLiteral("index"), s.index}, {QStringLiteral("type"), int(s.type)},
            {QStringLiteral("codec"), s.codecName}, {QStringLiteral("language"), s.language},
            {QStringLiteral("timeBaseNum"), s.timeBaseNumerator}, {QStringLiteral("timeBaseDen"), s.timeBaseDenominator},
            {QStringLiteral("durationUs"), s.duration.microseconds()}, {QStringLiteral("startUs"), s.startTime.microseconds()},
            {QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height},
            {QStringLiteral("sampleRate"), s.sampleRate}, {QStringLiteral("channels"), s.channels},
            {QStringLiteral("fpsNum"), s.frameRateNumerator}, {QStringLiteral("fpsDen"), s.frameRateDenominator},
            {QStringLiteral("sarNum"), s.pixelAspectNumerator}, {QStringLiteral("sarDen"), s.pixelAspectDenominator}});
    return {{QStringLiteral("video"), info.videoStreamIndex}, {QStringLiteral("audio"), info.audioStreamIndex},
        {QStringLiteral("durationUs"), info.duration.microseconds()}, {QStringLiteral("vfr"), info.variableFrameRate},
        {QStringLiteral("cfrVerified"), info.cfrVerified}, {QStringLiteral("streams"), streams}};
}

inline MediaInfo mediaInfoFromJson(const QJsonObject &json)
{
    MediaInfo info;
    info.videoStreamIndex = json.value(QStringLiteral("video")).toInt(-1);
    info.audioStreamIndex = json.value(QStringLiteral("audio")).toInt(-1);
    info.duration = MediaTime::fromMicroseconds(json.value(QStringLiteral("durationUs")).toInteger());
    info.variableFrameRate = json.value(QStringLiteral("vfr")).toBool();
    info.cfrVerified = json.value(QStringLiteral("cfrVerified")).toBool();
    for (const auto &value : json.value(QStringLiteral("streams")).toArray()) {
        const auto jsonStream = value.toObject();
        MediaStreamInfo s;
        s.index = jsonStream.value(QStringLiteral("index")).toInt(-1);
        s.codecName = jsonStream.value(QStringLiteral("codec")).toString();
        s.language = jsonStream.value(QStringLiteral("language")).toString();
        s.timeBaseNumerator = jsonStream.value(QStringLiteral("timeBaseNum")).toInt();
        s.timeBaseDenominator = jsonStream.value(QStringLiteral("timeBaseDen")).toInt(1);
        s.type = MediaStreamType(jsonStream.value(QStringLiteral("type")).toInt());
        s.duration = MediaTime::fromMicroseconds(jsonStream.value(QStringLiteral("durationUs")).toInteger());
        s.startTime = MediaTime::fromMicroseconds(jsonStream.value(QStringLiteral("startUs")).toInteger());
        s.width = jsonStream.value(QStringLiteral("width")).toInt();
        s.height = jsonStream.value(QStringLiteral("height")).toInt();
        s.sampleRate = jsonStream.value(QStringLiteral("sampleRate")).toInt();
        s.channels = jsonStream.value(QStringLiteral("channels")).toInt();
        s.frameRateNumerator = jsonStream.value(QStringLiteral("fpsNum")).toInt();
        s.frameRateDenominator = jsonStream.value(QStringLiteral("fpsDen")).toInt(1);
        s.pixelAspectNumerator = jsonStream.value(QStringLiteral("sarNum")).toInt(1);
        s.pixelAspectDenominator = jsonStream.value(QStringLiteral("sarDen")).toInt(1);
        s.averageFrameRate = s.frameRateDenominator > 0 ? double(s.frameRateNumerator) / s.frameRateDenominator : 0.0;
        info.streams.append(s);
    }
    return info;
}

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
