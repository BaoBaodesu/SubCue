#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <cstdint>

namespace subcue {

struct RoughCutSourceClip final {
    QString name;
    qint64 sourceStartSample = 0;
    qint64 sourceEndSample = 0;
    qint64 timelineStartSample = -1;
};

struct RoughCutExportRequest final {
    QString sequenceName = QStringLiteral("SubCue Rough Cut");
    QString mediaPath;
    int sampleRate = 0;
    int channels = 0;
    qint64 sourceSampleCount = 0;
    int frameRateNumerator = 60;
    int frameRateDenominator = 1;
    QList<RoughCutSourceClip> clips;
};

} // namespace subcue
