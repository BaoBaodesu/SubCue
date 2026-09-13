#pragma once

#include "common/app_error.h"
#include "common/media_time.h"
#include "media/audio_resampler.h"
#include "media/bounded_queue.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/hw_accel.h"
#include "media/video_frame_converter.h"
#include "playback/audio_clock.h"
#include "playback/audio_device.h"
#include "playback/audio_output.h"
#include "playback/frame_stepper.h"
#include "playback/seek_controller.h"
#include "playback/video_scheduler.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace subcue {

struct DisplayedVideoFrame final {
    QImage image;
    MediaTime pts = MediaTime::fromMicroseconds(-1);
    quint64 generation = 0;
};

class PlaybackEngine final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackEngine(QObject *parent = nullptr);
    ~PlaybackEngine() override;

    PlaybackEngine(const PlaybackEngine &) = delete;
    PlaybackEngine &operator=(const PlaybackEngine &) = delete;

    [[nodiscard]] bool open(const QString &path, AppError *error = nullptr);
    void close();
    void play();
    void pause();
    quint64 seek(MediaTime target);
    [[nodiscard]] bool stepForward(AppError *error = nullptr);
    [[nodiscard]] bool stepBackward(AppError *error = nullptr);

    qint64 consumeAudio(MediaTime duration);
    void pump();
    // 绑定输出设备：主线程用它预泵入解码数据，音频线程用它取走已解码样本。
    void setAudioDevice(IAudioDevice *device, int preRollMs);
    [[nodiscard]] qint64 primeAudio(int timeoutMs = 400);
    [[nodiscard]] bool waitForDisplayedFrame(int timeoutMs);
    void requestHwRuntimeFailure();

    [[nodiscard]] AudioOutput &audioOutput() noexcept { return audioOutput_; }
    [[nodiscard]] const AudioOutput &audioOutput() const noexcept { return audioOutput_; }
    [[nodiscard]] MediaTime position() const;
    [[nodiscard]] quint64 generation() const;
    [[nodiscard]] bool isPaused() const noexcept { return paused_.load(); }
    [[nodiscard]] bool isOpen() const noexcept { return open_.load(); }
    [[nodiscard]] bool hasVideo() const noexcept { return hasVideo_.load(); }
    [[nodiscard]] bool hasAudio() const noexcept { return hasAudio_.load(); }
    [[nodiscard]] int outputSampleRate() const noexcept { return outputSampleRate_; }
    [[nodiscard]] int outputChannels() const noexcept { return outputChannels_; }
    [[nodiscard]] HwAccelType hwAccel() const noexcept;
    [[nodiscard]] QVector<HwAccelAttempt> hwAttempts() const;
    [[nodiscard]] DisplayedVideoFrame displayedFrame() const;
    [[nodiscard]] int droppedFrameCount() const noexcept { return droppedFrames_.load(); }
    [[nodiscard]] bool hwRuntimeFallbackOccurred() const noexcept { return hwRuntimeFallback_.load(); }
    [[nodiscard]] const SeekController &seekController() const noexcept { return seek_; }
    [[nodiscard]] const AudioClock &clock() const noexcept { return clock_; }
    [[nodiscard]] const FrameStepper &frameStepper() const noexcept { return stepper_; }

private:
    struct VideoItem final {
        QImage image;
        MediaTime pts = MediaTime::fromMicroseconds(-1);
        quint64 generation = 0;
    };
    struct AudioItem final {
        QVector<float> samples;
        MediaTime pts = MediaTime::fromMicroseconds(-1);
        quint64 generation = 0;
    };

    void startWorkers();
    void stopWorkers();
    void demuxLoop();
    void videoLoop();
    void audioLoop();
    void wakeDemux();
    void scheduleWake(MediaTime delay);
    void onSchedulerWake();
    void drainAudioToOutput();
    void installSampleProvider();
    void beginClock(MediaTime start);
    [[nodiscard]] bool receiveVideoFrame(quint64 generation, AVFrame *frame, AppError *error);

    SeekController seek_;
    AudioClock clock_;
    AudioOutput audioOutput_;
    IAudioDevice *audioDevice_ = nullptr;
    VideoScheduler scheduler_;
    FrameStepper stepper_;
    Demuxer demuxer_;
    Decoder videoDecoder_;
    Decoder audioDecoder_;
    AudioResampler resampler_;
    VideoFrameConverter converter_;
    QTimer wakeTimer_;

    BoundedQueue<PacketPtr> videoPackets_{128, 32 * 1024 * 1024};
    BoundedQueue<PacketPtr> audioPackets_{128, 32 * 1024 * 1024};
    BoundedQueue<VideoItem> videoFrames_{8, 64 * 1024 * 1024};
    BoundedQueue<AudioItem> audioFrames_{48, 192'000};

    std::thread demuxThread_;
    std::thread videoThread_;
    std::thread audioThread_;
    std::mutex demuxMutex_;
    std::condition_variable demuxWake_;
    mutable std::mutex stateMutex_;
    mutable std::mutex displayMutex_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> open_{false};
    std::atomic<bool> paused_{true};
    std::atomic<bool> hasVideo_{false};
    std::atomic<bool> hasAudio_{false};
    std::atomic<bool> hwRuntimeFallback_{false};
    std::atomic<bool> forceHwFailure_{false};
    std::atomic<int> droppedFrames_{0};
    std::atomic<int> videoStreamIndex_{-1};
    std::atomic<int> audioStreamIndex_{-1};
    std::atomic<int> hwType_{static_cast<int>(HwAccelType::Software)};

    std::optional<VideoItem> pendingVideo_;
    DisplayedVideoFrame displayed_;
    QVector<HwAccelAttempt> hwAttempts_;
    AVRational videoTimeBase_{0, 1};
    AVRational audioTimeBase_{0, 1};
    MediaTime clockStart_ = MediaTime::fromMicroseconds(0);
    bool priming_ = false;
    qint64 preRollFrames_ = 0;
    std::atomic<bool> endOfStream_{false};
    int outputSampleRate_ = 48'000;
    int outputChannels_ = 2;
};

} // namespace subcue
