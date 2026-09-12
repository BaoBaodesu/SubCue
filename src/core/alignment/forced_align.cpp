#include "alignment/forced_align.h"

#include "alignment/alignment_engine.h"
#include "alignment/normalizer.h"
#include "alignment/python_round.h"

#include <algorithm>

namespace subcue {
namespace {

constexpr qint64 kMinDurationMs = ForcedAligner::kMinDurationMs;

[[nodiscard]] bool isPlaced(const Subtitle &subtitle)
{
    return subtitle.status != QStringLiteral("UNMATCHED") && subtitle.start < subtitle.end;
}

// 复刻 core/forced_align.py::_distribute_evenly。
void distributeEvenly(QVector<Subtitle> &subtitles, qint64 startMs, qint64 endMs)
{
    if (subtitles.isEmpty()) {
        return;
    }
    qint64 totalChars = 0;
    for (const Subtitle &subtitle : subtitles) {
        totalChars += std::max<qint64>(
            1, static_cast<qint64>(Normalizer::normalizeCodepoints(subtitle.text).size()));
    }
    const qint64 available = std::max(
        endMs - startMs, kMinDurationMs * static_cast<qint64>(subtitles.size()));
    qint64 cursor = startMs;
    for (Subtitle &subtitle : subtitles) {
        const qint64 chars = std::max<qint64>(
            1, static_cast<qint64>(Normalizer::normalizeCodepoints(subtitle.text).size()));
        const qint64 span = std::max(kMinDurationMs, pythonRound(
            static_cast<double>(available * chars)
            / static_cast<double>(std::max<qint64>(1, totalChars))));
        subtitle.start = MediaTime::fromMilliseconds(cursor);
        subtitle.end = MediaTime::fromMilliseconds(cursor + span);
        cursor += span;
        if (subtitle.status == QStringLiteral("UNMATCHED")) {
            subtitle.status = QStringLiteral("GAPPED");
            subtitle.confidence = std::max(subtitle.confidence, 0.30);
            subtitle.source = QStringLiteral("interpolated");
        }
    }
    Subtitle &last = subtitles.last();
    if (last.end.milliseconds() > endMs
        && endMs > last.start.milliseconds() + kMinDurationMs) {
        last.end = MediaTime::fromMilliseconds(endMs);
    }
}

// 复刻 core/forced_align.py::_fill_gaps。
void fillGaps(QVector<Subtitle> &subtitles, qint64 durationMs)
{
    if (subtitles.isEmpty()) {
        return;
    }
    qsizetype index = 0;
    while (index < subtitles.size()) {
        if (isPlaced(subtitles[index])) {
            ++index;
            continue;
        }
        const qsizetype runStart = index;
        while (index < subtitles.size() && !isPlaced(subtitles[index])) {
            ++index;
        }
        const qsizetype runEnd = index;
        const qint64 leftMs = runStart > 0
            ? subtitles[runStart - 1].end.milliseconds() : 0;
        qint64 rightMs = runEnd < subtitles.size()
            ? subtitles[runEnd].start.milliseconds() : durationMs;
        if (rightMs <= leftMs) {
            rightMs = leftMs + kMinDurationMs * (runEnd - runStart);
            rightMs = std::min(rightMs, durationMs);
        }
        QVector<Subtitle> slice;
        slice.reserve(runEnd - runStart);
        for (qsizetype k = runStart; k < runEnd; ++k) {
            slice.append(subtitles[k]);
        }
        distributeEvenly(slice, leftMs, rightMs);
        for (qsizetype k = runStart; k < runEnd; ++k) {
            subtitles[k] = slice[k - runStart];
        }
    }
}

// 复刻 core/forced_align.py::_enforce_constraints。
void enforceConstraints(QVector<Subtitle> &subtitles, qint64 durationMs)
{
    for (qsizetype index = 0; index < subtitles.size(); ++index) {
        Subtitle &subtitle = subtitles[index];
        if (subtitle.end.milliseconds() - subtitle.start.milliseconds() < kMinDurationMs) {
            subtitle.end = MediaTime::fromMilliseconds(
                subtitle.start.milliseconds() + kMinDurationMs);
        }
        if (index + 1 < subtitles.size()) {
            Subtitle &next = subtitles[index + 1];
            if (subtitle.end > next.start) {
                if (subtitle.confidence >= next.confidence) {
                    next.start = subtitle.end;
                } else {
                    subtitle.end = next.start;
                }
                if (subtitle.end.milliseconds() - subtitle.start.milliseconds() < kMinDurationMs) {
                    subtitle.end = MediaTime::fromMilliseconds(
                        subtitle.start.milliseconds() + kMinDurationMs);
                }
                if (next.end.milliseconds() - next.start.milliseconds() < kMinDurationMs) {
                    next.end = MediaTime::fromMilliseconds(
                        next.start.milliseconds() + kMinDurationMs);
                }
            }
        }
    }
    if (!subtitles.isEmpty()) {
        Subtitle &last = subtitles.last();
        if (last.end.milliseconds() > durationMs) {
            last.end = MediaTime::fromMilliseconds(std::max(
                last.start.milliseconds() + kMinDurationMs, durationMs));
        }
    }
}

} // namespace

AlignmentResult ForcedAligner::forcedAlignSubtitleLines(
    const QStringList &lines, const QVector<TranscriptWord> &words, qint64 durationMs)
{
    AlignmentResult result = AlignmentEngine::alignSubtitleLines(lines, words);
    if (words.isEmpty() || durationMs <= 0) {
        distributeEvenly(result.subtitles, 0, durationMs);
        return result;
    }
    fillGaps(result.subtitles, durationMs);
    enforceConstraints(result.subtitles, durationMs);
    return result;
}

} // namespace subcue
