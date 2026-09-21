#pragma once

#include "roughcut/decision_engine.h"

namespace subcue {

enum class RoughCutGapKind { WithinSentence, Comma, Sentence, Paragraph, Topic };

struct RoughCutTimelineClip final {
    qint64 sourceStartSample = 0;
    qint64 sourceEndSample = 0;
    qint64 timelineStartSample = 0;
    RoughCutDecision decision = RoughCutDecision::Review;
    int scriptLineId = -1;
    int retakeGroupId = -1;
    int recordingIndex = -1;
    QString text;
};

class RoughCutTimelineEngine final {
public:
    [[nodiscard]] static QVector<RoughCutTimelineClip> build(
        const QVector<RecognizedPassage> &recording,
        const QVector<RoughCutSegmentDecision> &decisions,
        int sampleRate, qint64 sourceSampleCount,
        const QVector<RoughCutGapKind> &gaps = {},
        int preRollMs = 80, int postRollMs = 250);
};

} // namespace subcue
