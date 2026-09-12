#include "playback/audio_device.h"

#ifdef Q_OS_WIN
#include "platform/windows/wasapi_audio_device.h"
#endif

namespace subcue {

QString VirtualAudioDevice::backendId() const
{
    return QStringLiteral("virtual");
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

qint64 VirtualAudioDevice::render(AudioOutput &output)
{
    if (!started_ || paused_) {
        if (timer_.isValid()) {
            timer_.start();
        }
        return 0;
    }
    const qint64 microseconds = timer_.nsecsElapsed() / 1'000;
    timer_.start();
    if (microseconds <= 0) {
        return 0;
    }
    return output.consumeDuration(MediaTime::fromMicroseconds(microseconds));
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
