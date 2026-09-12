#pragma once

#include "common/app_error.h"
#include "common/media_time.h"
#include "media/ffmpeg_raii.h"

#include <atomic>
#include <mutex>

namespace subcue {

class Decoder;
class Demuxer;

class SeekController final {
public:
    quint64 request(MediaTime target);
    void reset();
    [[nodiscard]] quint64 generation() const noexcept;
    [[nodiscard]] MediaTime target() const;
    [[nodiscard]] bool canCommit(quint64 generation) const noexcept;
    [[nodiscard]] bool isPending() const;
    [[nodiscard]] bool takePending(quint64 *generation, MediaTime *target);

    [[nodiscard]] bool executeKeyframeSeek(
        Demuxer &demuxer,
        Decoder &decoder,
        int streamIndex,
        AVRational timeBase,
        quint64 generation,
        AppError *error = nullptr);

    FramePtr decodeToTarget(
        Demuxer &demuxer,
        Decoder &decoder,
        int streamIndex,
        AVRational timeBase,
        quint64 generation,
        AppError *error = nullptr);

private:
    mutable std::mutex mutex_;
    std::atomic<quint64> generation_{0};
    MediaTime target_ = MediaTime::fromMicroseconds(0);
    bool pending_ = false;
};

} // namespace subcue
