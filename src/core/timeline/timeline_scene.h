#pragma once

#include "common/media_time.h"
#include "subtitle/subtitle.h"
#include "timeline/timeline_viewport.h"
#include "waveform/waveform_pyramid.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>

namespace subcue {

struct TimelineSceneRect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool operator==(const TimelineSceneRect &) const = default;
};

struct TimelineCueVisual final {
    QString id;
    QString text;
    TimelineSceneRect rect;
    bool selected = false;
    bool pending = false;
    bool operator==(const TimelineCueVisual &) const = default;
};

struct TimelineRulerTick final {
    double x = 0.0;
    qint64 ms = 0;
};

struct TimelineSceneMetrics final {
    double viewportWidth = 0.0;
    double viewportHeight = 0.0;
    double rulerHeight = 28.0;
    double subtitleTrackHeight = 54.0;
    double cueVerticalPadding = 7.0;
    double trimHandleWidth = 8.0;
    double playheadHitWidth = 4.0;
};

enum class TimelineHitKind {
    None,
    Ruler,
    Playhead,
    CueBody,
    CueTrimStart,
    CueTrimEnd,
    Waveform
};

struct TimelineHit final {
    TimelineHitKind kind = TimelineHitKind::None;
    QString cueId;
};

struct TimelineSceneLayout final {
    TimelineSceneRect background;
    TimelineSceneRect ruler;
    TimelineSceneRect subtitleTrack;
    TimelineSceneRect audioTrack;
    QVector<TimelineRulerTick> ticks;
    QVector<TimelineCueVisual> cues;
    QVector<WaveformPeak> waveform;
    TimelineSceneRect inOutRange;
    double playheadX = 0.0;
    bool playheadVisible = false;
};

class TimelineSceneBuilder final {
public:
    [[nodiscard]] static TimelineSceneLayout build(
        const TimelineViewport &viewport,
        const QList<Subtitle> &subtitles,
        const WaveformPyramid *waveform,
        const TimelineSceneMetrics &metrics,
        const QString &selectedId = {},
        const std::optional<MediaTime> &inPoint = {},
        const std::optional<MediaTime> &outPoint = {});

    [[nodiscard]] static TimelineHit hitTest(const TimelineSceneLayout &layout, double x, double y,
                                             const TimelineSceneMetrics &metrics);

    [[nodiscard]] static bool shouldDrawWaveform(const TimelineSceneLayout &layout);
    [[nodiscard]] static bool shouldDrawAudioClip(const TimelineSceneLayout &layout);
};

} // namespace subcue
