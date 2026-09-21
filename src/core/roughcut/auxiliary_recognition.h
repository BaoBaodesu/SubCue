#pragma once

#include "roughcut/decision_engine.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

struct RoughCutSuspiciousRange final {
    qint64 startSample = 0;
    qint64 endSample = 0;
    QVector<int> recordingIndexes;
};

struct RoughCutAuxiliaryResult final {
    int recordingIndex = -1;
    QString primaryText;
    QString funAsrText;
    QString whisperText;
    bool funAsrFailed = false;
    bool whisperFailed = false;
    bool conflict = false;
    RoughCutDecision agreementDecision = RoughCutDecision::Review;
};

class RoughCutAuxiliaryRecognition final {
public:
    [[nodiscard]] static QVector<RoughCutSuspiciousRange> plan(
        const QVector<RecognizedPassage> &recording,
        const QVector<RoughCutSegmentDecision> &decisions,
        int sampleRate, qint64 sourceSampleCount, int paddingMs = 300,
        int mergeGapMs = 150);
    [[nodiscard]] static RoughCutDecision reconcile(
        RoughCutDecision current, RoughCutAuxiliaryResult *result);
};

} // namespace subcue
