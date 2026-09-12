#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QVector>

#include <mutex>

namespace subcue {

class AudioOutput final {
public:
    [[nodiscard]] bool configure(int sampleRate, int channels, AppError *error = nullptr);
    void setGeneration(quint64 generation);
    void flush();
    void pause();
    void resume();

    [[nodiscard]] bool write(QVector<float> samples, MediaTime pts, quint64 generation);
    [[nodiscard]] QVector<float> takeFrames(qint64 frames);
    qint64 consumeFrames(qint64 frames);
    qint64 consumeDuration(MediaTime duration);

    [[nodiscard]] quint64 generation() const;
    [[nodiscard]] int sampleRate() const;
    [[nodiscard]] int channels() const;
    [[nodiscard]] bool isPaused() const;
    [[nodiscard]] MediaTime firstPts() const;
    [[nodiscard]] qint64 writtenSamples() const;
    [[nodiscard]] qint64 bufferedSamples() const;
    [[nodiscard]] qint64 consumedSamples() const;

private:
    mutable std::mutex mutex_;
    QVector<float> buffer_;
    MediaTime firstPts_ = MediaTime::fromMicroseconds(-1);
    quint64 generation_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    qint64 writtenFrames_ = 0;
    qint64 consumedFrames_ = 0;
    bool paused_ = false;
};

} // namespace subcue
