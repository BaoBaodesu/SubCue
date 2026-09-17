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
    QVector<ScriptMatch> byRecording(recording.size());
    for (int index = 0; index < byRecording.size(); ++index) byRecording[index].recordingIndex = index;
    for (const ScriptMatch &match : matches) {
        if (match.recordingIndex >= 0 && match.recordingIndex < byRecording.size())
            byRecording[match.recordingIndex] = match;
    }
    QVector<bool> grouped(recording.size(), false);
    for (const RoughCutRetakeGroup &group : groups) {
        for (const RoughCutTake &take : group.takes) {
            if (take.recordingIndex >= 0 && take.recordingIndex < result.size()) {
                grouped[take.recordingIndex] = true;
                result[take.recordingIndex].takeGroupId = group.id;
            }
        }
        if (group.needsReview || group.recommendedRecordingIndex < 0) continue;
        RoughCutSegmentDecision &recommended = result[group.recommendedRecordingIndex];
        recommended.autoDecision = RoughCutDecision::Keep;
        recommended.ruleScore = 100.0;
        recommended.reason = QStringLiteral("重录组中存在明确可信的完整版本");
        recommended.evidence = {group.reason};
        recommended.bestTake = true;
        for (const RoughCutTake &take : group.takes) {
            if (take.recordingIndex == group.recommendedRecordingIndex) continue;
            RoughCutSegmentDecision &decision = result[take.recordingIndex];
            const bool trusted = alignmentTrustworthy.isEmpty()
                || (take.recordingIndex < alignmentTrustworthy.size()
                    && alignmentTrustworthy.at(take.recordingIndex));
            const bool failureEvidence = byRecording.at(take.recordingIndex).status == ScriptMatchStatus::Retake
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
    for (int index = 0; index < result.size(); ++index) {
        if (grouped.at(index)) continue;
        const bool trusted = alignmentTrustworthy.isEmpty()
            || (index < alignmentTrustworthy.size() && alignmentTrustworthy.at(index));
        if (trusted && byRecording.at(index).scriptLineIndex >= 0
            && byRecording.at(index).similarity >= 72.0
            && recording.at(index).audioComplete) {
            result[index].autoDecision = RoughCutDecision::Keep;
            result[index].ruleScore = byRecording.at(index).similarity;
            result[index].reason = QStringLiteral("文案匹配可靠且语音边界可信");
            result[index].evidence = {
                QStringLiteral("综合相似度 %1").arg(byRecording.at(index).similarity, 0, 'f', 0),
                QStringLiteral("连续覆盖率 %1").arg(byRecording.at(index).continuousCoverage, 0, 'f', 0)};
        }
    }
    return result;
}

} // namespace subcue
