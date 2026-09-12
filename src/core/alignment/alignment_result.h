#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QVector>

namespace subcue {

// 对应 models/alignment_result.py。
struct AlignmentResult final {
    QVector<Subtitle> subtitles;

    [[nodiscard]] int highCount() const noexcept
    {
        int count = 0;
        for (const Subtitle &subtitle : subtitles) {
            if (subtitle.confidence >= 0.85) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] int lowCount() const noexcept
    {
        int count = 0;
        for (const Subtitle &subtitle : subtitles) {
            if (subtitle.status != QStringLiteral("UNMATCHED")
                && subtitle.status != QStringLiteral("SKIPPED_NO_AUDIO")
                && subtitle.confidence < 0.70) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] int unmatchedCount() const noexcept
    {
        int count = 0;
        for (const Subtitle &subtitle : subtitles) {
            if (subtitle.status == QStringLiteral("UNMATCHED")
                || subtitle.status == QStringLiteral("SKIPPED_NO_AUDIO")) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] int skippedCount() const noexcept
    {
        int count = 0;
        for (const Subtitle &subtitle : subtitles) {
            if (subtitle.status == QStringLiteral("SKIPPED_NO_AUDIO")) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] QVector<Subtitle> exportableSubtitles() const
    {
        QVector<Subtitle> out;
        out.reserve(subtitles.size());
        for (const Subtitle &subtitle : subtitles) {
            if (subtitle.isExportable()) {
                out.append(subtitle);
            }
        }
        return out;
    }
};

} // namespace subcue
