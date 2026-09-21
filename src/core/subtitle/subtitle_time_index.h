#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QList>
#include <QtCore/QVector>

namespace subcue {

class SubtitleTimeIndex final {
public:
    void rebuild(const QList<Subtitle> &subtitles);
    void invalidateCache() noexcept;

    [[nodiscard]] int documentIndexAt(qint64 positionMs) const;
    [[nodiscard]] int lookup(qint64 positionMs, int direction);

    [[nodiscard]] int intervalCount() const noexcept { return intervals_.size(); }
    [[nodiscard]] int cachedDocumentIndex() const noexcept { return cachedDocumentIndex_; }

private:
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
