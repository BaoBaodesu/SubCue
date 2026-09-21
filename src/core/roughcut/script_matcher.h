#pragma once

#include "roughcut/script_document.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <algorithm>
#include <atomic>

namespace subcue {

enum class ScriptMatchStatus { Match, Modified, Skipped, Retake, Added };

struct RecognizedPassage final {
    QString id;
    QString text;
    qint64 startSample = 0;
    qint64 endSample = 0;
    qint64 silenceBeforeSamples = 0;
    qint64 silenceAfterSamples = 0;
    double vadConfidence = 0.0;
    bool audioComplete = true;
    bool boundaryTrustworthy = false;
    int scriptLineIndex = -1;
    int takeGroupId = -1;
    int scriptLineEndIndex = -1;
    double textSimilarity = 0.0;
    double editSimilarity = 0.0;
    double continuousCoverage = 0.0;
    int scriptTokenStart = -1;
    int scriptTokenEnd = -1;
    bool preciseTiming = false;

    [[nodiscard]] qint64 durationSamples() const noexcept
    {
        return std::max<qint64>(0, endSample - startSample);
    }
};

struct ScriptMatch final {
    int recordingIndex = -1;
    int scriptLineIndex = -1;
    ScriptMatchStatus status = ScriptMatchStatus::Added;
    double similarity = 0.0;
    double editSimilarity = 0.0;
    double continuousCoverage = 0.0;
    int scriptLineEndIndex = -1;
    int scriptTokenStart = -1;
    int scriptTokenEnd = -1;
};

class ScriptMatcher final {
public:
    [[nodiscard]] static QVector<ScriptMatch> match(
        const ScriptDocument &script,
        const QVector<RecognizedPassage> &recording,
        const std::atomic<bool> *cancel = nullptr,
        bool *cancelled = nullptr);
};

} // namespace subcue
