#pragma once

#include "common/media_time.h"
#include "subtitle/subtitle.h"
#include "timeline/timeline_viewport.h"

#include <QtCore/QList>
#include <QtCore/QString>

#include <optional>

namespace subcue {

class SnapEngine final {
public:
    static constexpr double kSnapPixels = 8.0;
    static constexpr double kDefaultFps = 25.0;

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

    void setFps(double fps);
    [[nodiscard]] double fps() const noexcept { return fps_; }
    [[nodiscard]] MediaTime frameDuration() const noexcept { return frameDuration_; }

    [[nodiscard]] qint64 thresholdUs(const TimelineViewport &viewport) const;

    [[nodiscard]] MediaTime snap(
        MediaTime value,
        const TimelineViewport &viewport,
        MediaTime playhead,
        const std::optional<MediaTime> &inPoint,
        const std::optional<MediaTime> &outPoint,
        const QList<Subtitle> &cues,
        const QString &excludeId = {}) const;

private:
    bool enabled_ = true;
    double fps_ = kDefaultFps;
    MediaTime frameDuration_ = MediaTime::fromMicroseconds(40'000);
};

} // namespace subcue
