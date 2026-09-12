#pragma once

#include "alignment/alignment_result.h"
#include "alignment/transcript.h"

#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace subcue {

// 复刻 core/forced_align.py：保证每行都有非零时长、start < end、单调不重叠。
class ForcedAligner final {
public:
    static constexpr qint64 kMinDurationMs = 250;

    // 对应 core/forced_align.py::forced_align_subtitles。
    [[nodiscard]] static AlignmentResult forcedAlignSubtitleLines(
        const QStringList &lines, const QVector<TranscriptWord> &words, qint64 durationMs);
};

} // namespace subcue
