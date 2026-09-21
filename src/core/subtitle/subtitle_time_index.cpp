#include "subtitle/subtitle_time_index.h"

#include <algorithm>
#include <set>

namespace subcue {
namespace {

struct Event {
    qint64 time = 0;
    int documentIndex = 0;
    bool start = false;
};

} // namespace

void SubtitleTimeIndex::rebuild(const QList<Subtitle> &subtitles)
{
    QVector<Event> events;
    events.reserve(subtitles.size() * 2);
    for (int index = 0; index < subtitles.size(); ++index) {
        const Subtitle &cue = subtitles.at(index);
        if (!cue.isTimed()) continue;
        events.append({cue.start.milliseconds(), index, true});
        events.append({cue.end.milliseconds(), index, false});
    }
    std::sort(events.begin(), events.end(), [](const Event &left, const Event &right) {
        if (left.time != right.time) return left.time < right.time;
        if (left.start != right.start) return !left.start && right.start;
        return left.documentIndex < right.documentIndex;
    });

    intervals_.clear();
    intervals_.reserve(events.size());
    std::set<int> active;
    qint64 cursor = 0;
    bool started = false;
    for (const Event &event : events) {
        if (started && event.time > cursor) {
            intervals_.append({cursor, event.time, active.empty() ? -1 : *active.begin()});
        }
        if (event.start) active.insert(event.documentIndex);
        else active.erase(event.documentIndex);
        cursor = event.time;
        started = true;
    }
    invalidateCache();
}

void SubtitleTimeIndex::invalidateCache() noexcept
{
    cachedInterval_ = -1;
    cachedDocumentIndex_ = -1;
    lastPositionMs_ = -1;
}

int SubtitleTimeIndex::relocate(qint64 positionMs) const
{
    if (intervals_.isEmpty()) return -1;
    auto firstAfter = std::upper_bound(intervals_.cbegin(), intervals_.cend(), positionMs,
        [](qint64 position, const Interval &interval) { return position < interval.startMs; });
    if (firstAfter == intervals_.cbegin()) return -1;
    const auto current = firstAfter - 1;
    if (positionMs >= current->startMs && positionMs < current->endMs) {
        return static_cast<int>(current - intervals_.cbegin());
    }
    return -1;
}

int SubtitleTimeIndex::documentIndexAt(qint64 positionMs) const
{
    const int interval = relocate(positionMs);
    return interval >= 0 ? intervals_.at(interval).winner : -1;
}

int SubtitleTimeIndex::lookup(qint64 positionMs, int direction)
{
    if (cachedInterval_ >= 0 && cachedInterval_ < intervals_.size()) {
        const Interval &current = intervals_.at(cachedInterval_);
        if (positionMs >= current.startMs && positionMs < current.endMs) {
            lastPositionMs_ = positionMs;
            cachedDocumentIndex_ = current.winner;
            return cachedDocumentIndex_;
        }
        if (direction >= 0 && lastPositionMs_ >= 0 && positionMs >= lastPositionMs_) {
            int interval = cachedInterval_;
            for (int step = 0; step < 16 && interval + 1 < intervals_.size(); ++step) {
                ++interval;
                const Interval &next = intervals_.at(interval);
                if (positionMs >= next.startMs && positionMs < next.endMs) {
                    cachedInterval_ = interval;
                    cachedDocumentIndex_ = next.winner;
                    lastPositionMs_ = positionMs;
                    return cachedDocumentIndex_;
                }
                if (positionMs < next.startMs) break;
            }
        }
    }

    cachedInterval_ = relocate(positionMs);
    cachedDocumentIndex_ = cachedInterval_ >= 0 ? intervals_.at(cachedInterval_).winner : -1;
    lastPositionMs_ = positionMs;
    return cachedDocumentIndex_;
}

} // namespace subcue
