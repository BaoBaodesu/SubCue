#pragma once

#include "playback/audio_device.h"

#include "media/audio_resampler.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QVector>

#include <atomic>
#include <memory>
#include <thread>

namespace subcue {

class WasapiAudioDevice final : public IAudioDevice {
public:
    WasapiAudioDevice();
    ~WasapiAudioDevice() override;

    [[nodiscard]] QString backendId() const override;
    [[nodiscard]] bool probe(AppError *error = nullptr);
    [[nodiscard]] int preferredSampleRate() const override;
    [[nodiscard]] bool start(int sampleRate, int channels, AppError *error = nullptr) override;
    void stop() override;
    void pause() override;
    void resume() override;
    void setPlaybackRate(double rate) override;
    void setSampleProvider(AudioSampleProvider provider) override;
    [[nodiscard]] qint64 renderFrame() override;
    // 跳转后丢弃重采样器里属于旧位置的残留样本，避免边界爆音。
    void flushResampler();
    [[nodiscard]] bool isStarted() const override;
    [[nodiscard]] bool isHardware() const override;

private:
    struct Impl;

    void audioLoop();
    [[nodiscard]] qint64 fillBuffer();
    [[nodiscard]] qint64 fillBufferDirect(unsigned char *destination, unsigned int available);
    [[nodiscard]] qint64 fillBufferResampled(unsigned char *destination, unsigned int available);
    void applyFadeIn(QVector<float> &samples, qint64 frames, int channels);
    void resetResampler();
    void joinWorker();

    std::unique_ptr<Impl> impl_;
    SwrContextPtr resampler_;
    std::atomic<quint64> flushRequest_{0};
    quint64 appliedFlush_ = 0;
    std::thread thread_;
};

} // namespace subcue
