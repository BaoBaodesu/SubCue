#include "timeline/timeline_scene.h"

#include <algorithm>
#include <cmath>

namespace subcue {
namespace {

bool containsX(const TimelineSceneRect &rect, double x)
{
    return x >= rect.x && x <= rect.x + rect.width;
}

bool contains(const TimelineSceneRect &rect, double x, double y)
{
    return containsX(rect, x) && y >= rect.y && y <= rect.y + rect.height;
}

} // namespace

TimelineSceneLayout TimelineSceneBuilder::build(
    const TimelineViewport &viewport,
    const QList<Subtitle> &subtitles,
    const WaveformPyramid *waveform,
    const TimelineSceneMetrics &metrics,
    const QString &selectedId,
    const std::optional<MediaTime> &inPoint,
    const std::optional<MediaTime> &outPoint)
{
    TimelineSceneLayout layout;
    const double width = std::max(0.0, metrics.viewportWidth);
    const double height = std::max(0.0, metrics.viewportHeight);
    layout.background = {0.0, 0.0, width, height};
    layout.ruler = {0.0, 0.0, width, std::min(metrics.rulerHeight, height)};
    layout.subtitleTrack = {
        0.0,
        layout.ruler.height,
        width,
        std::min(metrics.subtitleTrackHeight, std::max(0.0, height - layout.ruler.height))};
    layout.audioTrack = {
        0.0,
        layout.ruler.height + layout.subtitleTrack.height,
        width,
        std::max(0.0, height - layout.ruler.height - layout.subtitleTrack.height)};

    const QVector<qint64> ticks = viewport.rulerTickMs(width);
    layout.ticks.reserve(ticks.size());
    for (qint64 ms : ticks) {
        TimelineRulerTick tick;
        tick.ms = ms;
        tick.x = viewport.xAtTime(MediaTime::fromMilliseconds(ms));
        layout.ticks.push_back(tick);
    }

    const double cueHeight = std::max(2.0, layout.subtitleTrack.height - metrics.cueVerticalPadding * 2.0);
    const double cueY = layout.subtitleTrack.y + metrics.cueVerticalPadding;
    for (const Subtitle &cue : subtitles) {
        if (!cue.isTimed()) {
            continue;
        }
        if (!viewport.rangeOverlapsViewport(cue.start, cue.end, width)) {
            continue;
        }
        TimelineCueVisual visual;
        visual.id = cue.id;
        visual.text = cue.text;
        visual.selected = cue.id == selectedId;
        visual.pending = cue.status == QStringLiteral("LOW_CONFIDENCE");
        visual.rect.x = viewport.xAtTime(cue.start);
        visual.rect.y = cueY;
        visual.rect.width = std::max(2.0, viewport.xAtTime(cue.end) - visual.rect.x);
        visual.rect.height = cueHeight;
        layout.cues.push_back(std::move(visual));
    }

    if (waveform && layout.audioTrack.height > 0.0 && width >= 1.0) {
        const int columns = std::max(0, static_cast<int>(std::ceil(std::min(width,
            viewport.xAtTime(std::min(waveform->duration(), viewport.visibleEnd(width)))))));
        layout.waveform = waveform->peaksForRange(viewport.visibleStart(),
            std::min(waveform->duration(), viewport.visibleEnd(width)), columns);
    }

    if (inPoint && outPoint && outPoint->microseconds() > inPoint->microseconds()) {
        layout.inOutRange.x = viewport.xAtTime(*inPoint);
        layout.inOutRange.y = 0.0;
        layout.inOutRange.width = std::max(1.0, viewport.xAtTime(*outPoint) - layout.inOutRange.x);
        layout.inOutRange.height = height;
    }

    layout.playheadX = viewport.xAtTime(viewport.playhead());
    layout.playheadVisible = layout.playheadX >= -2.0 && layout.playheadX <= width + 2.0;
    return layout;
}

TimelineHit TimelineSceneBuilder::hitTest(
    const TimelineSceneLayout &layout, double x, double y, const TimelineSceneMetrics &metrics)
{
    TimelineHit hit;
    for (const TimelineCueVisual &cue : layout.cues) {
        if (!contains(cue.rect, x, y)) {
            continue;
        }
        hit.cueId = cue.id;
        if (x <= cue.rect.x + metrics.trimHandleWidth) {
            hit.kind = TimelineHitKind::CueTrimStart;
        } else if (x >= cue.rect.x + cue.rect.width - metrics.trimHandleWidth) {
            hit.kind = TimelineHitKind::CueTrimEnd;
        } else {
            hit.kind = TimelineHitKind::CueBody;
        }
        return hit;
    }

    if (layout.playheadVisible && std::abs(x - layout.playheadX) <= metrics.playheadHitWidth) {
        hit.kind = TimelineHitKind::Playhead;
        return hit;
    }
    if (contains(layout.ruler, x, y)) {
        hit.kind = TimelineHitKind::Ruler;
        return hit;
    }
    if (contains(layout.audioTrack, x, y)) {
        hit.kind = TimelineHitKind::Waveform;
        return hit;
    }
    return hit;
}

bool TimelineSceneBuilder::shouldDrawWaveform(const TimelineSceneLayout &layout)
{
    return !layout.waveform.isEmpty() && layout.audioTrack.height > 0.0;
}

bool TimelineSceneBuilder::shouldDrawAudioClip(const TimelineSceneLayout &layout)
{
    return !layout.waveform.isEmpty() && layout.audioTrack.height > 2.0;
}

} // namespace subcue
