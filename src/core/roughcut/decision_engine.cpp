#include "roughcut/decision_engine.h"

namespace subcue {

QVector<RoughCutSegmentDecision> RoughCutDecisionEngine::decide(
    const QVector<RecognizedPassage> &recording, const QVector<ScriptMatch> &matches,
    const QVector<RoughCutRetakeGroup> &groups, const QVector<bool> &alignmentTrustworthy)
{
    QVector<RoughCutSegmentDecision> result;
    result.reserve(recording.size());
    for (int index = 0; index < recording.size(); ++index) {
        result.append({index, RoughCutDecision::Review, std::nullopt, 0.0,
            QStringLiteral("缺少足够的自动裁切证据"), {}});
    }
    if (matches.size() != recording.size()) return result;
    for (const RoughCutRetakeGroup &group : groups) {
        if (group.needsReview || group.recommendedRecordingIndex < 0) continue;
        RoughCutSegmentDecision &recommended = result[group.recommendedRecordingIndex];
        recommended.autoDecision = RoughCutDecision::Keep;
        recommended.ruleScore = 100.0;
        recommended.reason = QStringLiteral("重录组中存在明确可信的完整版本");
        recommended.evidence = {group.reason};
        for (const RoughCutTake &take : group.takes) {
            if (take.recordingIndex == group.recommendedRecordingIndex) continue;
            RoughCutSegmentDecision &decision = result[take.recordingIndex];
            const bool trusted = alignmentTrustworthy.isEmpty()
                || (take.recordingIndex < alignmentTrustworthy.size()
                    && alignmentTrustworthy.at(take.recordingIndex));
            const bool failureEvidence = matches.at(take.recordingIndex).status == ScriptMatchStatus::Retake
                || take.reasons.contains(QStringLiteral("疑似中断"));
            if (!trusted) {
                decision.reason = QStringLiteral("对齐边界不可信，保留复核");
            } else if (!failureEvidence) {
                decision.reason = QStringLiteral("没有独立失败证据，保留复核");
            } else {
                decision.autoDecision = RoughCutDecision::Cut;
                decision.ruleScore = 90.0;
                decision.reason = QStringLiteral("失败 Take 已有可信完整替代版本");
                decision.evidence = take.reasons;
                decision.evidence.append(group.reason);
            }
        }
    }
    return result;
}

} // namespace subcue
