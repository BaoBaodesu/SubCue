#pragma once

#include "common/media_time.h"

namespace subcue {

enum class FrameAction {
    Wait,
    Display,
    Drop
};

struct ScheduleDecision final {
    FrameAction action = FrameAction::Wait;
    MediaTime wakeDelay = MediaTime::fromMicroseconds(0);
};

class VideoScheduler final {
public:
    static constexpr qint64 kDefaultLateDropMicroseconds = 40'000;

    void setLateDropThreshold(MediaTime threshold) noexcept { lateDropThreshold_ = threshold; }
    [[nodiscard]] MediaTime lateDropThreshold() const noexcept { return lateDropThreshold_; }

    [[nodiscard]] ScheduleDecision evaluate(MediaTime videoPts, MediaTime masterClock) const noexcept;

private:
    MediaTime lateDropThreshold_ = MediaTime::fromMicroseconds(kDefaultLateDropMicroseconds);
};

} // namespace subcue
