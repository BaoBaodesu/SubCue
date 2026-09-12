#pragma once

#include "playback/audio_device.h"

#include <memory>

namespace subcue {

class WasapiAudioDevice final : public IAudioDevice {
public:
    WasapiAudioDevice();
    ~WasapiAudioDevice() override;

    [[nodiscard]] QString backendId() const override;
    [[nodiscard]] bool probe(AppError *error = nullptr);
    [[nodiscard]] bool start(int sampleRate, int channels, AppError *error = nullptr) override;
    void stop() override;
    void pause() override;
    void resume() override;
    qint64 render(AudioOutput &output) override;
    [[nodiscard]] bool isStarted() const override;
    [[nodiscard]] bool isHardware() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace subcue
