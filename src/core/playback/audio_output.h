#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QVector>

#include <functional>
#include <mutex>

namespace subcue {

// 设备回调：把最多 maximumFrames 帧、outputSampleRate() 采样率的交错 float 样本写入 destination，返回实际写入帧数。
using AudioSampleProvider = std::function<qint64(float *destination, qint64 maximumFrames)>;

class AudioOutput final {
public:
    // 预泵入上限：解码数据先囤到约 500ms，音频线程才有稳定余量，不会每次缺帧都硬切静音。
    static constexpr int kMaximumBufferMilliseconds = 500;

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
    [[nodiscard]] qint64 bufferedFrames() const;
    [[nodiscard]] qint64 consumedSamples() const;

private:
    [[nodiscard]] qint64 bufferedFramesLocked() const;
    void reserveLocked();

    mutable std::mutex mutex_;
    QVector<float> buffer_;
    qsizetype reservedSamples_ = 0;
    MediaTime firstPts_ = MediaTime::fromMicroseconds(-1);
    quint64 generation_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    qint64 writtenFrames_ = 0;
    qint64 consumedFrames_ = 0;
    bool paused_ = false;
};

} // namespace subcue
