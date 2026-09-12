#include "timeline/snap_engine.h"

#include <cmath>
#include <cstdlib>

namespace subcue {

void SnapEngine::setFps(double fps)
{
    fps_ = fps > 0.0 ? fps : kDefaultFps;
    frameDuration_ = MediaTime::fromMicroseconds(
        static_cast<qint64>(std::llround(1'000'000.0 / fps_)));
    if (frameDuration_.microseconds() <= 0) {
        frameDuration_ = MediaTime::fromMicroseconds(40'000);
    }
}

qint64 SnapEngine::thresholdUs(const TimelineViewport &viewport) const
{
    const double pixelsPerMs = std::max(0.0001, viewport.pixelsPerMs());
    return static_cast<qint64>(std::llround(kSnapPixels / pixelsPerMs * 1000.0));
}

MediaTime SnapEngine::snap(
    MediaTime value,
    const TimelineViewport &viewport,
    MediaTime playhead,
    const std::optional<MediaTime> &inPoint,
    const std::optional<MediaTime> &outPoint,
    const QList<Subtitle> &cues,
    const QString &excludeId) const
{
    const qint64 durationUs = viewport.timelineDuration().microseconds();
    qint64 chosen = std::max<qint64>(0, std::min(durationUs, value.microseconds()));
    if (!enabled_) {
        return MediaTime::fromMicroseconds(chosen);
    }

    QVector<qint64> targets;
    const qint64 frameUs = std::max<qint64>(1, frameDuration_.microseconds());
    targets.push_back(std::llround(static_cast<double>(chosen) / static_cast<double>(frameUs)) * frameUs);
    targets.push_back(playhead.microseconds());
    if (inPoint) {
        targets.push_back(inPoint->microseconds());
    }
    if (outPoint) {
        targets.push_back(outPoint->microseconds());
    }
    for (const Subtitle &cue : cues) {
        if (!cue.isTimed() || cue.id == excludeId) {
            continue;
        }
        targets.push_back(cue.start.microseconds());
        targets.push_back(cue.end.microseconds());
    }

    qint64 nearest = targets.front();
    qint64 bestDistance = std::llabs(nearest - chosen);
    for (qint64 target : targets) {
        const qint64 distance = std::llabs(target - chosen);
        if (distance < bestDistance) {
            bestDistance = distance;
            nearest = target;
        }
    }

    if (bestDistance <= thresholdUs(viewport)) {
        chosen = std::max<qint64>(0, std::min(durationUs, nearest));
    }
    return MediaTime::fromMicroseconds(chosen);
}

} // namespace subcue
