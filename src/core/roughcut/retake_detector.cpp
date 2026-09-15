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
    if (sampleRate <= 0 || neighbourSeconds <= 0 || matches.size() != recording.size()) return groups;
    QVector<bool> grouped(recording.size(), false);
    const qint64 neighbourSamples = static_cast<qint64>(sampleRate) * neighbourSeconds;
    for (int first = 0; first < recording.size(); ++first) {
        if (grouped.at(first)) continue;
        QVector<int> candidates{first};
        for (int second = first + 1; second < recording.size(); ++second) {
            if (recording.at(second).startSample - recording.at(first).endSample > neighbourSamples) break;
            const bool sameScript = matches.at(first).scriptLineIndex >= 0
                && matches.at(first).scriptLineIndex == matches.at(second).scriptLineIndex;
            if (sameScript || similarity(recording.at(first).text, recording.at(second).text) >= 75.0)
                candidates.append(second);
        }
        if (candidates.size() < 2) continue;
        RoughCutRetakeGroup group;
        int best = -1;
        double bestScore = -1.0;
        bool tied = false;
        for (int index : candidates) {
            grouped[index] = true;
            const bool complete = completeSentence(recording.at(index).text);
            const double score = matches.at(index).similarity + (complete ? 10.0 : 0.0);
            QStringList reasons{QStringLiteral("文案相似度 %1").arg(matches.at(index).similarity, 0, 'f', 0)};
            reasons.append(complete ? QStringLiteral("句子完整") : QStringLiteral("疑似中断"));
            group.takes.append({index, score, reasons});
            if (score > bestScore + 5.0) {
                best = index;
                bestScore = score;
                tied = false;
            } else if (std::abs(score - bestScore) <= 5.0) {
                tied = true;
            }
        }
        if (!tied && completeSentence(recording.at(best).text)
            && matches.at(best).similarity >= 75.0) {
            group.recommendedRecordingIndex = best;
            group.needsReview = false;
            group.reason = QStringLiteral("完整度和文案相似度均有明确优势");
        } else {
            group.reason = QStringLiteral("候选质量接近或缺少可信完整版本");
        }
        groups.append(std::move(group));
    }
    return groups;
}

} // namespace subcue
