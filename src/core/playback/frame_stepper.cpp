#include "playback/frame_stepper.h"

#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"

#include <utility>

namespace subcue {

bool FrameStepper::open(const QString &path, AppError *error)
{
    close();
    if (!index_.build(path, error)) {
        return false;
    }
    if (!demuxer_.open(path, error)) {
        close();
        return false;
    }
    videoStreamIndex_ = demuxer_.bestStream(AVMEDIA_TYPE_VIDEO);
    const AVStream *stream = demuxer_.stream(videoStreamIndex_);
    if (!stream) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, videoStreamIndex_, QStringLiteral("媒体中没有可逐帧解码的视频流"));
        }
        close();
        return false;
    }
    timeBase_ = stream->time_base;
    if (!decoder_.open(*stream, error)) {
        close();
        return false;
    }
    return showIndex(0, error);
}

void FrameStepper::close()
{
    decoder_.close();
    demuxer_.close();
    index_.clear();
    gop_.clear();
    history_.clear();
    current_ = {};
    gopStartIndex_ = -1;
    videoStreamIndex_ = -1;
    timeBase_ = {0, 1};
}

bool FrameStepper::seekTo(MediaTime pts, AppError *error)
{
    if (index_.isEmpty()) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("尚未建立逐帧索引"));
        }
        return false;
    }
    history_.clear();
    return showIndex(index_.findIndexAtOrAfter(pts), error);
}

bool FrameStepper::stepForward(AppError *error)
{
    if (current_.index < 0 || current_.index + 1 >= index_.size()) {
        return false;
    }
    rememberCurrent();
    return showIndex(current_.index + 1, error);
}

bool FrameStepper::stepBackward(AppError *error)
{
    if (current_.index <= 0) {
        return false;
    }
    const int previous = current_.index - 1;
    if (!history_.empty() && history_.back().index == previous) {
        current_ = history_.back();
        history_.pop_back();
        return true;
    }
    return showIndex(previous, error);
}

bool FrameStepper::decodeGopUntil(int targetIndex, AppError *error)
{
    const int key = index_.keyframeIndexAtOrBefore(targetIndex);
    if (key < 0) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("找不到关键帧"));
        }
        return false;
    }
    if (gopStartIndex_ == key) {
        for (const SteppedFrame &frame : gop_) {
            if (frame.index == targetIndex) {
                return true;
            }
        }
    }

    if (!demuxer_.seek(videoStreamIndex_, index_.at(key).seekTimestamp, error)) {
        return false;
    }
    decoder_.flush();
    gop_.clear();
    gopStartIndex_ = key;

    FramePtr frame = makeFrame();
    if (!frame) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频帧"));
        }
        return false;
    }

    const MediaTime targetPts = index_.at(targetIndex).pts;
    while (gop_.isEmpty() || gop_.constLast().index < targetIndex) {
        if (!decoder_.pullFrame(demuxer_, videoStreamIndex_, frame.get(), error)) {
            break;
        }
        SteppedFrame stepped;
        stepped.pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, timeBase_);
        stepped.index = gopStartIndex_ + gop_.size();
        for (int index = gopStartIndex_; index < index_.size(); ++index) {
            if (qAbs(index_.at(index).pts.microseconds() - stepped.pts.microseconds()) <= 1'000) {
                stepped.index = index;
                break;
            }
        }
        stepped.image = converter_.convert(*frame, {}, error);
        av_frame_unref(frame.get());
        if (stepped.image.isNull()) {
            return false;
        }
        gop_.push_back(std::move(stepped));
        if (gop_.constLast().pts.microseconds() >= targetPts.microseconds()
            && gop_.constLast().index >= targetIndex) {
            break;
        }
        if (gop_.size() > index_.size() + 2) {
            break;
        }
    }

    for (const SteppedFrame &cached : gop_) {
        if (cached.index == targetIndex) {
            return true;
        }
    }
    if (error) {
        *error = AppError(ErrorDomain::Decoder, AVERROR_EOF, QStringLiteral("无法解码目标帧"));
    }
    return false;
}

void FrameStepper::rememberCurrent()
{
    if (current_.index < 0) {
        return;
    }
    history_.push_back(current_);
    while (static_cast<int>(history_.size()) > kHistoryRingSize) {
        history_.pop_front();
    }
}

bool FrameStepper::showIndex(int frameIndex, AppError *error)
{
    if (frameIndex < 0 || frameIndex >= index_.size()) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("逐帧索引越界"));
        }
        return false;
    }
    if (!decodeGopUntil(frameIndex, error)) {
        return false;
    }
    for (const SteppedFrame &cached : gop_) {
        if (cached.index == frameIndex) {
            current_ = cached;
            return true;
        }
    }
    return false;
}

} // namespace subcue
