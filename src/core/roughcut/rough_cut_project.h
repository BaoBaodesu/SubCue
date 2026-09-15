#pragma once

#include "roughcut/decision_engine.h"
#include "roughcut/auxiliary_recognition.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace subcue {

struct RoughCutProject final {
    int schemaVersion = 1;
    int analysisVersion = 1;
    QString mediaPath;
    QByteArray mediaSha256;
    QString scriptPath;
    QString scriptText;
    int sampleRate = 0;
    int channels = 0;
    qint64 sourceSampleCount = 0;
    QVector<RecognizedPassage> recording;
    QVector<RoughCutSegmentDecision> decisions;
    QVector<RoughCutAuxiliaryResult> auxiliaryResults;
};

class RoughCutProjectSerializer final {
public:
    [[nodiscard]] static QByteArray mediaSha256(const QString &path,
                                                QString *errorMessage = nullptr);
    [[nodiscard]] static bool save(const QString &path, const RoughCutProject &project,
                                   QString *errorMessage = nullptr);
    [[nodiscard]] static std::optional<RoughCutProject> load(
        const QString &path, QString *errorMessage = nullptr);
};

} // namespace subcue
