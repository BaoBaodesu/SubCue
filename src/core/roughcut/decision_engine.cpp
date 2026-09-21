#include "roughcut/decision_engine.h"

#include "alignment/normalizer.h"

#include <algorithm>
#include <string>

namespace subcue {
namespace {

bool filler(const QString &text)
{
    const QString normalized = Normalizer::normalizeText(text);
    static const QStringList values{QStringLiteral("嗯"), QStringLiteral("啊"),
        QStringLiteral("呃"), QStringLiteral("额"), QStringLiteral("唉"),
        QStringLiteral("嗯嗯"), QStringLiteral("啊啊"), QStringLiteral("呃呃")};
    return values.contains(normalized);
}

bool restartCue(const QString &text)
{
    const QString normalized = Normalizer::normalizeText(text);
    if (normalized.size() > 20) return false;
    static const QStringList markers{QStringLiteral("重来"), QStringLiteral("再来"),
        QStringLiteral("说错了"), QStringLiteral("重新说"), QStringLiteral("不对")};
    return std::any_of(markers.cbegin(), markers.cend(), [&normalized](const QString &marker) {
        return normalized.contains(marker);
    });
}

bool containsInOrder(const std::u32string &spoken, const std::u32string &script)
{
    size_t matched = 0;
    for (char32_t token : spoken) {
        if (matched < script.size() && token == script.at(matched)) ++matched;
    }
    return matched == script.size();
}

bool containsNearby(const QString &text, const QString &fragment)
{
    if (text.contains(fragment)) return true;
    if (fragment.size() != 2) return false;
    for (qsizetype index = 0; index + 2 < text.size(); ++index) {
        if (text.at(index) == fragment.at(0) && text.at(index + 2) == fragment.at(1))
            return true;
    }
    return false;
}

QString failureReason(RoughCutFailureType type)
{
    switch (type) {
    case RoughCutFailureType::Retake: return QStringLiteral("检测到重录提示且已有完整替代版本");
    case RoughCutFailureType::Interrupted: return QStringLiteral("半句中断且后续已有完整版本");
    case RoughCutFailureType::Duplicate: return QStringLiteral("重复句子中已有更优版本");
    case RoughCutFailureType::WrongTake: return QStringLiteral("错误 Take 后已有可信正确版本");
    case RoughCutFailureType::Filler: return QStringLiteral("文案外短促无效发声");
    case RoughCutFailureType::None: return QStringLiteral("失败 Take 已有可信完整替代版本");
    }
    return QStringLiteral("失败 Take 已有可信完整替代版本");
}

} // namespace

QVector<RoughCutSegmentDecision> RoughCutDecisionEngine::decide(
    const QVector<RecognizedPassage> &recording, const QVector<ScriptMatch> &matches,
    const QVector<RoughCutRetakeGroup> &groups, const QVector<bool> &alignmentTrustworthy,
    const QString &scriptText)
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
            decision.failureType = take.failureType;
            decision.replacementRecordingIndex = group.recommendedRecordingIndex;
            const bool trusted = alignmentTrustworthy.isEmpty()
                || (take.recordingIndex < alignmentTrustworthy.size()
                    && alignmentTrustworthy.at(take.recordingIndex));
            const bool failureEvidence = take.failureType != RoughCutFailureType::None
                || byRecording.at(take.recordingIndex).status == ScriptMatchStatus::Retake
                || take.reasons.contains(QStringLiteral("疑似中断"));
            if (!trusted) {
                decision.reason = QStringLiteral("对齐边界不可信，保留复核");
            } else if (!failureEvidence) {
                decision.reason = QStringLiteral("没有独立失败证据，保留复核");
            } else {
                decision.autoDecision = RoughCutDecision::Cut;
                decision.ruleScore = 90.0;
                decision.reason = failureReason(take.failureType);
                decision.evidence = take.reasons;
                decision.evidence.append(group.reason);
            }
        }
    }
    for (int index = 0; index < result.size(); ++index) {
        if (grouped.at(index)) continue;
        const bool trusted = alignmentTrustworthy.isEmpty()
            || (index < alignmentTrustworthy.size() && alignmentTrustworthy.at(index));
        if (trusted && byRecording.at(index).scriptLineIndex < 0
            && filler(recording.at(index).text)) {
            result[index].autoDecision = RoughCutDecision::Cut;
            result[index].ruleScore = 96.0;
            result[index].failureType = RoughCutFailureType::Filler;
            result[index].reason = failureReason(RoughCutFailureType::Filler);
            result[index].evidence = {QStringLiteral("未匹配文案"),
                QStringLiteral("短促填充发声：%1").arg(recording.at(index).text)};
        } else if (trusted && byRecording.at(index).scriptLineIndex < 0
            && restartCue(recording.at(index).text) && index + 1 < recording.size()
            && byRecording.at(index + 1).scriptLineIndex >= 0) {
            result[index].autoDecision = RoughCutDecision::Cut;
            result[index].ruleScore = 92.0;
            result[index].failureType = RoughCutFailureType::Retake;
            result[index].replacementRecordingIndex = index + 1;
            result[index].reason = QStringLiteral("文案外重录提示且后续存在正确版本");
            result[index].evidence = {QStringLiteral("重录提示：%1").arg(recording.at(index).text)};
        } else if (trusted && byRecording.at(index).scriptLineIndex >= 0
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
    protectCuts(recording, &result, scriptText);
    return result;
}

void RoughCutDecisionEngine::protectCuts(
    const QVector<RecognizedPassage> &recording,
    QVector<RoughCutSegmentDecision> *decisions,
    const QString &scriptText)
{
    if (!decisions || decisions->size() != recording.size()) return;
    const QString script = Normalizer::normalizeText(scriptText);
    const std::u32string scriptTokens = Normalizer::normalizeCodepoints(scriptText);
    for (int index = 0; index < recording.size(); ++index) {
        RoughCutSegmentDecision &decision = (*decisions)[index];
        if (decision.autoDecision != RoughCutDecision::Cut) continue;
        const RecognizedPassage &passage = recording.at(index);
        const QString spoken = Normalizer::normalizeText(passage.text);
        if (decision.failureType == RoughCutFailureType::Filler
            && passage.scriptTokenStart < 0 && passage.boundaryTrustworthy
            && (script.isEmpty() || !script.contains(spoken))) continue;
        if (decision.failureType == RoughCutFailureType::Retake
            && passage.scriptTokenStart < 0 && passage.preciseTiming
            && passage.boundaryTrustworthy && !script.isEmpty() && restartCue(passage.text)
            && decision.replacementRecordingIndex >= 0
            && decision.replacementRecordingIndex < recording.size()
            && decisions->at(decision.replacementRecordingIndex).effectiveDecision() == RoughCutDecision::Keep
            && recording.at(decision.replacementRecordingIndex).boundaryTrustworthy
            && recording.at(decision.replacementRecordingIndex).preciseTiming) {
            const QString replacementText = Normalizer::normalizeText(
                recording.at(decision.replacementRecordingIndex).text);
            bool scriptCovered = true;
            const int width = spoken.size() > 1 ? 2 : 1;
            for (int offset = 0; offset + width <= spoken.size(); ++offset) {
                const QString fragment = spoken.mid(offset, width);
                if (script.contains(fragment) && !replacementText.contains(fragment)) {
                    scriptCovered = false;
                    break;
                }
            }
            if (scriptCovered) continue;
        }
        bool covered = passage.boundaryTrustworthy && passage.preciseTiming
            && passage.scriptTokenStart >= 0
            && passage.scriptTokenEnd > passage.scriptTokenStart;
        const RecognizedPassage *replacement = nullptr;
        if (covered) {
            for (int other = 0; other < recording.size(); ++other) {
                if (other == index || decisions->at(other).effectiveDecision() != RoughCutDecision::Keep)
                    continue;
                const RecognizedPassage &candidate = recording.at(other);
                if (!candidate.boundaryTrustworthy || !candidate.preciseTiming
                    || candidate.scriptTokenStart > passage.scriptTokenStart
                    || candidate.scriptTokenEnd < passage.scriptTokenEnd) continue;
                replacement = &candidate;
                break;
            }
            covered = replacement != nullptr;
        }
        if (covered && !script.isEmpty()) {
            const std::u32string replacementTokens = Normalizer::normalizeCodepoints(replacement->text);
            if (passage.scriptTokenEnd > static_cast<int>(scriptTokens.size())
                || !containsInOrder(replacementTokens,
                    scriptTokens.substr(passage.scriptTokenStart,
                        passage.scriptTokenEnd - passage.scriptTokenStart)))
                covered = false;
            const QString replacementText = Normalizer::normalizeText(replacement->text);
            const int width = spoken.size() > 1 ? 2 : 1;
            for (int offset = 0; offset + width <= spoken.size(); ++offset) {
                const QString fragment = spoken.mid(offset, width);
                if (script.contains(fragment) && !containsNearby(replacementText, fragment)) {
                    covered = false;
                    break;
                }
            }
        }
        if (covered) continue;
        decision.autoDecision = RoughCutDecision::Review;
        decision.bestTake = false;
        decision.replacementRecordingIndex = -1;
        decision.reason = QStringLiteral("缺少覆盖全部有效文案的可信保留版本，禁止自动剪除");
        decision.evidence.append(QStringLiteral("文案独有内容或字词时间边界尚未验证"));
    }
}

} // namespace subcue
