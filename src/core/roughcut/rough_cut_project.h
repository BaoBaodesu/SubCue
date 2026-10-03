#pragma once

#include "roughcut/decision_engine.h"
#include "roughcut/auxiliary_recognition.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QJsonObject>

#include <atomic>
#include <functional>
#include <optional>

namespace subcue {

struct RoughCutProject final {
    static constexpr int CurrentSchemaVersion = 6;
    int schemaVersion = CurrentSchemaVersion;
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
    QJsonObject state;
};

class RoughCutProjectSerializer final {
public:
    [[nodiscard]] static QByteArray fingerprint(const RoughCutProject &project);
    [[nodiscard]] static QByteArray mediaSha256(const QString &path,
                                                QString *errorMessage = nullptr,
                                                const std::atomic<bool> *cancel = nullptr,
                                                const std::function<void(qint64, qint64)> &progress = {});
    [[nodiscard]] static bool save(const QString &path, const RoughCutProject &project,
                                   QString *errorMessage = nullptr, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] static std::optional<RoughCutProject> load(
        const QString &path, QString *errorMessage = nullptr);
};

} // namespace subcue
