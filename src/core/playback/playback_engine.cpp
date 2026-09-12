#include "playback/playback_engine.h"

#include "common/logging.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <chrono>
#include <utility>

namespace subcue {

PlaybackEngine::PlaybackEngine(QObject *parent)
    : QObject(parent)
{
    wakeTimer_.setSingleShot(true);
    wakeTimer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&wakeTimer_, &QTimer::timeout, this, &PlaybackEngine::onSchedulerWake);
}

PlaybackEngine::~PlaybackEngine()
{
    close();
}

bool PlaybackEngine::open(const QString &path, AppError *error)
{
    close();
    if (!demuxer_.open(path, error)) {
        return false;
    }

    videoStreamIndex_ = demuxer_.bestStream(AVMEDIA_TYPE_VIDEO);
    audioStreamIndex_ = demuxer_.bestStream(AVMEDIA_TYPE_AUDIO);
    const AVStream *videoStream = demuxer_.stream(videoStreamIndex_.load());
    const AVStream *audioStream = demuxer_.stream(audioStreamIndex_.load());
    hasVideo_ = videoStream != nullptr;
    hasAudio_ = audioStream != nullptr;
    if (!hasVideo_ && !hasAudio_) {
        if (error) {
            *error = AppError(ErrorDomain::Media, AVERROR_STREAM_NOT_FOUND, QStringLiteral("媒体中没有可播放的流"), path);
        }
        close();
        return false;
    }

    if (videoStream) {
        videoTimeBase_ = videoStream->time_base;
        if (!videoDecoder_.openWithFallback(*videoStream, error)) {
            close();
            return false;
        }
        hwType_ = static_cast<int>(videoDecoder_.activeType());
        hwAttempts_ = videoDecoder_.attempts();
        (void)stepper_.open(path, nullptr);
    }
    if (audioStream) {
        audioTimeBase_ = audioStream->time_base;
        if (!audioDecoder_.open(*audioStream, error)) {
            close();
            return false;
        }
        if (!resampler_.configure(*audioDecoder_.context(), outputSampleRate_, outputChannels_, error)) {
            close();
            return false;
        }
    }
    if (!audioOutput_.configure(outputSampleRate_, outputChannels_, error)) {
        close();
        return false;
    }

    seek_.reset();

    videoPackets_.resetAbort();
    audioPackets_.resetAbort();
    videoFrames_.resetAbort();
    audioFrames_.resetAbort();
    videoPackets_.setGeneration(0);
    audioPackets_.setGeneration(0);
    videoFrames_.setGeneration(0);
    audioFrames_.setGeneration(0);
    audioOutput_.setGeneration(0);
    droppedFrames_ = 0;
    hwRuntimeFallback_ = false;
    forceHwFailure_ = false;
    paused_ = true;
    open_ = true;
    startWorkers();
    return true;
}

void PlaybackEngine::close()
{
    stopWorkers();
    wakeTimer_.stop();
    pendingVideo_.reset();
    stepper_.close();
    videoDecoder_.close();
    audioDecoder_.close();
    demuxer_.close();
    audioOutput_.flush();
    clock_.reset();
    {
        std::lock_guard lock(displayMutex_);
        displayed_ = {};
    }
    {
        std::lock_guard lock(stateMutex_);
        hwAttempts_.clear();
    }
    hasVideo_ = false;
    hasAudio_ = false;
    open_ = false;
    paused_ = true;
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
}

void PlaybackEngine::play()
{
    paused_ = false;
    audioOutput_.resume();
    clock_.resume();
    onSchedulerWake();
}

void PlaybackEngine::pause()
{
    clock_.syncFrom(audioOutput_);
    clock_.pause();
    audioOutput_.pause();
    paused_ = true;
    wakeTimer_.stop();
}

quint64 PlaybackEngine::seek(MediaTime target)
{
    const quint64 generation = seek_.request(target);
    videoPackets_.setGeneration(generation);
    audioPackets_.setGeneration(generation);
    videoFrames_.setGeneration(generation);
    audioFrames_.setGeneration(generation);
    audioOutput_.setGeneration(generation);
    clock_.reset();
    pendingVideo_.reset();
    {
        std::lock_guard lock(displayMutex_);
        displayed_ = {};
    }
    if (stepper_.index().size() > 0) {
        (void)stepper_.seekTo(target, nullptr);
    }
    wakeDemux();
    onSchedulerWake();
    return generation;
}

bool PlaybackEngine::stepForward(AppError *error)
{
    pause();
    return stepper_.stepForward(error);
}

