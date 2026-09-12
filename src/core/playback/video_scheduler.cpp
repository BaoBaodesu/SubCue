#include "playback/video_scheduler.h"

namespace subcue {

ScheduleDecision VideoScheduler::evaluate(MediaTime videoPts, MediaTime masterClock) const noexcept
{
    ScheduleDecision decision;
    if (!videoPts.isValidRange() || !masterClock.isValidRange()) {
        decision.action = FrameAction::Wait;
        return decision;
    }
    const qint64 diff = videoPts.microseconds() - masterClock.microseconds();
    if (diff > 0) {
        decision.action = FrameAction::Wait;
        decision.wakeDelay = MediaTime::fromMicroseconds(diff);
        return decision;
    }
    if (diff < -lateDropThreshold_.microseconds()) {
        decision.action = FrameAction::Drop;
        return decision;
    }
    decision.action = FrameAction::Display;
    return decision;
}

} // namespace subcue
