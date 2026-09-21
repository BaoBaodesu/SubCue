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
    QString pendingZeroDuration;
    for (const TranscriptWord &word : transcript.words) {
        if (word.text.trimmed().isEmpty() || word.startMs < 0 || word.endMs < word.startMs) {
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
        if (converted.endSample == converted.startSample) {
            pendingZeroDuration += converted.text;
            continue;
        }
        if (!pendingZeroDuration.isEmpty()) {
            converted.text.prepend(pendingZeroDuration);
            converted.timingTrustworthy = false;
            pendingZeroDuration.clear();
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
                result.words.last().timingTrustworthy = false;
                converted.timingTrustworthy = false;
                result.reviewReason = QStringLiteral("部分字词时间重叠，相关片段需要复核");
            }
        }
        result.words.append(std::move(converted));
    }
    if (!pendingZeroDuration.isEmpty() && !result.words.isEmpty()) {
        result.words.last().text += pendingZeroDuration;
        result.words.last().timingTrustworthy = false;
        result.reviewReason = QStringLiteral("部分字词没有独立时间，相关片段需要复核");
    }
    if (result.words.isEmpty()) {
        result.reviewReason = QStringLiteral("没有可用的字词对齐结果");
        return result;
    }
    result.trustworthy = true;
    return result;
}

QVector<RecognizedPassage> RoughCutAlignmentEvidence::buildPassages(
    QVector<RecognizedPassage> speech, const Transcript &transcript,
    int sourceSampleRate, qint64 sourceSampleCount)
{
    const bool precise = std::all_of(transcript.words.cbegin(), transcript.words.cend(),
        [](const TranscriptWord &word) { return word.preciseTiming; });
    const RoughCutAlignmentResult aligned = validate(
        transcript, sourceSampleRate, sourceSampleCount, precise);
    if (aligned.trustworthy) {
        const QVector<RoughCutAlignedWord> &words = aligned.words;
        QVector<RecognizedPassage> passages;
        const QStringList markers{QStringLiteral("重来"), QStringLiteral("再来"),
            QStringLiteral("说错了"), QStringLiteral("重新说"), QStringLiteral("不对")};
        const QStringList fillers{QStringLiteral("嗯"), QStringLiteral("啊"),
            QStringLiteral("呃"), QStringLiteral("额")};
        int first = 0;
        bool startBoundaryTrustworthy = true;
        for (int index = 0; index <= words.size(); ++index) {
            const bool gap = index < words.size() && index > first
                && words.at(index).startSample - words.at(index - 1).endSample
                    >= static_cast<qint64>(sourceSampleRate) * 450 / 1000;
            QString preceding;
            for (int word = first; word < index; ++word) preceding += words.at(word).text;
            const QString normalizedPreceding = Normalizer::normalizeText(preceding);
            const bool restart = index < words.size() && index > first
                && std::any_of(markers.cbegin(), markers.cend(), [&normalizedPreceding](const QString &marker) {
                    return normalizedPreceding.endsWith(marker);
                });
            QString following;
            if (index < words.size() && normalizedPreceding.size() >= 4
                && words.at(index).startSample - words.at(index - 1).endSample
                    >= static_cast<qint64>(sourceSampleRate) * 120 / 1000) {
                for (int word = index; word < words.size() && following.size() < 5; ++word)
                    following += words.at(word).text;
            }
            const bool repeatedStart = normalizedPreceding.size() >= 4
                && Normalizer::normalizeText(following).startsWith(normalizedPreceding.left(3));
            const bool fillerBoundary = index < words.size() && index > first
                && words.at(index).startSample - words.at(index - 1).endSample
                    >= static_cast<qint64>(sourceSampleRate) * 120 / 1000
                && (fillers.contains(Normalizer::normalizeText(words.at(index).text))
                    || fillers.contains(Normalizer::normalizeText(words.at(index - 1).text)));
            if (index < words.size() && !gap && !restart && !repeatedStart && !fillerBoundary)
                continue;
            if (index > first) {
                RecognizedPassage passage;
                passage.id = QStringLiteral("speech-%1").arg(passages.size() + 1);
                passage.startSample = std::max<qint64>(0,
                    words.at(first).startSample - static_cast<qint64>(sourceSampleRate) * 40 / 1000);
                passage.endSample = std::min(sourceSampleCount,
                    words.at(index - 1).endSample + static_cast<qint64>(sourceSampleRate) * 40 / 1000);
                for (int word = first; word < index; ++word) passage.text += words.at(word).text;
                passage.text = passage.text.trimmed();
                passage.preciseTiming = true;
                const bool endBoundaryTrustworthy = index >= words.size()
                    || words.at(index).startSample - words.at(index - 1).endSample
                        >= static_cast<qint64>(sourceSampleRate) * 80 / 1000;
                passage.boundaryTrustworthy = !passage.text.isEmpty()
                    && passage.endSample > passage.startSample
                    && startBoundaryTrustworthy && endBoundaryTrustworthy;
                passage.boundaryTrustworthy &= words.at(first).timingTrustworthy
                    && words.at(index - 1).timingTrustworthy;
                for (const RecognizedPassage &region : speech) {
                    if (region.endSample <= passage.startSample || region.startSample >= passage.endSample) continue;
                    passage.vadConfidence = std::max(passage.vadConfidence, region.vadConfidence);
                }
                if (!passages.isEmpty() && passage.startSample < passages.constLast().endSample) {
                    const qint64 boundary = (passage.startSample + passages.constLast().endSample) / 2;
                    passages.last().endSample = boundary;
                    passage.startSample = boundary;
                }
                passages.append(std::move(passage));
                startBoundaryTrustworthy = endBoundaryTrustworthy;
            }
            first = index;
        }
        for (int index = 0; index < passages.size(); ++index) {
            passages[index].silenceBeforeSamples = index == 0 ? passages.at(index).startSample
                : std::max<qint64>(0, passages.at(index).startSample - passages.at(index - 1).endSample);
            passages[index].silenceAfterSamples = index + 1 < passages.size()
                ? std::max<qint64>(0, passages.at(index + 1).startSample - passages.at(index).endSample) : 0;
        }
        return passages;
    }
    for (RecognizedPassage &segment : speech) {
        for (const TranscriptWord &word : transcript.words) {
            if (word.text.trimmed().isEmpty() || word.endMs <= word.startMs) continue;
            const qint64 midpoint = (word.startMs + word.endMs) * sourceSampleRate / 2000;
            if (midpoint >= segment.startSample && midpoint < segment.endSample)
                segment.text += word.text;
        }
        segment.text = segment.text.trimmed();
        segment.boundaryTrustworthy = false;
    }
    return speech;
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
