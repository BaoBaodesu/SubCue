#pragma once

#include "roughcut/script_matcher.h"

#include <QtCore/QStringList>

namespace subcue {

enum class RoughCutFailureType {
    None,
    Retake,
    Interrupted,
    Duplicate,
    WrongTake,
    Filler
};

[[nodiscard]] QString roughCutFailureName(RoughCutFailureType value);

struct RoughCutTake final {
    int recordingIndex = -1;
    double score = 0.0;
    QStringList reasons;
    bool complete = false;
    bool interruption = false;
    bool restartMarker = false;
    RoughCutFailureType failureType = RoughCutFailureType::None;
    qint64 startSample = 0;
    qint64 endSample = 0;
    int scriptTokenStart = -1;
    int scriptTokenEnd = -1;
    double scriptCoverage = 0.0;
    bool boundaryTrustworthy = false;
    QString recognizedText;
};

struct RoughCutRetakeGroup final {
    QVector<RoughCutTake> takes;
    int recommendedRecordingIndex = -1;
    bool needsReview = true;
    QString reason;
    int id = -1;
    int scriptLineIndex = -1;
    double confidence = 0.0;
};

class RetakeDetector final {
public:
    [[nodiscard]] static QVector<RoughCutRetakeGroup> detect(
        const QVector<RecognizedPassage> &recording,
        const QVector<ScriptMatch> &matches,
        int sampleRate,
        int neighbourSeconds = 90);
};

} // namespace subcue