bool PlaybackEngine::stepBackward(AppError *error)
{
    pause();
    return stepper_.stepBackward(error);
}

qint64 PlaybackEngine::consumeAudio(MediaTime duration)
{
    drainAudioToOutput();
    qint64 consumed = 0;
    if (hasAudio_.load()) {
        consumed = audioOutput_.consumeDuration(duration);
        clock_.syncFrom(audioOutput_);
    } else if (duration.microseconds() > 0) {
        if (!clock_.isStarted()) {
            clock_.start(MediaTime::fromMicroseconds(0), outputSampleRate_);
        }
        const qint64 frames = av_rescale(duration.microseconds(), outputSampleRate_, 1'000'000);
        clock_.setWrittenSamples(clock_.writtenSamples() + frames);
        clock_.setBufferedSamples(0);
        consumed = frames;
    }
    onSchedulerWake();
    return consumed;
}

void PlaybackEngine::pump()
{
    drainAudioToOutput();
    clock_.syncFrom(audioOutput_);
    onSchedulerWake();
}

bool PlaybackEngine::waitForDisplayedFrame(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        drainAudioToOutput();
        clock_.syncFrom(audioOutput_);
        onSchedulerWake();
        QCoreApplication::processEvents();
        {
            std::lock_guard lock(displayMutex_);
            if (displayed_.generation == seek_.generation() && displayed_.pts.isValidRange()) {
                return true;
            }
        }
        if (!paused_.load()) {
            audioOutput_.consumeDuration(MediaTime::fromMilliseconds(5));
            clock_.syncFrom(audioOutput_);
        }
        QThread::msleep(5);
    }
    return false;
}

void PlaybackEngine::requestHwRuntimeFailure()
{
    forceHwFailure_.store(true);
    videoPackets_.wake();
}

MediaTime PlaybackEngine::position() const
{
    return clock_.now();
}

quint64 PlaybackEngine::generation() const
{
    return seek_.generation();
}

HwAccelType PlaybackEngine::hwAccel() const noexcept
{
    return static_cast<HwAccelType>(hwType_.load());
}

QVector<HwAccelAttempt> PlaybackEngine::hwAttempts() const
{
    std::lock_guard lock(stateMutex_);
    return hwAttempts_;
}

DisplayedVideoFrame PlaybackEngine::displayedFrame() const
{
    std::lock_guard lock(displayMutex_);
    return displayed_;
}

void PlaybackEngine::startWorkers()
{
    stop_ = false;
    demuxThread_ = std::thread(&PlaybackEngine::demuxLoop, this);
    if (hasVideo_.load()) {
        videoThread_ = std::thread(&PlaybackEngine::videoLoop, this);
    }
    if (hasAudio_.load()) {
        audioThread_ = std::thread(&PlaybackEngine::audioLoop, this);
    }
}

void PlaybackEngine::stopWorkers()
{
    stop_ = true;
    videoPackets_.abort();
    audioPackets_.abort();
    videoFrames_.abort();
    audioFrames_.abort();
    wakeDemux();
    if (demuxThread_.joinable()) {
        demuxThread_.join();
    }
    if (videoThread_.joinable()) {
        videoThread_.join();
    }
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    videoPackets_.resetAbort();
    audioPackets_.resetAbort();
    videoFrames_.resetAbort();
    audioFrames_.resetAbort();
    videoPackets_.clear();
    audioPackets_.clear();
    videoFrames_.clear();
    audioFrames_.clear();
}

void PlaybackEngine::wakeDemux()
{
    std::lock_guard lock(demuxMutex_);
    demuxWake_.notify_all();
}

