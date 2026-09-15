#pragma once

#include "roughcut/script_document.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

enum class ScriptMatchStatus { Match, Modified, Skipped, Retake, Added };

struct RecognizedPassage final {
    QString id;
    QString text;
    qint64 startSample = 0;
    qint64 endSample = 0;
};

struct ScriptMatch final {
    int recordingIndex = -1;
    int scriptLineIndex = -1;
    ScriptMatchStatus status = ScriptMatchStatus::Added;
    double similarity = 0.0;
};

class ScriptMatcher final {
public:
    [[nodiscard]] static QVector<ScriptMatch> match(
        const ScriptDocument &script,
        const QVector<RecognizedPassage> &recording);
};

} // namespace subcue
