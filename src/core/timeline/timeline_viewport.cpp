#include "timeline/timeline_viewport.h"

#include <QtCore/QString>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>

namespace subcue {
namespace {

double clampDouble(double value, double minimum, double maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

qint64 clampUs(qint64 value, qint64 minimum, qint64 maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

} // namespace

void TimelineViewport::setDuration(MediaTime duration)
{
    duration_ = duration.microseconds() < 0 ? MediaTime::fromMicroseconds(0) : duration;
    playhead_ = MediaTime::fromMicroseconds(clampUs(playhead_.microseconds(), 0, timelineDuration().microseconds()));
}

void TimelineViewport::setHasMedia(bool hasMedia) noexcept
{
    hasMedia_ = hasMedia;
}

void TimelineViewport::setPlayhead(MediaTime playhead)
{
    playhead_ = MediaTime::fromMicroseconds(clampUs(playhead.microseconds(), 0, timelineDuration().microseconds()));
}

MediaTime TimelineViewport::timelineDuration() const noexcept
{
    if (duration_.microseconds() > 0) {
        return duration_;
    }
    return MediaTime::fromMilliseconds(kEmptyTimelineMs);
}

void TimelineViewport::setPixelsPerMs(double pixelsPerMs)
{
    if (std::isfinite(pixelsPerMs) && pixelsPerMs > 0.0) {
        pixelsPerMs_ = pixelsPerMs;
    }
}

int TimelineViewport::zoomPercent() const noexcept
{
    return qRound(pixelsPerMs_ / kDefaultPixelsPerMs * 100.0);
}

void TimelineViewport::setScrollOffset(double pixels, double viewportWidth)
{
    scrollOffset_ = pixels;
    clampScroll(viewportWidth);
}

void TimelineViewport::zoomBy(double factor, double viewportWidth)
{
    applyZoom(pixelsPerMs_ * factor, viewportWidth);
}

void TimelineViewport::setZoomPercent(int percent, double viewportWidth)
{
    percent = std::max(0, std::min(kMaxZoomPercent, percent));
    applyZoom(static_cast<double>(percent) / 1000.0, viewportWidth);
}

void TimelineViewport::adjustZoomPercent(int delta, double viewportWidth)
{
    setZoomPercent(zoomPercent() + delta, viewportWidth);
}

void TimelineViewport::fit(double viewportWidth)
{
    const double width = std::max(1.0, viewportWidth);
    if (!hasMedia_) {
        pixelsPerMs_ = kDefaultPixelsPerMs;
    } else {
        const double durationMs = std::max(1.0, static_cast<double>(timelineDuration().milliseconds()));
        pixelsPerMs_ = width / durationMs;
    }
    scrollOffset_ = 0.0;
}

void TimelineViewport::scrollBy(double deltaPixels, double viewportWidth)
{
    scrollOffset_ += deltaPixels;
    clampScroll(viewportWidth);
}

void TimelineViewport::setVisibleRange(double startRatio, double endRatio, double viewportWidth)
{
    if (!std::isfinite(startRatio) || !std::isfinite(endRatio) || viewportWidth <= 0.0) {
        return;
    }
    const double durationMs = timelineDuration().microseconds() / 1000.0;
    const double minimum = std::min(1.0, viewportWidth / (kMaxPixelsPerMs * durationMs));
    startRatio = clampDouble(startRatio, 0.0, 1.0 - minimum);
    endRatio = clampDouble(endRatio, startRatio + minimum, 1.0);
    pixelsPerMs_ = viewportWidth / ((endRatio - startRatio) * durationMs);
    scrollOffset_ = startRatio * durationMs * pixelsPerMs_;
    clampScroll(viewportWidth);
}

bool TimelineViewport::followPlayback(int direction, double viewportWidth)
{
    if (viewportWidth <= 0.0 || direction == 0) return false;
    const double x = xAtTime(playhead_);
    const double previous = scrollOffset_;
    if (direction > 0 && (x >= viewportWidth * 0.90 || x < 0.0)) {
        scrollBy(x - viewportWidth * 0.10, viewportWidth);
    } else if (direction < 0 && (x <= viewportWidth * 0.10 || x > viewportWidth)) {
        scrollBy(x - viewportWidth * 0.90, viewportWidth);
    }
    return previous != scrollOffset_;
}

void TimelineViewport::ensureTimeVisible(MediaTime time, double viewportWidth)
{
    const double width = std::max(0.0, viewportWidth);
    const double x = time.microseconds() / 1000.0 * pixelsPerMs_;
    if (x < scrollOffset_) {
        scrollOffset_ = std::max(0.0, x - width * 0.1);
    } else if (x > scrollOffset_ + width) {
        scrollOffset_ = std::max(0.0, x - width * 0.9);
    }
    clampScroll(viewportWidth);
}

MediaTime TimelineViewport::timeAtX(double x) const
{
    const double ms = (x + scrollOffset_) / pixelsPerMs_;
    const qint64 us = static_cast<qint64>(std::llround(ms * 1000.0));
    return MediaTime::fromMicroseconds(clampUs(us, 0, timelineDuration().microseconds()));
}

double TimelineViewport::xAtTime(MediaTime time) const
{
    return time.microseconds() / 1000.0 * pixelsPerMs_ - scrollOffset_;
}

MediaTime TimelineViewport::visibleStart() const
{
    return timeAtX(0.0);
}

MediaTime TimelineViewport::visibleEnd(double viewportWidth) const
{
    return timeAtX(std::max(0.0, viewportWidth));
}

bool TimelineViewport::rangeOverlapsViewport(MediaTime start, MediaTime end, double viewportWidth) const
{
    return end.microseconds() > visibleStart().microseconds()
        && start.microseconds() < visibleEnd(viewportWidth).microseconds();
}

double TimelineViewport::contentWidth() const
{
    return timelineDuration().milliseconds() * pixelsPerMs_;
}

double TimelineViewport::maxScrollOffset(double viewportWidth) const
{
    return std::max(0.0, contentWidth() - std::max(0.0, viewportWidth));
}

qint64 TimelineViewport::rulerIntervalMs() const
{
    static constexpr qint64 kIntervals[] = {
        40, 100, 200, 500, 1'000, 2'000, 5'000, 10'000, 30'000, 60'000, 120'000, 300'000, 600'000};
    const double target = 70.0 / std::max(0.0001, pixelsPerMs_);
    for (qint64 interval : kIntervals) {
        if (static_cast<double>(interval) >= target) {
            return interval;
        }
    }
    return kIntervals[std::size(kIntervals) - 1];
}

QVector<qint64> TimelineViewport::rulerTickMs(double viewportWidth) const
{
    const qint64 step = rulerIntervalMs();
    const qint64 durationMs = timelineDuration().milliseconds();
    const double startMs = visibleStart().milliseconds();
    qint64 first = static_cast<qint64>(std::floor(startMs / static_cast<double>(step))) * step;
    if (first < 0) {
        first = 0;
    }
    QVector<qint64> ticks;
    for (qint64 ms = first; ms <= durationMs; ms += step) {
        const double x = xAtTime(MediaTime::fromMilliseconds(ms));
        if (x > viewportWidth) {
            break;
        }
        if (x >= 0.0) {
            ticks.push_back(ms);
        }
    }
    return ticks;
}

QString TimelineViewport::formatRulerLabel(qint64 ms, qint64 intervalMs)
{
    ms = std::max<qint64>(0, ms);
    intervalMs = std::max<qint64>(1, intervalMs);
    const qint64 hours = ms / 3'600'000;
    const qint64 minutes = (ms % 3'600'000) / 60'000;
    const qint64 seconds = (ms % 60'000) / 1'000;
    const qint64 millis = ms % 1'000;
    QString text;
    if (hours > 0) {
        text = QStringLiteral("%1:%2:%3")
                   .arg(hours)
                   .arg(minutes, 2, 10, QLatin1Char('0'))
                   .arg(seconds, 2, 10, QLatin1Char('0'));
    } else {
        text = QStringLiteral("%1:%2")
                   .arg(minutes, 2, 10, QLatin1Char('0'))
                   .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    if (intervalMs < 100) {
        text += QStringLiteral(".%1").arg(millis, 3, 10, QLatin1Char('0'));
    } else if (intervalMs < 1000) {
        text += QStringLiteral(".%1").arg(millis / 100);
    }
    return text;
}

void TimelineViewport::clampScroll(double viewportWidth)
{
    scrollOffset_ = clampDouble(scrollOffset_, 0.0, maxScrollOffset(viewportWidth));
}

void TimelineViewport::applyZoom(double pixelsPerMs, double viewportWidth)
{
    const double width = std::max(1.0, viewportWidth);
    const double playheadX = playhead_.microseconds() / 1000.0 * pixelsPerMs_;
    const double anchor = playheadX - scrollOffset_;
    const double minimum = width / (timelineDuration().microseconds() / 1000.0);
    pixelsPerMs_ = clampDouble(pixelsPerMs, minimum, std::max(minimum, kMaxPixelsPerMs));
    const double clampedAnchor = clampDouble(anchor, 0.0, width);
    scrollOffset_ = std::max(0.0, playhead_.microseconds() / 1000.0 * pixelsPerMs_ - clampedAnchor);
    clampScroll(viewportWidth);
}

} // namespace subcue
