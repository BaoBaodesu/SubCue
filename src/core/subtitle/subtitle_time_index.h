#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QList>
#include <QtCore/QVector>
#include <QtCore/QSet>

namespace subcue {

class SubtitleTimeIndex final {
public:
    void rebuild(const QList<Subtitle> &subtitles);
    void invalidateCache() noexcept;

    [[nodiscard]] int documentIndexAt(qint64 positionMs) const;
    [[nodiscard]] int lookup(qint64 positionMs, int direction);
    [[nodiscard]] QVector<int> activeDocumentIndices(qint64 positionUs) const;
    [[nodiscard]] const QSet<QString> &overlappingIds() const noexcept { return overlappingIds_; }
    [[nodiscard]] const QVector<QPair<qint64, qint64>> &overlapRanges() const noexcept { return overlapRanges_; }

    [[nodiscard]] int intervalCount() const noexcept { return intervals_.size(); }
    [[nodiscard]] int cachedDocumentIndex() const noexcept { return cachedDocumentIndex_; }

private:
    struct Entry {
        qint64 startUs = 0;
        qint64 endUs = 0;
        qint64 maximumEndUs = 0;
        int documentIndex = -1;
    };
    QVector<Entry> entries_;
    QSet<QString> overlappingIds_;
    QVector<QPair<qint64, qint64>> overlapRanges_;
    struct Interval {
        qint64 startMs = 0;
        qint64 endMs = 0;
        int winner = -1;
    };

    [[nodiscard]] int relocate(qint64 positionMs) const;

    QVector<Interval> intervals_;
    int cachedInterval_ = -1;
    int cachedDocumentIndex_ = -1;
    qint64 lastPositionMs_ = -1;
};

} // namespace subcue
