#pragma once

#include "alignment/transcript.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

struct RoughCutAlignedWord final {
    QString text;
    qint64 startSample = 0;
    qint64 endSample = 0;
};

struct RoughCutAlignmentResult final {
    QVector<RoughCutAlignedWord> words;
    bool trustworthy = false;
    QString reviewReason;
};

struct RoughCutSilenceEvidence final {
    qint64 startSample = 0;
    qint64 endSample = 0;
    double peak = 0.0;
};

class RoughCutAlignmentEvidence final {
public:
    [[nodiscard]] static RoughCutAlignmentResult validate(
        const Transcript &transcript, int sourceSampleRate, qint64 sourceSampleCount,
        bool preciseWordTiming);

    // 这里只提供能量静音证据，不把静音区间解释为可靠的语音分类结果。
    [[nodiscard]] static QVector<RoughCutSilenceEvidence> detectSilence(
        const QVector<float> &monoPcm, int pcmSampleRate, int sourceSampleRate,
        qint64 sourceStartSample = 0, double peakThreshold = 0.01,
        int minimumSilenceMs = 120);
};

} // namespace subcue
