#pragma once

#include "common/app_error.h"
#include "playback/audio_output.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QString>

#include <memory>

namespace subcue {

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    [[nodiscard]] virtual QString backendId() const = 0;
    [[nodiscard]] virtual bool start(int sampleRate, int channels, AppError *error = nullptr) = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual qint64 render(AudioOutput &output) = 0;
    [[nodiscard]] virtual bool isStarted() const = 0;
    [[nodiscard]] virtual bool isHardware() const = 0;
};

enum class AudioDeviceKind {
    Virtual,
    Wasapi,
    Auto
};

class VirtualAudioDevice final : public IAudioDevice {
public:
    [[nodiscard]] QString backendId() const override;
    [[nodiscard]] bool start(int sampleRate, int channels, AppError *error = nullptr) override;
    void stop() override;
    void pause() override;
    void resume() override;
    qint64 render(AudioOutput &output) override;
    [[nodiscard]] bool isStarted() const override;
    [[nodiscard]] bool isHardware() const override;

private:
    QElapsedTimer timer_;
    int sampleRate_ = 0;
    int channels_ = 0;
    bool started_ = false;
    bool paused_ = false;
};

[[nodiscard]] std::unique_ptr<IAudioDevice> createAudioDevice(
    AudioDeviceKind kind, AppError *error = nullptr);

} // namespace subcue
