#pragma once

#include "ai/ai_review_types.h"
#include "alignment/transcript.h"
#include "roughcut/alignment_evidence.h"
#include "roughcut/script_matcher.h"
#include "roughcut/timeline_engine.h"

namespace subcue {

struct SafeCutRange final {
    qint64 startSample = 0;
    qint64 endSample = 0;
};

class SafeCutBoundary final {
public:
    [[nodiscard]] static qint64 resolveKeepTail(
        qint64 wordEndSample,
        const QVector<RoughCutSilenceEvidence> &silences,
        int sampleRate,
        qint64 sourceSampleCount,
        const OmniReviewSettings &settings);

    [[nodiscard]] static SafeCutRange resolve(
        SafeCutRange proposedCut,
        SafeCutRange keepRange,
        const QVector<RoughCutSilenceEvidence> &silences,
        int sampleRate,
        qint64 sourceSampleCount,
        const OmniReviewSettings &settings);

    static void applyToPassages(
        QVector<RecognizedPassage> *recording,
        QVector<RoughCutSegmentDecision> *decisions,
        const QVector<TranscriptWord> &words,
        int sampleRate,
        qint64 sourceSampleCount,
        const OmniReviewSettings &settings);

    [[nodiscard]] static QVector<RoughCutTimelineClip> buildTimeline(
        const QVector<RecognizedPassage> &recording,
        const QVector<RoughCutSegmentDecision> &decisions,
        int sampleRate,
        qint64 sourceSampleCount,
        const OmniReviewSettings &settings);
};

} // namespace subcue
