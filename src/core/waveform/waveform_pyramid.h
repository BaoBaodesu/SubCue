#pragma once

#include "common/media_time.h"

#include <QtCore/QVector>

namespace subcue {

struct WaveformPeak final {
    float min = 0.0f;
    float max = 0.0f;
};

struct WaveformLevel final {
    int samplesPerPeak = 0;
    QVector<WaveformPeak> peaks;
};

class WaveformPyramid final {
public:
    static constexpr int kBaseSamplesPerPeak = 16;
    static constexpr int kLevelFactor = 4;

    [[nodiscard]] static WaveformPyramid fromMonoFloat(
        const float *samples,
        qsizetype count,
        int sampleRate);

    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] qint64 sampleCount() const noexcept { return sampleCount_; }
    [[nodiscard]] MediaTime duration() const;
    [[nodiscard]] const QVector<WaveformLevel> &levels() const noexcept { return levels_; }
    [[nodiscard]] qsizetype byteSize() const noexcept;

    [[nodiscard]] const WaveformLevel *levelForSamplesPerPeak(int samplesPerPeak) const;
    [[nodiscard]] QVector<WaveformPeak> peaksForRange(MediaTime start, MediaTime end, int columns) const;

private:
    int sampleRate_ = 0;
    qint64 sampleCount_ = 0;
    QVector<WaveformLevel> levels_;
};

} // namespace subcue
