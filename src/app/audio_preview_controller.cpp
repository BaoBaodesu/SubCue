#include "audio_preview_controller.h"
#include "application_context.h"
#include "project_session.h"
#include "tts/audio_quality.h"
#include <QtCore/QPointer>
#include <algorithm>
#include <cmath>
namespace subcue {
AudioPreviewController::AudioPreviewController(ApplicationContext *context, ProjectSession *session, QObject *parent)
    : QObject(parent), context_(context)
{
    context_->registerAudioClient(&playback_);
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &AudioPreviewController::tick);
    connect(session, &ProjectSession::aboutToChange, this, &AudioPreviewController::clear);
}
AudioPreviewController::~AudioPreviewController() { stop(); context_->unregisterAudioClient(&playback_); }
void AudioPreviewController::stop()
{
    cancel_ = true; task_.waitForFinished(); ++epoch_; loading_ = false;
    timer_.stop();
    const bool owned = playing_ || paused_;
    paused_ = false;
    playing_ = false; playback_.pause(); playback_.close(); playback_.setAudioDevice(nullptr, 0);
    sourcePath_.clear(); stoppedPositionUs_ = 0;
    if (owned) {
        if (context_->audioDevice) context_->audioDevice->stop();
        emit released();
    }
    status_ = QStringLiteral("试听已停止。"); emit changed();
}
void AudioPreviewController::clear()
{
    stop(); replayPath_.clear(); replayHash_.clear(); durationUs_ = 0; emit changed();
}
void AudioPreviewController::setVolume(double value)
{
    if (!std::isfinite(value)) return;
    volume_ = std::clamp(value, 0.0, 1.0);
    playback_.setOutputGain(muted_ ? 0.0f : float(volume_)); emit changed();
}
void AudioPreviewController::setMuted(bool value)
{
    muted_ = value; playback_.setOutputGain(muted_ ? 0.0f : float(volume_)); emit changed();
}
void AudioPreviewController::seek(qint64 positionUs)
{
    if ((!playing_ && !paused_) || !context_->audioDevice) return;
    stoppedPositionUs_ = std::clamp<qint64>(positionUs, 0, durationUs_);
    context_->audioDevice->pause(); playback_.seek(MediaTime::fromMicroseconds(std::clamp<qint64>(positionUs, 0, durationUs_))); emit changed();
}
void AudioPreviewController::pause()
{
    if (!playing_) return;
    stoppedPositionUs_ = playback_.position().microseconds();
    playback_.pause();
    if (context_->audioDevice) context_->audioDevice->pause();
    timer_.stop(); playing_ = false; paused_ = true;
    status_ = QStringLiteral("试听已暂停。"); emit changed();
}
void AudioPreviewController::resume()
{
    if (!paused_ || !context_->audioDevice || !context_->audioDevice->isStarted()) return;
    playback_.play();
    if (!playback_.isPriming()) context_->audioDevice->resume();
    paused_ = false; playing_ = true; timer_.start();
    status_ = context_->audioDevice->isHardware() ? QStringLiteral("正在试听。") : QStringLiteral("正在虚拟试听计时，不产生可听输出。"); emit changed();
}
void AudioPreviewController::play(const QString &path, const QString &sha256, qint64 startUs)
{
    if (context_->projectSession && context_->projectSession->busy()) {
        status_ = QStringLiteral("工程正在保存或迁移，请稍候再试听。"); emit changed(); return;
    }
    if (path == sourcePath_ && startUs == 0) {
        if (playing_) { pause(); return; }
        if (paused_) { resume(); return; }
    }
    stop();
    const auto hash = QByteArray::fromHex(sha256.toLatin1());
    if (hash.size() != 32 || sha256.size() != 64) { status_ = QStringLiteral("试听资产指纹无效。"); emit changed(); return; }
    durationUs_ = 0;
    const auto epoch = ++epoch_; cancel_ = false; loading_ = true;
    status_ = QStringLiteral("正在准备试听…"); emit changed();
    const QPointer<AudioPreviewController> self(this); auto *cancel = &cancel_;
    task_ = context_->submitAudioProcessing([self, epoch, path, sha256, hash, startUs, cancel] {
        QString error; qint64 duration = 0;
        try {
            const auto quality = AudioQuality::inspect(path, &error, cancel);
            if (quality) duration = quality->sampleCount * 1000000 / quality->sampleRate;
            if (quality && quality->sha256 != hash) error = QStringLiteral("试听文件与资产指纹不符，请恢复原件。");
        } catch (...) { error = QStringLiteral("试听准备失败，请检查文件和可用内存。"); }
        if (cancel->load()) error = QStringLiteral("试听准备已取消。");
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, path, sha256, startUs, error, duration] {
            if (!self || self->epoch_ != epoch) return;
            self->loading_ = false;
            if (!error.isEmpty()) { self->status_ = error; emit self->changed(); return; }
            emit self->requested();
            self->context_->unbindAudioClients();
            auto *device = self->context_->audioDevice.get();
            AppError failure(ErrorDomain::Media, 0, {});
            if (device) device->stop();
            self->playback_.setAudioDevice(device, 20);
            if (!device || !self->playback_.open(path, &failure) || !self->playback_.hasAudio()
                || !device->start(self->playback_.outputSampleRate(), self->playback_.outputChannels(), &failure)) {
                self->playback_.close(); self->playback_.setAudioDevice(nullptr, 0);
                if (device) device->stop();
                self->status_ = failure.userMessage().isEmpty() ? QStringLiteral("音频设备或试听文件不可用。") : failure.userMessage();
                emit self->released(); emit self->changed(); return;
            }
            device->setPlaybackRate(1.0); device->pause();
            self->sourcePath_ = path; self->replayPath_ = path; self->replayHash_ = sha256; self->durationUs_ = duration;
            if (startUs > 0) self->playback_.seek(MediaTime::fromMicroseconds(startUs));
            self->playback_.setAudioDevice(device, 20); self->playback_.play();
            if (!self->playback_.isPriming()) device->resume();
            self->playing_ = true; self->timer_.start(); self->status_ = device->isHardware()
                ? QStringLiteral("正在试听。") : QStringLiteral("正在虚拟试听计时，不产生可听输出。"); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void AudioPreviewController::tick()
{
    if (!playing_) return;
    if (!context_->audioDevice || !context_->audioDevice->isStarted()) {
        stop(); status_ = QStringLiteral("音频设备已停止，试听已退出；请检查设备后重试。"); emit changed(); return;
    }
    const bool priming = playback_.isPriming(); playback_.pump();
    if (context_->audioDevice && !playback_.isPriming()) {
        if (priming) context_->audioDevice->resume();
        if (!context_->audioDevice->isHardware()) (void)context_->audioDevice->renderFrame();
        playback_.pump();
    }
    if (playback_.audioFinished()) {
        const bool hardware = context_->audioDevice->isHardware(); stop(); stoppedPositionUs_ = durationUs_;
        status_ = hardware ? QStringLiteral("试听完毕。") : QStringLiteral("虚拟试听计时完成，没有可听输出。");
    }
    emit changed();
}
}
