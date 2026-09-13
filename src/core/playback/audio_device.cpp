#include "playback/audio_device.h"

#include <QtCore/QVector>

extern "C" {
#include <libavutil/mathematics.h>
}

#ifdef Q_OS_WIN
#include "platform/windows/wasapi_audio_device.h"
#endif

#include <algorithm>
#include <utility>

namespace subcue {

QString VirtualAudioDevice::backendId() const
{
    return QStringLiteral("virtual");
}

int VirtualAudioDevice::preferredSampleRate() const
{
    return 0;
}

bool VirtualAudioDevice::start(int sampleRate, int channels, AppError *error)
{
    if (sampleRate <= 0 || channels <= 0) {
        if (error) {
            *error = AppError(ErrorDomain::Media, 1, QStringLiteral("虚拟音频输出参数无效"));
        }
        return false;
    }
    sampleRate_ = sampleRate;
    channels_ = channels;
    paused_ = false;
    started_ = true;
    timer_.start();
    return true;
}

void VirtualAudioDevice::stop()
{
    started_ = false;
    paused_ = false;
    provider_ = {};
}

void VirtualAudioDevice::pause()
{
    paused_ = true;
}

void VirtualAudioDevice::resume()
{
    if (!started_) {
        return;
    }
    paused_ = false;
    timer_.start();
}

void VirtualAudioDevice::setPlaybackRate(double rate)
{
    playbackRate_ = std::clamp(rate, 0.5, 3.0);
}

void VirtualAudioDevice::setSampleProvider(AudioSampleProvider provider)
{
    provider_ = std::move(provider);
}

void VirtualAudioDevice::flushResampler()
{
}

qint64 VirtualAudioDevice::renderFrame()
{
    if (!started_ || paused_ || !provider_) {
        if (timer_.isValid()) {
            timer_.start();
        }
        return 0;
    }
    const qint64 microseconds = timer_.nsecsElapsed() / 1'000;
    timer_.start();
    if (microseconds <= 0 || sampleRate_ <= 0) {
        return 0;
    }
    // 没有真实声卡：按真实时间取走等量解码数据，让时钟照常推进。
    const qint64 frames = static_cast<qint64>(
        av_rescale(microseconds, sampleRate_, 1'000'000) * playbackRate_);
    const qint64 requested = std::min<qint64>(frames, sampleRate_);
    if (requested <= 0) {
        return 0;
    }
    QVector<float> scratch(static_cast<qsizetype>(requested) * channels_);
    const qint64 pulled = provider_(scratch.data(), requested);
    return std::clamp<qint64>(pulled, 0, requested);
}

bool VirtualAudioDevice::isStarted() const
{
    return started_;
}

bool VirtualAudioDevice::isHardware() const
{
    return false;
}

std::unique_ptr<IAudioDevice> createAudioDevice(AudioDeviceKind kind, AppError *error)
{
    Q_UNUSED(error);
    if (kind == AudioDeviceKind::Virtual) {
        return std::make_unique<VirtualAudioDevice>();
    }

#ifdef Q_OS_WIN
    if (kind == AudioDeviceKind::Wasapi || kind == AudioDeviceKind::Auto) {
        return std::make_unique<WasapiAudioDevice>();
    }
#else
    Q_UNUSED(error);
    if (kind == AudioDeviceKind::Wasapi && error) {
        *error = AppError(ErrorDomain::Media, 2, QStringLiteral("当前平台不支持 WASAPI"));
    }
#endif

    return std::make_unique<VirtualAudioDevice>();
}

} // namespace subcue
