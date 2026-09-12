#include "playback/audio_output.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>

namespace subcue {

bool AudioOutput::configure(int sampleRate, int channels, AppError *error)
{
    if (sampleRate <= 0 || channels <= 0) {
        if (error) {
            *error = AppError(ErrorDomain::Media, AVERROR(EINVAL), QStringLiteral("音频输出参数无效"));
        }
        return false;
    }
    std::lock_guard lock(mutex_);
    sampleRate_ = sampleRate;
    channels_ = channels;
    buffer_.clear();
    firstPts_ = MediaTime::fromMicroseconds(-1);
    writtenFrames_ = 0;
    consumedFrames_ = 0;
    paused_ = false;
    return true;
}

void AudioOutput::setGeneration(quint64 generation)
{
    std::lock_guard lock(mutex_);
    generation_ = generation;
    buffer_.clear();
    firstPts_ = MediaTime::fromMicroseconds(-1);
    writtenFrames_ = 0;
    consumedFrames_ = 0;
}

void AudioOutput::flush()
{
    std::lock_guard lock(mutex_);
    buffer_.clear();
    firstPts_ = MediaTime::fromMicroseconds(-1);
    writtenFrames_ = 0;
    consumedFrames_ = 0;
}

void AudioOutput::pause()
{
    std::lock_guard lock(mutex_);
    paused_ = true;
}

void AudioOutput::resume()
{
    std::lock_guard lock(mutex_);
    paused_ = false;
}

bool AudioOutput::write(QVector<float> samples, MediaTime pts, quint64 generation)
{
    std::lock_guard lock(mutex_);
    if (generation != generation_ || channels_ <= 0 || samples.isEmpty()) {
        return false;
    }
    if (samples.size() % channels_ != 0) {
        return false;
    }
    if (firstPts_.microseconds() < 0) {
        firstPts_ = pts;
    }
    const qint64 frames = static_cast<qint64>(samples.size() / channels_);
    buffer_ += samples;
    writtenFrames_ += frames;
    return true;
}

QVector<float> AudioOutput::takeFrames(qint64 frames)
{
    std::lock_guard lock(mutex_);
    if (paused_ || frames <= 0 || channels_ <= 0) {
        return {};
    }
    const qint64 bufferedFrames = static_cast<qint64>(buffer_.size() / channels_);
    const qint64 consumed = std::min(frames, bufferedFrames);
    if (consumed <= 0) {
        return {};
    }
    const qsizetype sampleCount = static_cast<qsizetype>(consumed * channels_);
    QVector<float> taken(buffer_.cbegin(), buffer_.cbegin() + sampleCount);
    buffer_.remove(0, sampleCount);
    consumedFrames_ += consumed;
    return taken;
}

qint64 AudioOutput::consumeFrames(qint64 frames)
{
    const int channels = this->channels();
    if (channels <= 0) {
        return 0;
    }
    return static_cast<qint64>(takeFrames(frames).size()) / channels;
}

qint64 AudioOutput::consumeDuration(MediaTime duration)
{
    int sampleRate = 0;
    {
        std::lock_guard lock(mutex_);
        sampleRate = sampleRate_;
    }
    if (sampleRate <= 0 || duration.microseconds() <= 0) {
        return 0;
    }
    return consumeFrames(av_rescale(duration.microseconds(), sampleRate, 1'000'000));
}

quint64 AudioOutput::generation() const
{
    std::lock_guard lock(mutex_);
    return generation_;
}

int AudioOutput::sampleRate() const
{
    std::lock_guard lock(mutex_);
    return sampleRate_;
}

int AudioOutput::channels() const
{
    std::lock_guard lock(mutex_);
    return channels_;
}

bool AudioOutput::isPaused() const
{
    std::lock_guard lock(mutex_);
    return paused_;
}

MediaTime AudioOutput::firstPts() const
{
    std::lock_guard lock(mutex_);
    return firstPts_;
}

qint64 AudioOutput::writtenSamples() const
{
    std::lock_guard lock(mutex_);
    return writtenFrames_;
}

qint64 AudioOutput::bufferedSamples() const
{
    std::lock_guard lock(mutex_);
    return channels_ <= 0 ? 0 : static_cast<qint64>(buffer_.size() / channels_);
}

qint64 AudioOutput::consumedSamples() const
{
    std::lock_guard lock(mutex_);
    return consumedFrames_;
}

} // namespace subcue
