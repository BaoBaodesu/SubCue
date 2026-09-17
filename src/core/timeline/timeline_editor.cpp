#include "timeline/timeline_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace subcue {
namespace {

qint64 clampUs(qint64 value, qint64 minimum, qint64 maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

} // namespace

TimelineEditor::TimelineEditor(
    SubtitleDocument *document,
    SubtitleCommandManager *commands,
    TimelineViewport *viewport,
    SnapEngine *snap)
    : document_(document), commands_(commands), viewport_(viewport), snap_(snap)
{
    Q_ASSERT(document_);
    Q_ASSERT(commands_);
    Q_ASSERT(viewport_);
    Q_ASSERT(snap_);
}

void TimelineEditor::setInPoint(std::optional<MediaTime> inPoint)
{
    inPoint_ = std::move(inPoint);
}

void TimelineEditor::setOutPoint(std::optional<MediaTime> outPoint)
{
    outPoint_ = std::move(outPoint);
}

NeighborBounds TimelineEditor::neighborBounds(const QString &id) const
{
    NeighborBounds bounds;
    bounds.previousEnd = MediaTime::fromMicroseconds(0);
    bounds.nextStart = viewport_->timelineDuration();
    const int index = document_->indexOf(id);
    if (index < 0) {
        return bounds;
    }
    const QList<Subtitle> &list = document_->subtitles();
    for (int i = index - 1; i >= 0; --i) {
        if (list.at(i).isTimed()) {
            bounds.previousEnd = list.at(i).end;
            break;
        }
    }
    for (int i = index + 1; i < list.size(); ++i) {
        if (list.at(i).isTimed()) {
            bounds.nextStart = list.at(i).start;
            break;
        }
    }
    return bounds;
}

QList<Subtitle> TimelineEditor::visibleCues(double viewportWidth) const
{
    QList<Subtitle> visible;
    for (Subtitle cue : document_->subtitles()) {
        if (drag_ && cue.id == drag_->id) {
            cue.start = drag_->previewStart;
            cue.end = drag_->previewEnd;
        }
        if (!cue.isTimed()) {
            continue;
        }
        if (viewport_->rangeOverlapsViewport(cue.start, cue.end, viewportWidth)) {
            visible.push_back(std::move(cue));
        }
    }
    return visible;
}

std::optional<Subtitle> TimelineEditor::previewCue() const
{
    if (!drag_) {
        return std::nullopt;
    }
    auto cue = document_->subtitle(drag_->id);
    if (!cue) {
        return std::nullopt;
    }
    cue->start = drag_->previewStart;
    cue->end = drag_->previewEnd;
    return cue;
}

bool TimelineEditor::beginDrag(const QString &id, CueDragMode mode)
{
    const auto cue = document_->subtitle(id);
    if (!cue || !cue->isTimed()) {
        return false;
    }
    DragState drag;
    drag.id = id;
    drag.mode = mode;
    drag.originalStart = cue->start;
    drag.originalEnd = cue->end;
    drag.previewStart = cue->start;
    drag.previewEnd = cue->end;
    drag_ = std::move(drag);
    return true;
}

bool TimelineEditor::updateDrag(MediaTime delta)
{
    if (!drag_) {
        return false;
    }

    DragState &drag = *drag_;
    const NeighborBounds bounds = neighborBounds(drag.id);
    if (drag.mode == CueDragMode::TrimStart) {
        const qint64 snapped = snapTime(
            MediaTime::fromMicroseconds(drag.originalStart.microseconds() + delta.microseconds()),
            drag.id).microseconds();
        const qint64 start = clampUs(
            snapped,
            bounds.previousEnd.microseconds(),
            drag.originalEnd.microseconds() - kMinCueUs);
        drag.previewStart = MediaTime::fromMicroseconds(start);
        drag.previewEnd = drag.originalEnd;
    } else if (drag.mode == CueDragMode::TrimEnd) {
        const qint64 snapped = snapTime(
            MediaTime::fromMicroseconds(drag.originalEnd.microseconds() + delta.microseconds()),
            drag.id).microseconds();
        const qint64 end = clampUs(
            snapped,
            drag.originalStart.microseconds() + kMinCueUs,
            bounds.nextStart.microseconds());
        drag.previewStart = drag.originalStart;
        drag.previewEnd = MediaTime::fromMicroseconds(end);
    } else {
        const qint64 duration = drag.originalEnd.microseconds() - drag.originalStart.microseconds();
        qint64 start = drag.originalStart.microseconds() + delta.microseconds();
        if (snap_->isEnabled()) {
            const qint64 snapStart = snapTime(MediaTime::fromMicroseconds(start), drag.id).microseconds();
            const qint64 snapEndAsStart =
                snapTime(MediaTime::fromMicroseconds(start + duration), drag.id).microseconds() - duration;
            start = std::llabs(snapStart - start) <= std::llabs(snapEndAsStart - start)
                ? snapStart
                : snapEndAsStart;
        }
        // 移动字幕时允许预览跨过其他字幕；只在提交时检查最终位置是否重叠。
        start = clampUs(start, 0, viewport_->timelineDuration().microseconds() - duration);
        drag.previewStart = MediaTime::fromMicroseconds(start);
        drag.previewEnd = MediaTime::fromMicroseconds(start + duration);
    }
    return true;
}

bool TimelineEditor::updateDragByPixels(double deltaX)
{
    const double ms = deltaX / std::max(0.0001, viewport_->pixelsPerMs());
    return updateDrag(MediaTime::fromMicroseconds(static_cast<qint64>(std::llround(ms * 1000.0))));
}

bool TimelineEditor::endDrag()
{
    if (!drag_) {
        return false;
    }
    const DragState drag = *drag_;
    drag_.reset();
    if (drag.previewStart == drag.originalStart && drag.previewEnd == drag.originalEnd) {
        return true;
    }
    return setTiming(drag.id, drag.previewStart, drag.previewEnd);
}

void TimelineEditor::cancelDrag()
{
    drag_.reset();
}

bool TimelineEditor::setTiming(const QString &id, MediaTime start, MediaTime end)
{
    if (!document_->subtitle(id)) {
        return false;
    }
    if (end.microseconds() - start.microseconds() < kMinCueUs) {
        return false;
    }
    if (start.microseconds() < 0
        || end.microseconds() > viewport_->timelineDuration().microseconds()) {
        return false;
    }
    for (const Subtitle &subtitle : document_->subtitles()) {
        if (subtitle.id != id && subtitle.isTimed()
            && start < subtitle.end && end > subtitle.start) {
            return false;
        }
    }
    return commands_->setTiming(id, start, end);
}

bool TimelineEditor::splitAtPlayhead(const QString &id)
{
    const auto cue = document_->subtitle(id);
    if (!cue) {
        return false;
    }
    const qint64 playhead = viewport_->playhead().microseconds();
    if (playhead - cue->start.microseconds() < kMinCueUs
        || cue->end.microseconds() - playhead < kMinCueUs) {
        return false;
    }
    return commands_->split(id, viewport_->playhead(), cue->text, QString());
}

bool TimelineEditor::joinAroundPlayhead()
{
    const qint64 playhead = viewport_->playhead().microseconds();
    int before = -1;
    int after = -1;
    const QList<Subtitle> &list = document_->subtitles();
    for (int index = 0; index < list.size(); ++index) {
        const Subtitle &cue = list.at(index);
        if (!cue.isTimed()) {
            continue;
        }
        if (cue.end.microseconds() <= playhead) {
            before = index;
        } else if (cue.start.microseconds() >= playhead && after < 0) {
            after = index;
            break;
        }
    }
    if (before < 0 || after < 0) {
        return false;
    }
    const Subtitle left = list.at(before);
    const Subtitle right = list.at(after);
    if (playhead - left.start.microseconds() < kMinCueUs
        || right.end.microseconds() - playhead < kMinCueUs) {
        return false;
    }
    commands_->stack()->beginMacro(QStringLiteral("衔接字幕"));
    const bool leftOk = commands_->setTiming(left.id, left.start, viewport_->playhead());
    const bool rightOk = commands_->setTiming(right.id, viewport_->playhead(), right.end);
    commands_->stack()->endMacro();
    return leftOk && rightOk;
}

MediaTime TimelineEditor::snapTime(MediaTime value, const QString &excludeId) const
{
    return snap_->snap(
        value,
        *viewport_,
        viewport_->playhead(),
        inPoint_,
        outPoint_,
        document_->subtitles(),
        excludeId);
}

} // namespace subcue
