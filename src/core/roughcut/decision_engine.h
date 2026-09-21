#pragma once

#include "roughcut/retake_detector.h"

#include <QtCore/QStringList>
#include <optional>

namespace subcue {

enum class RoughCutDecision { Keep, Cut, Review };

struct RoughCutSegmentDecision final {
    int recordingIndex = -1;
    RoughCutDecision autoDecision = RoughCutDecision::Review;
    std::optional<RoughCutDecision> userDecision;
    double ruleScore = 0.0;
    QString reason;
    QStringList evidence;
    int takeGroupId = -1;
    int replacementRecordingIndex = -1;
    bool bestTake = false;
    RoughCutFailureType failureType = RoughCutFailureType::None;
    double modelProbability = -1.0;
    QString modelVersion;
    QString decisionSource = QStringLiteral("rule");

    [[nodiscard]] RoughCutDecision effectiveDecision() const
    {
        return userDecision.value_or(autoDecision);
    }
};

class RoughCutDecisionEngine final {
public:
    [[nodiscard]] static QVector<RoughCutSegmentDecision> decide(
        const QVector<RecognizedPassage> &recording,
        const QVector<ScriptMatch> &matches,
        const QVector<RoughCutRetakeGroup> &groups,
        const QVector<bool> &alignmentTrustworthy = {},
        const QString &scriptText = {});
    static void protectCuts(const QVector<RecognizedPassage> &recording,
                            QVector<RoughCutSegmentDecision> *decisions,
                            const QString &scriptText = {});
};

} // namespace subcue
