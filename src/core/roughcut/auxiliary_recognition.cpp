#include "roughcut/auxiliary_recognition.h"

#include "alignment/normalizer.h"

#include <algorithm>

namespace subcue {

QVector<RoughCutSuspiciousRange> RoughCutAuxiliaryRecognition::plan(
    const QVector<RecognizedPassage> &recording,
    const QVector<RoughCutSegmentDecision> &decisions,
    int sampleRate, qint64 sourceSampleCount, int paddingMs, int mergeGapMs)
{
    QVector<RoughCutSuspiciousRange> result;
    if (recording.size() != decisions.size() || sampleRate <= 0 || sourceSampleCount <= 0) return result;
    const qint64 padding = static_cast<qint64>(std::max(0, paddingMs)) * sampleRate / 1000;
    const qint64 mergeGap = static_cast<qint64>(std::max(0, mergeGapMs)) * sampleRate / 1000;
    for (int index = 0; index < recording.size(); ++index) {
        if (decisions.at(index).effectiveDecision() != RoughCutDecision::Review) continue;
        const qint64 start = std::max<qint64>(0, recording.at(index).startSample - padding);
        const qint64 end = std::min(sourceSampleCount, recording.at(index).endSample + padding);
        if (end <= start) continue;
        if (!result.isEmpty() && start <= result.constLast().endSample + mergeGap) {
            result.last().endSample = std::max(result.constLast().endSample, end);
            result.last().recordingIndexes.append(index);
        } else {
            result.append({start, end, {index}});
        }
    }
    return result;
}

RoughCutDecision RoughCutAuxiliaryRecognition::reconcile(
    RoughCutDecision current, RoughCutAuxiliaryResult *result)
{
    if (!result) return RoughCutDecision::Review;
    const QString primary = Normalizer::normalizeText(result->primaryText);
    const QString funAsr = Normalizer::normalizeText(result->funAsrText);
    result->conflict = result->funAsrFailed || primary.isEmpty()
        || funAsr.isEmpty() || primary != funAsr;
    if (result->conflict) return RoughCutDecision::Review;
    return current == RoughCutDecision::Review ? result->agreementDecision : current;
}

} // namespace subcue
