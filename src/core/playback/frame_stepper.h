#pragma once

#include "common/app_error.h"
#include "common/media_time.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/video_frame_converter.h"
#include "playback/frame_index.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtGui/QImage>

#include <deque>

namespace subcue {

struct SteppedFrame final {
    int index = -1;
    MediaTime pts = MediaTime::fromMicroseconds(-1);
    QImage image;
};

class FrameStepper final {
public:
    static constexpr int kHistoryRingSize = 8;

    [[nodiscard]] bool open(const QString &path, AppError *error = nullptr);
    void close();

    [[nodiscard]] bool seekTo(MediaTime pts, AppError *error = nullptr);
    [[nodiscard]] bool stepForward(AppError *error = nullptr);
    [[nodiscard]] bool stepBackward(AppError *error = nullptr);

    [[nodiscard]] bool isOpen() const noexcept { return current_.index >= 0; }
    [[nodiscard]] const FrameIndex &index() const noexcept { return index_; }
    [[nodiscard]] SteppedFrame current() const { return current_; }
    [[nodiscard]] MediaTime currentPts() const { return current_.pts; }
    [[nodiscard]] int currentIndex() const noexcept { return current_.index; }
    [[nodiscard]] int gopSize() const noexcept { return gop_.size(); }
    [[nodiscard]] int historySize() const noexcept { return static_cast<int>(history_.size()); }

private:
    [[nodiscard]] bool decodeGopUntil(int targetIndex, AppError *error);
    void rememberCurrent();
    [[nodiscard]] bool showIndex(int frameIndex, AppError *error);

    FrameIndex index_;
    Demuxer demuxer_;
    Decoder decoder_;
    VideoFrameConverter converter_;
    int videoStreamIndex_ = -1;
    AVRational timeBase_{0, 1};
    int gopStartIndex_ = -1;
    QVector<SteppedFrame> gop_;
    std::deque<SteppedFrame> history_;
    SteppedFrame current_;
};

} // namespace subcue
