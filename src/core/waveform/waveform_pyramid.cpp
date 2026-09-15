#include "waveform/waveform_pyramid.h"

#include "media/ffmpeg_time.h"

#include <utility>

namespace subcue {
namespace {

WaveformPeak minMaxRange(const float *samples, qsizetype begin, qsizetype end)
{
    WaveformPeak peak{samples[begin], samples[begin]};
    for (qsizetype index = begin + 1; index < end; ++index) {
        if (samples[index] < peak.min) {
            peak.min = samples[index];
        }
        if (samples[index] > peak.max) {
            peak.max = samples[index];
        }
    }
    return peak;
}

WaveformPeak combinePeaks(const QVector<WaveformPeak> &peaks, qsizetype begin, qsizetype end)
{
    WaveformPeak peak = peaks[begin];
    for (qsizetype index = begin + 1; index < end; ++index) {
        if (peaks[index].min < peak.min) {
            peak.min = peaks[index].min;
        }
        if (peaks[index].max > peak.max) {
            peak.max = peaks[index].max;
        }
    }
    return peak;
}

} // namespace

WaveformPyramid WaveformPyramid::fromMonoFloat(const float *samples, qsizetype count, int sampleRate)
{
    if (!samples || count <= 0 || sampleRate <= 0) {
        return {};
    }

    WaveformLevel level;
    level.samplesPerPeak = kBaseSamplesPerPeak;
    const qsizetype peakCount = (count + kBaseSamplesPerPeak - 1) / kBaseSamplesPerPeak;
    level.peaks.reserve(peakCount);
    for (qsizetype sample = 0; sample < count; sample += kBaseSamplesPerPeak) {
        const qsizetype end = count - sample < kBaseSamplesPerPeak ? count : sample + kBaseSamplesPerPeak;
        level.peaks.push_back(minMaxRange(samples, sample, end));
    }
    return fromBasePeaks(std::move(level.peaks), count, sampleRate);
}

WaveformPyramid WaveformPyramid::fromBasePeaks(
    QVector<WaveformPeak> peaks,
    qint64 sampleCount,
    int sampleRate)
{
    WaveformPyramid pyramid;
    pyramid.sampleRate_ = sampleRate;
    pyramid.sampleCount_ = sampleCount;
    if (peaks.isEmpty() || sampleCount <= 0 || sampleRate <= 0) {
        return pyramid;
    }

    WaveformLevel level;
    level.samplesPerPeak = kBaseSamplesPerPeak;
    level.peaks = std::move(peaks);
    pyramid.levels_.push_back(std::move(level));

    while (pyramid.levels_.back().peaks.size() > 1) {
        const WaveformLevel &previous = pyramid.levels_.back();
        WaveformLevel next;
        next.samplesPerPeak = previous.samplesPerPeak * kLevelFactor;
        const qsizetype nextCount = (previous.peaks.size() + kLevelFactor - 1) / kLevelFactor;
        next.peaks.reserve(nextCount);
        for (qsizetype index = 0; index < previous.peaks.size(); index += kLevelFactor) {
            const qsizetype end = previous.peaks.size() - index < kLevelFactor
                ? previous.peaks.size()
                : index + kLevelFactor;
            next.peaks.push_back(combinePeaks(previous.peaks, index, end));
        }
        pyramid.levels_.push_back(std::move(next));
    }
    return pyramid;
}

MediaTime WaveformPyramid::duration() const
{
    if (sampleRate_ <= 0 || sampleCount_ <= 0) {
        return MediaTime::fromMicroseconds(0);
    }
    return mediaTimeFromTimestamp(sampleCount_, AVRational{1, sampleRate_});
}

qsizetype WaveformPyramid::byteSize() const noexcept
{
    qsizetype bytes = 0;
    for (const WaveformLevel &level : levels_) {
        bytes += static_cast<qsizetype>(level.peaks.size()) * static_cast<qsizetype>(sizeof(WaveformPeak));
    }
    return bytes;
}

const WaveformLevel *WaveformPyramid::levelForSamplesPerPeak(int samplesPerPeak) const
{
    if (levels_.isEmpty()) {
        return nullptr;
    }
    const WaveformLevel *chosen = &levels_.first();
    for (const WaveformLevel &level : levels_) {
        if (level.samplesPerPeak <= samplesPerPeak) {
            chosen = &level;
        } else {
            break;
        }
    }
    return chosen;
}

QVector<WaveformPeak> WaveformPyramid::peaksForRange(MediaTime start, MediaTime end, int columns) const
{
    QVector<WaveformPeak> output;
    if (columns <= 0 || sampleRate_ <= 0 || sampleCount_ <= 0 || levels_.isEmpty()) {
        return output;
    }
    if (end.microseconds() <= start.microseconds()) {
        return output;
    }

    qint64 startSample = av_rescale_q(start.microseconds(), kMicrosecondTimeBase, AVRational{1, sampleRate_});
    qint64 endSample = av_rescale_q(end.microseconds(), kMicrosecondTimeBase, AVRational{1, sampleRate_});
    if (startSample < 0) {
        startSample = 0;
    }
    if (endSample > sampleCount_) {
        endSample = sampleCount_;
    }
    if (endSample <= startSample) {
        return output;
    }

    const qint64 rangeSamples = endSample - startSample;
    int samplesPerColumn = static_cast<int>((rangeSamples + columns - 1) / columns);
    if (samplesPerColumn < 1) {
        samplesPerColumn = 1;
    }
    const WaveformLevel *level = levelForSamplesPerPeak(samplesPerColumn);
    if (!level || level->peaks.isEmpty()) {
        return output;
    }

    output.resize(columns);
    for (int column = 0; column < columns; ++column) {
        const qint64 columnStart = startSample + rangeSamples * column / columns;
        const qint64 columnEnd = startSample + rangeSamples * (column + 1) / columns;
        qsizetype peakStart = static_cast<qsizetype>(columnStart / level->samplesPerPeak);
        qsizetype peakEnd = static_cast<qsizetype>((columnEnd + level->samplesPerPeak - 1) / level->samplesPerPeak);
        if (peakStart < 0) {
            peakStart = 0;
        }
        const qsizetype peakCount = level->peaks.size();
        if (peakStart >= peakCount) {
            continue;
        }
        if (peakEnd <= peakStart) {
            peakEnd = peakStart + 1;
        }
        if (peakEnd > peakCount) {
            peakEnd = peakCount;
        }
        output[column] = combinePeaks(level->peaks, peakStart, peakEnd);
    }
    return output;
}

} // namespace subcue
