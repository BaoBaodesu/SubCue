#include "roughcut/timeline_engine.h"

#include <algorithm>

namespace subcue {
namespace {

QPair<int, int> gapRange(RoughCutGapKind kind)
{
    switch (kind) {
    case RoughCutGapKind::WithinSentence: return {100, 180};
    case RoughCutGapKind::Comma: return {180, 280};
    case RoughCutGapKind::Paragraph: return {450, 700};
    case RoughCutGapKind::Topic: return {600, 900};
    case RoughCutGapKind::Sentence: return {300, 450};
    }
    return {300, 450};
}

qint64 millisecondsToSamples(int milliseconds, int sampleRate)
{
    return (static_cast<qint64>(milliseconds) * sampleRate + 500) / 1000;
}

} // namespace

QVector<RoughCutTimelineClip> RoughCutTimelineEngine::build(
    const QVector<RecognizedPassage> &recording,
    const QVector<RoughCutSegmentDecision> &decisions, int sampleRate,
    qint64 sourceSampleCount, const QVector<RoughCutGapKind> &gaps,
    int preRollMs, int postRollMs)
{
    QVector<RoughCutTimelineClip> result;
    if (recording.size() != decisions.size() || sampleRate <= 0 || sourceSampleCount <= 0) return result;
    const qint64 preRoll = millisecondsToSamples(std::max(0, preRollMs), sampleRate);
    const qint64 postRoll = millisecondsToSamples(std::max(0, postRollMs), sampleRate);
    qint64 timelineEnd = 0;
    int previousIncluded = -1;
    for (int index = 0; index < recording.size(); ++index) {
        const RoughCutDecision effective = decisions.at(index).effectiveDecision();
        if (effective == RoughCutDecision::Cut) continue;
        qint64 start = std::max<qint64>(0, recording.at(index).startSample - preRoll);
        qint64 end = std::min(sourceSampleCount, recording.at(index).endSample + postRoll);
        if (index > 0) start = std::max(start, recording.at(index - 1).endSample);
        if (index + 1 < recording.size()) end = std::min(end, recording.at(index + 1).startSample);
        if (end <= start) continue;
        if (previousIncluded >= 0) {
            const qint64 originalGap = std::max<qint64>(0,
                recording.at(index).startSample - recording.at(previousIncluded).endSample);
            const RoughCutGapKind kind = previousIncluded < gaps.size()
                ? gaps.at(previousIncluded) : RoughCutGapKind::Sentence;
            const auto range = gapRange(kind);
            timelineEnd += std::clamp(originalGap,
                millisecondsToSamples(range.first, sampleRate),
                millisecondsToSamples(range.second, sampleRate));
        }
        result.append({start, end, timelineEnd, effective, -1, -1, recording.at(index).text});
        timelineEnd += end - start;
        previousIncluded = index;
    }
    return result;
}

} // namespace subcue
