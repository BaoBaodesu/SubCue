#include "roughcut/safe_cut_boundary.h"

#include <algorithm>

namespace subcue {
namespace {

qint64 msToSamples(int milliseconds, int sampleRate)
{
    return (static_cast<qint64>(milliseconds) * sampleRate + 500) / 1000;
}

qint64 lastWordEnd(const QVector<TranscriptWord> &words, qint64 startMs, qint64 endMs, int sampleRate)
{
    qint64 last = -1;
    for (const TranscriptWord &word : words) {
        if (word.endMs < startMs || word.startMs > endMs) continue;
        if (word.text.trimmed().isEmpty()) continue;
        last = std::max(last, word.endMs * sampleRate / 1000);
    }
    return last;
}

} // namespace

qint64 SafeCutBoundary::resolveKeepTail(
    qint64 wordEndSample,
    const QVector<RoughCutSilenceEvidence> &silences,
    int sampleRate,
    qint64 sourceSampleCount,
    const OmniReviewSettings &settings)
{
    if (sampleRate <= 0 || sourceSampleCount <= 0) return std::max<qint64>(0, wordEndSample);
    const qint64 minTail = msToSamples(settings.minTailMs, sampleRate);
    const qint64 preferredTail = msToSamples(settings.preferredTailMs, sampleRate);
    const qint64 maxTail = msToSamples(settings.maxTailMs, sampleRate);
    const qint64 minEnd = std::min(sourceSampleCount, std::max<qint64>(0, wordEndSample) + minTail);
    const qint64 maxEnd = std::min(sourceSampleCount, std::max<qint64>(0, wordEndSample) + maxTail);
    qint64 chosen = std::min(sourceSampleCount, std::max<qint64>(0, wordEndSample) + preferredTail);
    chosen = std::max(chosen, minEnd);
    for (const RoughCutSilenceEvidence &silence : silences) {
        if (silence.endSample <= minEnd || silence.startSample >= maxEnd) continue;
        const qint64 cut = std::clamp((silence.startSample + silence.endSample) / 2, minEnd, maxEnd);
        if (cut > wordEndSample) {
            chosen = cut;
            break;
        }
    }
    if (chosen <= wordEndSample) chosen = minEnd;
    return std::min(sourceSampleCount, chosen);
}

SafeCutRange SafeCutBoundary::resolve(
    SafeCutRange proposedCut,
    SafeCutRange keepRange,
    const QVector<RoughCutSilenceEvidence> &silences,
    int sampleRate,
    qint64 sourceSampleCount,
    const OmniReviewSettings &settings)
{
    SafeCutRange result = proposedCut;
    if (result.endSample < result.startSample) std::swap(result.startSample, result.endSample);
    const qint64 keepTail = resolveKeepTail(
        keepRange.endSample, silences, sampleRate, sourceSampleCount, settings);
    // KEEP 优先：CUT 不得吃掉保留片段的尾音保护。
    if (result.startSample < keepTail && result.endSample > keepRange.startSample) {
        result.startSample = keepTail;
    }
    if (result.endSample > keepRange.startSample && result.startSample < keepRange.startSample) {
        result.endSample = keepRange.startSample;
    }
    if (result.endSample <= result.startSample) {
        result.startSample = 0;
        result.endSample = 0;
    }
    return result;
}

void SafeCutBoundary::applyToPassages(
    QVector<RecognizedPassage> *recording,
    QVector<RoughCutSegmentDecision> *decisions,
    const QVector<TranscriptWord> &words,
    int sampleRate,
    qint64 sourceSampleCount,
    const OmniReviewSettings &settings)
{
    if (!recording || !decisions || recording->size() != decisions->size() || sampleRate <= 0) return;
    QVector<qint64> keepTails(recording->size(), 0);
    for (int index = 0; index < recording->size(); ++index) {
        RecognizedPassage &passage = (*recording)[index];
        const qint64 startMs = sampleRate > 0 ? passage.startSample * 1000 / sampleRate : 0;
        const qint64 endMs = sampleRate > 0 ? passage.endSample * 1000 / sampleRate : 0;
        qint64 wordEnd = lastWordEnd(words, startMs, endMs, sampleRate);
        if (wordEnd < 0) wordEnd = passage.endSample;
        const qint64 silenceStart = passage.endSample;
        const qint64 silenceEnd = passage.endSample + passage.silenceAfterSamples;
        QVector<RoughCutSilenceEvidence> silences;
        if (passage.silenceAfterSamples > 0) {
            silences.append({silenceStart, silenceEnd, 0.0});
        }
        keepTails[index] = resolveKeepTail(wordEnd, silences, sampleRate, sourceSampleCount, settings);
        if ((*decisions)[index].effectiveDecision() != RoughCutDecision::Cut) {
            passage.endSample = std::max(passage.endSample, keepTails[index]);
            if (index + 1 < recording->size()) {
                passage.endSample = std::min(passage.endSample, (*recording)[index + 1].startSample);
            }
            passage.endSample = std::min(passage.endSample, sourceSampleCount);
        }
    }
    for (int index = 0; index < recording->size(); ++index) {
        if ((*decisions)[index].effectiveDecision() != RoughCutDecision::Cut) continue;
        RecognizedPassage &cut = (*recording)[index];
        SafeCutRange proposed{cut.startSample, cut.endSample};
        SafeCutRange keepBefore{0, 0};
        if (index > 0 && (*decisions)[index - 1].effectiveDecision() != RoughCutDecision::Cut) {
            keepBefore = {(*recording)[index - 1].startSample, keepTails[index - 1]};
        }
        QVector<RoughCutSilenceEvidence> silences;
        if (index > 0 && (*recording)[index - 1].silenceAfterSamples > 0) {
            const RecognizedPassage &previous = (*recording)[index - 1];
            silences.append({previous.endSample, previous.endSample + previous.silenceAfterSamples, 0.0});
        }
        const SafeCutRange resolved = resolve(
            proposed, keepBefore, silences, sampleRate, sourceSampleCount, settings);
        if (resolved.endSample <= resolved.startSample) {
            (*decisions)[index].autoDecision = RoughCutDecision::Review;
            (*decisions)[index].reason = QStringLiteral("剪切与保留尾音冲突，已降为复核");
            continue;
        }
        cut.startSample = resolved.startSample;
        cut.endSample = resolved.endSample;
        if (index + 1 < recording->size()
            && (*decisions)[index + 1].effectiveDecision() != RoughCutDecision::Cut) {
            cut.endSample = std::min(cut.endSample, (*recording)[index + 1].startSample);
        }
    }
}

QVector<RoughCutTimelineClip> SafeCutBoundary::buildTimeline(
    const QVector<RecognizedPassage> &recording,
    const QVector<RoughCutSegmentDecision> &decisions,
    int sampleRate,
    qint64 sourceSampleCount,
    const OmniReviewSettings &settings)
{
    const int postRoll = settings.preferredTailMs;
    const int handle = std::min(settings.handleMs, settings.maxTailMs);
    QVector<RoughCutTimelineClip> clips = RoughCutTimelineEngine::build(
        recording, decisions, sampleRate, sourceSampleCount, {}, 80, postRoll);
    if (handle <= 0 || sampleRate <= 0) return clips;
    const qint64 handleSamples = msToSamples(handle, sampleRate);
    for (RoughCutTimelineClip &clip : clips) {
        if (clip.decision == RoughCutDecision::Cut) continue;
        qint64 start = std::max<qint64>(0, clip.sourceStartSample - handleSamples);
        qint64 end = std::min(sourceSampleCount, clip.sourceEndSample + handleSamples);
        // 手柄只能伸进静音，不能把已剪除的原声重新包进时间线。
        for (int index = 0; index < recording.size() && index < decisions.size(); ++index) {
            if (decisions.at(index).effectiveDecision() != RoughCutDecision::Cut) continue;
            const RecognizedPassage &cut = recording.at(index);
            if (cut.endSample <= cut.startSample) continue;
            if (start < cut.endSample && clip.sourceStartSample >= cut.endSample)
                start = std::max(start, cut.endSample);
            if (end > cut.startSample && clip.sourceEndSample <= cut.startSample)
                end = std::min(end, cut.startSample);
        }
        if (end > start) {
            clip.sourceStartSample = start;
            clip.sourceEndSample = end;
        }
    }
    for (int index = 1; index < clips.size(); ++index) {
        if (clips[index].sourceStartSample < clips[index - 1].sourceEndSample) {
            const qint64 mid = (clips[index - 1].sourceEndSample + clips[index].sourceStartSample) / 2;
            clips[index - 1].sourceEndSample = mid;
            clips[index].sourceStartSample = mid;
        }
    }
    // 扩展后的长度才是时间线长度；保留原间隔，重新累计后续位置。
    const auto original = RoughCutTimelineEngine::build(
        recording, decisions, sampleRate, sourceSampleCount, {}, 80, postRoll);
    qint64 endSample = 0;
    for (int index = 0; index < clips.size(); ++index) {
        const qint64 gap = index == 0 ? 0 : std::max<qint64>(0,
            original[index].timelineStartSample - original[index - 1].timelineStartSample
            - (original[index - 1].sourceEndSample - original[index - 1].sourceStartSample));
        clips[index].timelineStartSample = endSample + gap;
        endSample = clips[index].timelineStartSample + clips[index].sourceEndSample - clips[index].sourceStartSample;
    }
    return clips;
}

} // namespace subcue
