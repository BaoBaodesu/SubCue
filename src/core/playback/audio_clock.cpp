#include "playback/audio_clock.h"

#include "playback/audio_output.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace subcue {

void AudioClock::reset()
{
    firstPts_ = MediaTime::fromMicroseconds(-1);
    pausedAt_ = MediaTime::fromMicroseconds(0);
    sampleRate_ = 0;
    writtenSamples_ = 0;
    bufferedSamples_ = 0;
    started_ = false;
    paused_ = false;
}

void AudioClock::start(MediaTime firstPts, int sampleRate)
{
    firstPts_ = firstPts;
    sampleRate_ = sampleRate;
    writtenSamples_ = 0;
    bufferedSamples_ = 0;
    pausedAt_ = firstPts;
    started_ = sampleRate > 0 && firstPts.microseconds() >= 0;
    paused_ = false;
}

void AudioClock::pause()
{
    if (!started_ || paused_) {
        return;
    }
    pausedAt_ = now();
    paused_ = true;
}

void AudioClock::resume()
{
    paused_ = false;
}

void AudioClock::setWrittenSamples(qint64 samples) noexcept
{
    writtenSamples_ = samples < 0 ? 0 : samples;
}

void AudioClock::setBufferedSamples(qint64 samples) noexcept
{
    bufferedSamples_ = samples < 0 ? 0 : samples;
}

void AudioClock::syncFrom(const AudioOutput &output)
{
    if (!output.sampleRate()) {
        return;
    }
    const bool stayPaused = paused_ || output.isPaused();
    if (!started_ || firstPts_.microseconds() < 0) {
        start(output.firstPts(), output.sampleRate());
    }
    setWrittenSamples(output.writtenSamples());
    setBufferedSamples(output.bufferedSamples());
    if (stayPaused) {
        pause();
    } else {
        resume();
    }
}

qint64 AudioClock::consumedSamples() const noexcept
{
    if (writtenSamples_ <= bufferedSamples_) {
        return 0;
    }
    return writtenSamples_ - bufferedSamples_;
}

MediaTime AudioClock::now() const noexcept
{
    if (!started_ || sampleRate_ <= 0) {
        return MediaTime::fromMicroseconds(-1);
    }
    if (paused_) {
        return pausedAt_;
    }
    const qint64 audible = consumedSamples();
    const qint64 offset = av_rescale(audible, 1'000'000, sampleRate_);
    return MediaTime::fromMicroseconds(firstPts_.microseconds() + offset);
}

} // namespace subcue
