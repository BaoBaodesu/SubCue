#include "roughcut/alignment_evidence.h"

#include "alignment/normalizer.h"

#include <algorithm>
#include <cmath>

namespace subcue {
namespace {

qint64 millisecondsToSamples(qint64 milliseconds, int sampleRate)
{
    return (milliseconds * static_cast<qint64>(sampleRate) + 500) / 1000;
}

qint64 pcmToSourceSample(qint64 pcmSample, int pcmRate, int sourceRate, qint64 sourceStart)
{
    return sourceStart + (pcmSample * static_cast<qint64>(sourceRate) + pcmRate / 2) / pcmRate;
}

} // namespace

RoughCutAlignmentResult RoughCutAlignmentEvidence::validate(
    const Transcript &transcript, int sourceSampleRate, qint64 sourceSampleCount,
    bool preciseWordTiming)
{
    RoughCutAlignmentResult result;
    if (sourceSampleRate <= 0 || sourceSampleCount <= 0) {
        result.reviewReason = QStringLiteral("源音频采样信息无效");
        return result;
    }
    if (!preciseWordTiming) {
        result.reviewReason = QStringLiteral("仅有分块粗定位，不能据此裁切");
        return result;
    }
    for (const TranscriptWord &word : transcript.words) {
        if (word.text.trimmed().isEmpty() || word.startMs < 0 || word.endMs <= word.startMs) {
            result.reviewReason = QStringLiteral("字词时间范围无效");
            return result;
        }
        RoughCutAlignedWord converted{word.text,
            millisecondsToSamples(word.startMs, sourceSampleRate),
            millisecondsToSamples(word.endMs, sourceSampleRate)};
        if (converted.endSample > sourceSampleCount || converted.startSample < 0) {
            result.reviewReason = QStringLiteral("字词时间超出源音频范围");
            return result;
        }
        if (!result.words.isEmpty()) {
            const RoughCutAlignedWord &previous = result.words.constLast();
            if (converted.startSample < previous.startSample) {
                result.reviewReason = QStringLiteral("字词时间顺序无效");
                result.words.clear();
                return result;
            }
            if (Normalizer::normalizeText(previous.text)
                    == Normalizer::normalizeText(converted.text)
                && converted.startSample < previous.endSample) {
                if (converted.endSample > result.words.last().endSample)
                    result.words.last().endSample = converted.endSample;
                continue;
            }
            if (converted.startSample < previous.endSample) {
                result.reviewReason = QStringLiteral("对齐字词发生无法消解的重叠");
                result.words.clear();
                return result;
            }
        }
        result.words.append(std::move(converted));
    }
    if (result.words.isEmpty()) {
        result.reviewReason = QStringLiteral("没有可用的字词对齐结果");
        return result;
    }
    result.trustworthy = true;
    return result;
}

QVector<RoughCutSilenceEvidence> RoughCutAlignmentEvidence::detectSilence(
    const QVector<float> &monoPcm, int pcmSampleRate, int sourceSampleRate,
    qint64 sourceStartSample, double peakThreshold, int minimumSilenceMs)
{
    QVector<RoughCutSilenceEvidence> result;
    if (monoPcm.isEmpty() || pcmSampleRate <= 0 || sourceSampleRate <= 0
        || peakThreshold < 0.0 || minimumSilenceMs < 0) return result;
    const qint64 minimumSamples = std::max<qint64>(1,
        (static_cast<qint64>(minimumSilenceMs) * pcmSampleRate + 999) / 1000);
    qint64 silenceStart = -1;
    double peak = 0.0;
    for (qint64 index = 0; index <= monoPcm.size(); ++index) {
        const bool silent = index < monoPcm.size()
            && std::abs(static_cast<double>(monoPcm.at(index))) <= peakThreshold;
        if (silent) {
            if (silenceStart < 0) silenceStart = index;
            peak = std::max(peak, std::abs(static_cast<double>(monoPcm.at(index))));
        } else if (silenceStart >= 0) {
            if (index - silenceStart >= minimumSamples) {
                result.append({pcmToSourceSample(silenceStart, pcmSampleRate, sourceSampleRate,
                                   sourceStartSample),
                    pcmToSourceSample(index, pcmSampleRate, sourceSampleRate, sourceStartSample), peak});
            }
            silenceStart = -1;
            peak = 0.0;
        }
    }
    return result;
}

} // namespace subcue
