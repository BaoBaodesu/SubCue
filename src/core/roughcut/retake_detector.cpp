#include "roughcut/retake_detector.h"

#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"

#include <algorithm>

namespace subcue {
namespace {

bool completeSentence(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.endsWith(QLatin1Char('.')) || trimmed.endsWith(QLatin1Char('!'))
        || trimmed.endsWith(QLatin1Char('?')) || trimmed.endsWith(QChar(0x3002))
        || trimmed.endsWith(QChar(0xff01)) || trimmed.endsWith(QChar(0xff1f));
}

bool hasRestartMarker(const QString &text)
{
    const QString normalized = Normalizer::normalizeText(text);
    static const QStringList markers{QStringLiteral("重来"), QStringLiteral("再来"),
        QStringLiteral("说错了"), QStringLiteral("不对"), QStringLiteral("重新说"),
        QStringLiteral("等一下")};
    return std::any_of(markers.cbegin(), markers.cend(), [&](const QString &marker) {
        return normalized.contains(marker);
    });
}

bool prefixRelated(const QString &left, const QString &right)
{
    const QString a = Normalizer::normalizeText(left);
    const QString b = Normalizer::normalizeText(right);
    return std::min(a.size(), b.size()) >= 3 && (a.startsWith(b) || b.startsWith(a));
}

bool sameTarget(const ScriptMatch &left, const ScriptMatch &right)
{
    if (left.scriptTokenStart < 0 || right.scriptTokenStart < 0) return false;
    if (left.scriptTokenStart != right.scriptTokenStart) return false;
    return std::min(left.scriptTokenEnd, right.scriptTokenEnd)
        - left.scriptTokenStart >= 3;
}

double similarity(const QString &left, const QString &right)
{
    return FuzzRatio::ratio(Normalizer::normalizeText(left), Normalizer::normalizeText(right));
}

} // namespace

QString roughCutFailureName(RoughCutFailureType value)
{
    switch (value) {
    case RoughCutFailureType::Retake: return QStringLiteral("RETAKE");
    case RoughCutFailureType::Interrupted: return QStringLiteral("INTERRUPTED");
    case RoughCutFailureType::Duplicate: return QStringLiteral("DUPLICATE");
    case RoughCutFailureType::WrongTake: return QStringLiteral("WRONG_TAKE");
    case RoughCutFailureType::Filler: return QStringLiteral("FILLER");
    case RoughCutFailureType::None: return QStringLiteral("NONE");
    }
    return QStringLiteral("NONE");
}

QVector<RoughCutRetakeGroup> RetakeDetector::detect(
    const QVector<RecognizedPassage> &recording, const QVector<ScriptMatch> &matches,
    int sampleRate, int neighbourSeconds)
{
    QVector<RoughCutRetakeGroup> groups;
    if (sampleRate <= 0 || neighbourSeconds <= 0) return groups;
    QVector<ScriptMatch> byRecording(recording.size());
    for (int index = 0; index < byRecording.size(); ++index) byRecording[index].recordingIndex = index;
    for (const ScriptMatch &match : matches) {
        if (match.recordingIndex >= 0 && match.recordingIndex < byRecording.size())
            byRecording[match.recordingIndex] = match;
    }
    QVector<bool> grouped(recording.size(), false);
    const qint64 neighbourSamples = static_cast<qint64>(sampleRate) * neighbourSeconds;
    for (int first = 0; first < recording.size(); ++first) {
        if (grouped.at(first)) continue;
        QVector<int> candidates{first};
        for (int second = first + 1; second < recording.size(); ++second) {
            if (recording.at(second).startSample - recording.at(first).endSample > neighbourSamples) break;
            if (grouped.at(second)) continue;
            const bool sameScript = sameTarget(byRecording.at(first), byRecording.at(second));
            if (sameScript || ((byRecording.at(first).scriptTokenStart < 0
                || byRecording.at(second).scriptTokenStart < 0)
                && byRecording.at(first).scriptLineIndex >= 0
                && byRecording.at(first).scriptLineIndex == byRecording.at(second).scriptLineIndex
                && prefixRelated(recording.at(first).text, recording.at(second).text)))
                candidates.append(second);
        }
        if (candidates.size() < 2) continue;
        RoughCutRetakeGroup group;
        group.id = groups.size();
        group.scriptLineIndex = byRecording.at(first).scriptLineIndex;
        int best = -1;
        double bestScore = -1.0;
        double secondScore = -1.0;
        for (int index : candidates) {
            grouped[index] = true;
            const bool complete = byRecording.at(index).scriptTokenStart >= 0
                ? byRecording.at(index).continuousCoverage >= 85.0
                : completeSentence(recording.at(index).text);
            const bool interruption = !complete;
            const bool restartMarker = hasRestartMarker(recording.at(index).text);
            const double coverage = byRecording.at(index).continuousCoverage;
            const int coveredTokens = std::max(0, byRecording.at(index).scriptTokenEnd
                - byRecording.at(index).scriptTokenStart);
            const double score = coverage * 0.45 + byRecording.at(index).similarity * 0.35
                + byRecording.at(index).editSimilarity * 0.20 + (complete ? 12.0 : -8.0)
                - (interruption ? 6.0 : 0.0) - (restartMarker ? 10.0 : 0.0)
                + (recording.at(index).audioComplete ? 3.0 : -12.0)
                + std::min(24, coveredTokens) + index * 0.25;
            QStringList reasons{QStringLiteral("文案相似度 %1").arg(byRecording.at(index).similarity, 0, 'f', 0)};
            reasons.append(complete ? QStringLiteral("句子完整") : QStringLiteral("疑似中断"));
            if (restartMarker) reasons.append(QStringLiteral("包含重录提示词"));
            RoughCutTake take{index, score, reasons, complete, interruption, restartMarker};
            take.startSample = recording.at(index).startSample;
            take.endSample = recording.at(index).endSample;
            take.scriptTokenStart = byRecording.at(index).scriptTokenStart;
            take.scriptTokenEnd = byRecording.at(index).scriptTokenEnd;
            take.scriptCoverage = coverage;
            take.boundaryTrustworthy = recording.at(index).boundaryTrustworthy;
            take.recognizedText = recording.at(index).text;
            group.takes.append(std::move(take));
            if (score > bestScore) {
                secondScore = bestScore;
                best = index;
                bestScore = score;
            } else secondScore = std::max(secondScore, score);
        }
        const bool identical = best >= 0 && std::all_of(candidates.cbegin(), candidates.cend(),
            [&](int index) { return Normalizer::normalizeText(recording.at(index).text)
                == Normalizer::normalizeText(recording.at(best).text); });
        if ((bestScore - secondScore > 3.0 || identical)
            && best >= 0 && group.takes.at(std::distance(candidates.cbegin(),
                std::find(candidates.cbegin(), candidates.cend(), best))).complete
            && byRecording.at(best).similarity >= 68.0) {
            group.recommendedRecordingIndex = best;
            group.needsReview = false;
            group.confidence = std::clamp(bestScore, 0.0, 100.0);
            group.reason = QStringLiteral("完整度和文案相似度均有明确优势");
        } else {
            group.reason = QStringLiteral("候选质量接近或缺少可信完整版本");
        }
        if (group.recommendedRecordingIndex >= 0) {
            const QString bestText = recording.at(group.recommendedRecordingIndex).text;
            for (RoughCutTake &take : group.takes) {
                if (take.recordingIndex == group.recommendedRecordingIndex) continue;
                const QString text = recording.at(take.recordingIndex).text;
                if (take.restartMarker) {
                    take.failureType = RoughCutFailureType::Retake;
                } else if (similarity(text, bestText) >= 88.0) {
                    take.failureType = RoughCutFailureType::Duplicate;
                } else if (take.interruption && prefixRelated(text, bestText)) {
                    take.failureType = RoughCutFailureType::Interrupted;
                } else {
                    take.failureType = RoughCutFailureType::WrongTake;
                }
                take.reasons.append(QStringLiteral("失败类型 %1")
                    .arg(roughCutFailureName(take.failureType)));
            }
        }
        groups.append(std::move(group));
    }
    return groups;
}

} // namespace subcue
