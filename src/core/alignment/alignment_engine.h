#pragma once

#include "alignment/alignment_result.h"
#include "alignment/transcript.h"
#include "subtitle/subtitle.h"

#include <QtCore/QStringList>
#include <QtCore/QStringView>
#include <QtCore/QVector>

namespace subcue {

// 复刻 core/alignment.py 与 core/confidence.py 的自动对齐行为。
// 语义必须与 Python 后端逐位一致（RapidFuzz ratio、CPython round/NFKC/lower）。
class AlignmentEngine final {
public:
    [[nodiscard]] static QString normalizeText(QStringView text);

    [[nodiscard]] static double fuzzRatio(QStringView left, QStringView right);

    // 对应 core/confidence.py::calculate_confidence。
    [[nodiscard]] static double calculateConfidence(
        double similarity, double ambiguity, double lengthRatio) noexcept;

    // 对应 core/confidence.py::needs_ai_review。
    [[nodiscard]] static bool needsAiReview(double confidence, double ambiguity = 1.0) noexcept;

    // 对应 core/alignment.py::align_subtitles。
    [[nodiscard]] static AlignmentResult alignSubtitleLines(
        const QStringList &lines, const QVector<TranscriptWord> &words);

    // 对应 core/alignment.py::finalize_audio_first。
    static void finalizeAudioFirst(QVector<Subtitle> &subtitles);
};

} // namespace subcue