void PlaybackEngine::scheduleWake(MediaTime delay)
{
    const qint64 microseconds = delay.microseconds();
    const int milliseconds = microseconds <= 0
        ? 0
        : static_cast<int>((microseconds + 999) / 1'000);
    wakeTimer_.start(milliseconds);
}

void PlaybackEngine::drainAudioToOutput()
{
    BoundedQueue<AudioItem>::Item item;
    while (audioFrames_.tryPop(item)) {
        if (!seek_.canCommit(item.generation)) {
            continue;
        }
        (void)audioOutput_.write(std::move(item.value.samples), item.value.pts, item.generation);
    }
}

void PlaybackEngine::onSchedulerWake()
{
    if (!open_.load()) {
        return;
    }
    if (!pendingVideo_) {
        BoundedQueue<VideoItem>::Item item;
        if (!videoFrames_.tryPop(item)) {
            return;
        }
        pendingVideo_ = std::move(item.value);
    }
    if (!seek_.canCommit(pendingVideo_->generation)) {
        pendingVideo_.reset();
        onSchedulerWake();
        return;
    }

    clock_.syncFrom(audioOutput_);
    MediaTime master = clock_.now();
    if (!master.isValidRange()) {
        if (paused_.load()) {
            std::lock_guard lock(displayMutex_);
            displayed_.image = pendingVideo_->image;
            displayed_.pts = pendingVideo_->pts;
            displayed_.generation = pendingVideo_->generation;
            pendingVideo_.reset();
            return;
        }
        master = pendingVideo_->pts;
    }

    const ScheduleDecision decision = scheduler_.evaluate(pendingVideo_->pts, master);
    if (decision.action == FrameAction::Wait) {
        quint64 displayedGeneration = 0;
        {
            std::lock_guard lock(displayMutex_);
            displayedGeneration = displayed_.generation;
        }
        if (paused_.load() && displayedGeneration != pendingVideo_->generation) {
            std::lock_guard lock(displayMutex_);
            displayed_.image = pendingVideo_->image;
            displayed_.pts = pendingVideo_->pts;
            displayed_.generation = pendingVideo_->generation;
            pendingVideo_.reset();
            return;
        }
        if (paused_.load()) {
            return;
        }
        scheduleWake(decision.wakeDelay);
        return;
    }
    if (decision.action == FrameAction::Drop) {
        droppedFrames_.fetch_add(1);
        pendingVideo_.reset();
        onSchedulerWake();
        return;
    }

    {
        std::lock_guard lock(displayMutex_);
        displayed_.image = pendingVideo_->image;
        displayed_.pts = pendingVideo_->pts;
        displayed_.generation = pendingVideo_->generation;
    }
    pendingVideo_.reset();
    if (!paused_.load()) {
        scheduleWake(MediaTime::fromMicroseconds(0));
    }
}

void PlaybackEngine::demuxLoop()
{
    while (!stop_.load()) {
        quint64 generation = 0;
        MediaTime target;
        if (seek_.takePending(&generation, &target)) {
            const int streamIndex = hasVideo_.load() ? videoStreamIndex_.load() : audioStreamIndex_.load();
            const AVRational timeBase = hasVideo_.load() ? videoTimeBase_ : audioTimeBase_;
            if (streamIndex >= 0) {
                (void)demuxer_.seek(streamIndex, timestampFromMediaTime(target, timeBase), nullptr);
            }
            continue;
        }

        AppError error(ErrorDomain::Media, 0, QString());
        PacketPtr packet = demuxer_.readPacket(&error);
        if (seek_.takePending(&generation, &target)) {
            const int streamIndex = hasVideo_.load() ? videoStreamIndex_.load() : audioStreamIndex_.load();
            const AVRational timeBase = hasVideo_.load() ? videoTimeBase_ : audioTimeBase_;
            if (streamIndex >= 0) {
                (void)demuxer_.seek(streamIndex, timestampFromMediaTime(target, timeBase), nullptr);
            }
            continue;
        }
        if (!packet) {
            std::unique_lock lock(demuxMutex_);
            demuxWake_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return stop_.load() || seek_.isPending();
            });
            continue;
        }

        generation = seek_.generation();
        const int streamIndex = packet->stream_index;
        const int bytes = packet->size;
        if (hasVideo_.load() && streamIndex == videoStreamIndex_.load()) {
            (void)videoPackets_.push(std::move(packet), bytes, generation);
        } else if (hasAudio_.load() && streamIndex == audioStreamIndex_.load()) {
            (void)audioPackets_.push(std::move(packet), bytes, generation);
        }
    }
}

