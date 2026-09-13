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
    // 设备混音采样率；0 表示没有固定要求，按调用方给出的采样率启动。
    [[nodiscard]] virtual int preferredSampleRate() const = 0;
    [[nodiscard]] virtual bool start(int sampleRate, int channels, AppError *error = nullptr) = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void setPlaybackRate(double rate) = 0;
    // 注册缺帧时的数据来源；未注册时 renderFrame 只输出静音。
    virtual void setSampleProvider(AudioSampleProvider provider) = 0;
    // 丢弃设备侧转换状态（跳转后必须调用，否则残留样本会接在旧位置后面）。
    virtual void flushResampler() = 0;
    // 取一次音频数据按设备混音率补齐并转成设备格式，返回写入帧数。
    [[nodiscard]] virtual qint64 renderFrame() = 0;
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
    [[nodiscard]] int preferredSampleRate() const override;
    [[nodiscard]] bool start(int sampleRate, int channels, AppError *error = nullptr) override;
    void stop() override;
    void pause() override;
    void resume() override;
    void setPlaybackRate(double rate) override;
    void setSampleProvider(AudioSampleProvider provider) override;
    void flushResampler() override;
    [[nodiscard]] qint64 renderFrame() override;
    [[nodiscard]] bool isStarted() const override;
    [[nodiscard]] bool isHardware() const override;

private:
    QElapsedTimer timer_;
    AudioSampleProvider provider_;
    int sampleRate_ = 0;
    int channels_ = 0;
    double playbackRate_ = 1.0;
    bool started_ = false;
    bool paused_ = false;
};

[[nodiscard]] std::unique_ptr<IAudioDevice> createAudioDevice(
    AudioDeviceKind kind, AppError *error = nullptr);

} // namespace subcue
