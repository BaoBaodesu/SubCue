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
    entries_.clear();
    overlappingIds_.clear();
    overlapRanges_.clear();
    QVector<Event> preciseEvents;
    preciseEvents.reserve(subtitles.size() * 2);
    QVector<Event> events;
    events.reserve(subtitles.size() * 2);
    for (int index = 0; index < subtitles.size(); ++index) {
        const Subtitle &cue = subtitles.at(index);
        if (!cue.isTimed()) continue;
        entries_.append({cue.start.microseconds(), cue.end.microseconds(), cue.end.microseconds(), index});
        preciseEvents.append({cue.start.microseconds(), index, true});
        preciseEvents.append({cue.end.microseconds(), index, false});
        events.append({cue.start.milliseconds(), index, true});
        events.append({cue.end.milliseconds(), index, false});
    }
    std::sort(events.begin(), events.end(), [](const Event &left, const Event &right) {
        if (left.time != right.time) return left.time < right.time;
        if (left.start != right.start) return !left.start && right.start;
        return left.documentIndex < right.documentIndex;
    });

    std::sort(entries_.begin(), entries_.end(), [](const Entry &left, const Entry &right) {
        return left.startUs != right.startUs ? left.startUs < right.startUs : left.documentIndex < right.documentIndex;
    });
    // 平衡区间索引只保存每条字幕一次，不为重叠区间复制活动字幕集合。
    const auto build = [this](auto &&self, int first, int last) -> qint64 {
        if (first >= last) return 0;
        const int middle = first + (last - first) / 2;
        entries_[middle].maximumEndUs = std::max({entries_[middle].endUs,
            self(self, first, middle), self(self, middle + 1, last)});
        return entries_[middle].maximumEndUs;
    };
    build(build, 0, static_cast<int>(entries_.size()));
    std::sort(preciseEvents.begin(), preciseEvents.end(), [](const Event &left, const Event &right) {
        if (left.time != right.time) return left.time < right.time;
        return left.start != right.start ? !left.start : left.documentIndex < right.documentIndex;
    });
    std::set<int> overlapping;
    qint64 previous = 0;
    for (const Event &event : preciseEvents) {
        if (event.time > previous && overlapping.size() > 1) {
            if (!overlapRanges_.isEmpty() && overlapRanges_.last().second == previous)
                overlapRanges_.last().second = event.time;
            else overlapRanges_.append({previous, event.time});
        }
        if (event.start) {
            if (!overlapping.empty()) {
                overlappingIds_.insert(subtitles.at(*overlapping.begin()).id);
                overlappingIds_.insert(subtitles.at(event.documentIndex).id);
            }
            overlapping.insert(event.documentIndex);
        } else overlapping.erase(event.documentIndex);
        previous = event.time;
    }

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

QVector<int> SubtitleTimeIndex::activeDocumentIndices(qint64 positionUs) const
{
    QVector<int> result;
    const auto query = [this, positionUs, &result](auto &&self, int first, int last) -> void {
        if (first >= last) return;
        const int middle = first + (last - first) / 2;
        const Entry &entry = entries_.at(middle);
        if (entry.maximumEndUs <= positionUs) return;
        self(self, first, middle);
        if (entry.startUs <= positionUs && positionUs < entry.endUs) result.append(entry.documentIndex);
        if (entry.startUs <= positionUs) self(self, middle + 1, last);
    };
    query(query, 0, static_cast<int>(entries_.size()));
    std::sort(result.begin(), result.end());
    return result;
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
