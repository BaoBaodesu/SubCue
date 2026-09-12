#pragma once

#include "common/media_time.h"

#include <QtCore/QtGlobal>

namespace subcue {

class AudioOutput;

class AudioClock final {
public:
    void reset();
    void start(MediaTime firstPts, int sampleRate);
    void pause();
    void resume();

    void setWrittenSamples(qint64 samples) noexcept;
    void setBufferedSamples(qint64 samples) noexcept;
    void syncFrom(const AudioOutput &output);

    [[nodiscard]] MediaTime now() const noexcept;
    [[nodiscard]] bool isPaused() const noexcept { return paused_; }
    [[nodiscard]] bool isStarted() const noexcept { return started_; }
    [[nodiscard]] MediaTime firstPts() const noexcept { return firstPts_; }
    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] qint64 writtenSamples() const noexcept { return writtenSamples_; }
    [[nodiscard]] qint64 bufferedSamples() const noexcept { return bufferedSamples_; }
    [[nodiscard]] qint64 consumedSamples() const noexcept;

private:
    MediaTime firstPts_ = MediaTime::fromMicroseconds(-1);
    MediaTime pausedAt_ = MediaTime::fromMicroseconds(0);
    int sampleRate_ = 0;
    qint64 writtenSamples_ = 0;
    qint64 bufferedSamples_ = 0;
    bool started_ = false;
    bool paused_ = false;
};

} // namespace subcue
