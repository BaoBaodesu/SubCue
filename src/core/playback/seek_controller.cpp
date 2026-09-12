#include "playback/seek_controller.h"

#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"

#include <utility>

namespace subcue {

quint64 SeekController::request(MediaTime target)
{
    std::lock_guard lock(mutex_);
    const quint64 generation = generation_.load(std::memory_order_relaxed) + 1;
    generation_.store(generation, std::memory_order_release);
    target_ = target;
    pending_ = true;
    return generation;
}

void SeekController::reset()
{
    std::lock_guard lock(mutex_);
    generation_.store(0, std::memory_order_release);
    target_ = MediaTime::fromMicroseconds(0);
    pending_ = false;
}

quint64 SeekController::generation() const noexcept
{
    return generation_.load(std::memory_order_acquire);
}

MediaTime SeekController::target() const
{
    std::lock_guard lock(mutex_);
    return target_;
}

bool SeekController::canCommit(quint64 generation) const noexcept
{
    return generation == generation_.load(std::memory_order_acquire);
}

bool SeekController::isPending() const
{
    std::lock_guard lock(mutex_);
    return pending_;
}

bool SeekController::takePending(quint64 *generation, MediaTime *target)
{
    std::lock_guard lock(mutex_);
    if (!pending_) {
        return false;
    }
    pending_ = false;
    if (generation) {
        *generation = generation_.load(std::memory_order_relaxed);
    }
    if (target) {
        *target = target_;
    }
    return true;
}

bool SeekController::executeKeyframeSeek(
    Demuxer &demuxer,
    Decoder &decoder,
    int streamIndex,
    AVRational timeBase,
    quint64 generation,
    AppError *error)
{
    if (!canCommit(generation)) {
        return false;
    }
    const MediaTime target = this->target();
    const qint64 timestamp = timestampFromMediaTime(target, timeBase);
    if (!demuxer.seek(streamIndex, timestamp, error)) {
        return false;
    }
    if (!canCommit(generation)) {
        return false;
    }
    decoder.flush();
    return canCommit(generation);
}

FramePtr SeekController::decodeToTarget(
    Demuxer &demuxer,
    Decoder &decoder,
    int streamIndex,
    AVRational timeBase,
    quint64 generation,
    AppError *error)
{
    FramePtr frame = makeFrame();
    if (!frame) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频帧"));
        }
        return {};
    }
    const MediaTime target = this->target();
    while (canCommit(generation)) {
        AppError decodeError(ErrorDomain::Decoder, 0, QString());
        if (!decoder.pullFrame(demuxer, streamIndex, frame.get(), &decodeError)) {
            if (error && decodeError.code() != 0) {
                *error = std::move(decodeError);
            }
            return {};
        }
        if (!canCommit(generation)) {
            return {};
        }
        const MediaTime pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, timeBase);
        if (pts.microseconds() >= target.microseconds()) {
            return frame;
        }
        av_frame_unref(frame.get());
    }
    return {};
}

} // namespace subcue