bool PlaybackEngine::receiveVideoFrame(quint64 generation, AVFrame *frame, AppError *error)
{
    const int received = videoDecoder_.receive(frame);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
        return false;
    }
    if (received < 0) {
        if (videoDecoder_.handleRuntimeFailure(error)) {
            hwRuntimeFallback_ = true;
            hwType_ = static_cast<int>(HwAccelType::Software);
            std::lock_guard lock(stateMutex_);
            hwAttempts_ = videoDecoder_.attempts();
            qCInfo(subcuePlaybackLog) << "Video decoder fell back to software";
            return false;
        }
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("视频解码失败"));
        }
        return false;
    }
    if (!videoDecoder_.ensureSoftwareFrame(frame, error)) {
        return false;
    }
    if (!seek_.canCommit(generation)) {
        av_frame_unref(frame);
        return true;
    }
    const MediaTime pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, videoTimeBase_);
    if (pts.microseconds() < seek_.target().microseconds()) {
        av_frame_unref(frame);
        return true;
    }
    AppError convertError(ErrorDomain::Decoder, 0, QString());
    QImage image = converter_.convert(*frame, {}, &convertError);
    av_frame_unref(frame);
    if (image.isNull()) {
        return true;
    }
    VideoItem item;
    item.image = std::move(image);
    item.pts = pts;
    item.generation = generation;
    const qsizetype bytes = item.image.sizeInBytes();
    if (videoFrames_.push(std::move(item), bytes, generation)) {
        QMetaObject::invokeMethod(this, [this] { onSchedulerWake(); }, Qt::QueuedConnection);
    }
    return true;
}

void PlaybackEngine::videoLoop()
{
    FramePtr frame = makeFrame();
    if (!frame) {
        return;
    }
    quint64 lastGeneration = 0;
    while (!stop_.load()) {
        BoundedQueue<PacketPtr>::Item item;
        if (!videoPackets_.pop(item)) {
            break;
        }
        if (item.generation != lastGeneration) {
            videoDecoder_.flush();
            lastGeneration = item.generation;
        }
        if (!seek_.canCommit(item.generation)) {
            continue;
        }
        if (forceHwFailure_.exchange(false)) {
            if (videoDecoder_.handleRuntimeFailure(nullptr)) {
                hwRuntimeFallback_ = true;
                hwType_ = static_cast<int>(HwAccelType::Software);
                std::lock_guard lock(stateMutex_);
                hwAttempts_ = videoDecoder_.attempts();
            }
        }

        AppError error(ErrorDomain::Decoder, 0, QString());
        int sent = videoDecoder_.send(item.value.get());
        while (sent == AVERROR(EAGAIN)) {
            if (!receiveVideoFrame(item.generation, frame.get(), &error)) {
                break;
            }
            sent = videoDecoder_.send(item.value.get());
        }
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            if (videoDecoder_.handleRuntimeFailure(nullptr)) {
                hwRuntimeFallback_ = true;
                hwType_ = static_cast<int>(HwAccelType::Software);
                std::lock_guard lock(stateMutex_);
                hwAttempts_ = videoDecoder_.attempts();
            }
            continue;
        }
        while (receiveVideoFrame(item.generation, frame.get(), &error)) {
        }
    }
}

void PlaybackEngine::audioLoop()
{
    FramePtr frame = makeFrame();
    if (!frame) {
        return;
    }
    quint64 lastGeneration = 0;
    while (!stop_.load()) {
        BoundedQueue<PacketPtr>::Item item;
        if (!audioPackets_.pop(item)) {
            break;
        }
        if (item.generation != lastGeneration) {
            audioDecoder_.flush();
            if (audioDecoder_.context()) {
                (void)resampler_.configure(*audioDecoder_.context(), outputSampleRate_, outputChannels_, nullptr);
            }
            lastGeneration = item.generation;
        }
        if (!seek_.canCommit(item.generation)) {
            continue;
        }

        int sent = audioDecoder_.send(item.value.get());
        auto receiveAll = [&] {
            for (;;) {
                const int received = audioDecoder_.receive(frame.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                    return;
                }
                if (received < 0) {
                    av_frame_unref(frame.get());
                    return;
                }
                if (!seek_.canCommit(item.generation)) {
                    av_frame_unref(frame.get());
                    return;
                }
                const MediaTime pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, audioTimeBase_);
                if (pts.microseconds() < seek_.target().microseconds()) {
                    av_frame_unref(frame.get());
                    continue;
                }
                AppError error(ErrorDomain::Decoder, 0, QString());
                QVector<float> samples = resampler_.convert(*frame, &error);
                av_frame_unref(frame.get());
                if (samples.isEmpty()) {
                    continue;
                }
                AudioItem audio;
                audio.samples = std::move(samples);
                audio.pts = pts;
                audio.generation = item.generation;
                const qsizetype bytes = audio.samples.size() * static_cast<qsizetype>(sizeof(float));
                (void)audioFrames_.push(std::move(audio), bytes, item.generation);
            }
        };
        while (sent == AVERROR(EAGAIN)) {
            receiveAll();
            sent = audioDecoder_.send(item.value.get());
        }
        if (sent < 0) {
            continue;
        }
        receiveAll();
    }
}

} // namespace subcue
