#pragma once

#include "common/media_time.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

class TimelineViewport final {
public:
    static constexpr double kDefaultPixelsPerMs = 0.1;
    static constexpr double kMinPixelsPerMs = 0.001;
    static constexpr double kMaxPixelsPerMs = 10.0;
    static constexpr int kMinZoomPercent = 10;
    static constexpr int kMaxZoomPercent = 10'000;
    static constexpr qint64 kEmptyTimelineMs = 60'000;

    void setDuration(MediaTime duration);
    void setHasMedia(bool hasMedia) noexcept;
    void setPlayhead(MediaTime playhead);

    [[nodiscard]] MediaTime duration() const noexcept { return duration_; }
    [[nodiscard]] MediaTime timelineDuration() const noexcept;
    [[nodiscard]] bool hasMedia() const noexcept { return hasMedia_; }
    [[nodiscard]] MediaTime playhead() const noexcept { return playhead_; }

    void setPixelsPerMs(double pixelsPerMs);
    [[nodiscard]] double pixelsPerMs() const noexcept { return pixelsPerMs_; }
    [[nodiscard]] int zoomPercent() const noexcept;

    void setScrollOffset(double pixels, double viewportWidth);
    [[nodiscard]] double scrollOffset() const noexcept { return scrollOffset_; }

    void zoomBy(double factor, double viewportWidth);
    void setZoomPercent(int percent, double viewportWidth);
    void adjustZoomPercent(int delta, double viewportWidth);
    void fit(double viewportWidth);
    void setVisibleRange(double startRatio, double endRatio, double viewportWidth);
    void scrollBy(double deltaPixels, double viewportWidth);
    void ensureTimeVisible(MediaTime time, double viewportWidth);
    bool followPlayback(int direction, double viewportWidth);

    [[nodiscard]] MediaTime timeAtX(double x) const;
    [[nodiscard]] double xAtTime(MediaTime time) const;
    [[nodiscard]] MediaTime visibleStart() const;
    [[nodiscard]] MediaTime visibleEnd(double viewportWidth) const;
    [[nodiscard]] bool rangeOverlapsViewport(MediaTime start, MediaTime end, double viewportWidth) const;
    [[nodiscard]] double contentWidth() const;
    [[nodiscard]] double maxScrollOffset(double viewportWidth) const;
    [[nodiscard]] qint64 rulerIntervalMs() const;
    [[nodiscard]] QVector<qint64> rulerTickMs(double viewportWidth) const;
    [[nodiscard]] static QString formatRulerLabel(qint64 ms, qint64 intervalMs);

private:
    void clampScroll(double viewportWidth);
    void applyZoom(double pixelsPerMs, double viewportWidth);

    MediaTime duration_{};
    MediaTime playhead_{};
    double pixelsPerMs_ = kDefaultPixelsPerMs;
    double scrollOffset_ = 0.0;
    bool hasMedia_ = false;
};

} // namespace subcue
