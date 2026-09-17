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
    static const QStringList markers{QStringLiteral("呃"), QStringLiteral("啊"),
        QStringLiteral("不是"), QStringLiteral("重来"), QStringLiteral("再来")};
    return std::any_of(markers.cbegin(), markers.cend(), [&](const QString &marker) {
        return normalized.contains(marker);
    });
}

double similarity(const QString &left, const QString &right)
{
    return FuzzRatio::ratio(Normalizer::normalizeText(left), Normalizer::normalizeText(right));
}

} // namespace

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
            const bool sameScript = byRecording.at(first).scriptLineIndex >= 0
                && byRecording.at(first).scriptLineIndex == byRecording.at(second).scriptLineIndex;
            if (sameScript || similarity(recording.at(first).text, recording.at(second).text) >= 75.0)
                candidates.append(second);
        }
        if (candidates.size() < 2) continue;
        RoughCutRetakeGroup group;
        group.id = groups.size();
        group.scriptLineIndex = byRecording.at(first).scriptLineIndex;
        int best = -1;
        double bestScore = -1.0;
        bool tied = false;
        for (int index : candidates) {
            grouped[index] = true;
            const bool complete = completeSentence(recording.at(index).text)
                || byRecording.at(index).continuousCoverage >= 90.0;
            const bool interruption = !complete || recording.at(index).silenceAfterSamples
                > static_cast<qint64>(sampleRate) * 2;
            const bool restartMarker = hasRestartMarker(recording.at(index).text);
            const double coverage = byRecording.at(index).continuousCoverage;
            const double score = coverage * 0.45 + byRecording.at(index).similarity * 0.35
                + byRecording.at(index).editSimilarity * 0.20 + (complete ? 12.0 : -8.0)
                - (interruption ? 6.0 : 0.0) - (restartMarker ? 10.0 : 0.0)
                + (recording.at(index).audioComplete ? 3.0 : -12.0)
                + index * 0.25;
            QStringList reasons{QStringLiteral("文案相似度 %1").arg(byRecording.at(index).similarity, 0, 'f', 0)};
            reasons.append(complete ? QStringLiteral("句子完整") : QStringLiteral("疑似中断"));
            if (restartMarker) reasons.append(QStringLiteral("包含重录提示词"));
            group.takes.append({index, score, reasons, complete, interruption, restartMarker});
            if (score > bestScore + 5.0) {
                best = index;
                bestScore = score;
                tied = false;
            } else if (std::abs(score - bestScore) <= 5.0) {
                tied = true;
            }
        }
        if (!tied && best >= 0 && group.takes.at(std::distance(candidates.cbegin(),
                std::find(candidates.cbegin(), candidates.cend(), best))).complete
            && byRecording.at(best).similarity >= 72.0) {
            group.recommendedRecordingIndex = best;
            group.needsReview = false;
            group.confidence = std::clamp(bestScore, 0.0, 100.0);
            group.reason = QStringLiteral("完整度和文案相似度均有明确优势");
        } else {
            group.reason = QStringLiteral("候选质量接近或缺少可信完整版本");
        }
        groups.append(std::move(group));
    }
    return groups;
}

} // namespace subcue
