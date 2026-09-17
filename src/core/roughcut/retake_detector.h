#pragma once

#include "roughcut/script_matcher.h"

#include <QtCore/QStringList>

namespace subcue {

struct RoughCutTake final {
    int recordingIndex = -1;
    double score = 0.0;
    QStringList reasons;
    bool complete = false;
    bool interruption = false;
    bool restartMarker = false;
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
