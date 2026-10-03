#include "rough_cut_controller.h"
#include "common/file_path_guard.h"
#include "video_preview_item.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QDateTime>

#include "ai/ai_review_service.h"
#include "ai/ai_review_settings.h"
#include "ai/ai_types.h"
#include "application_context.h"
#include "asr/asr_provider_catalog.h"
#include "asr/asr_provider_factory.h"
#include "asr/asr_types.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/http_client.h"
#include "media/media_probe.h"
#include "media/demuxer.h"
#include "media/decoder.h"
#include "media/ffmpeg_error.h"
#include "roughcut/retake_detector.h"
#include "roughcut/alignment_evidence.h"
#include "roughcut/safe_cut_boundary.h"
#include "roughcut/rough_cut_project.h"
#include "roughcut/script_document.h"
#include "roughcut/speech_segment_analyzer.h"
#include "roughcut/xmeml_exporter.h"
#include "roughcut/wav_exporter.h"
#include "timeline_scene_item.h"
#include "waveform/waveform_generator.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDataStream>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>

#include <algorithm>
#include <variant>

namespace subcue {
namespace {

QString localPath(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

QString transcriptText(const Transcript &transcript, qint64 startMs, qint64 endMs)
{
    QString result;
    for (const TranscriptWord &word : transcript.words) {
        const qint64 midpoint = (word.startMs + word.endMs) / 2;
        if (midpoint >= startMs && midpoint < endMs) result += word.text;
    }
    return result.trimmed();
}

std::variant<qint64, AppError> audioEndSample(const QString &path, int sampleRate, const std::atomic<bool> *cancel)
{
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    Decoder decoder;
    if (!demuxer.open(path, &error)) return error;
    const int index = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const auto *stream = demuxer.stream(index);
    if (!stream || !decoder.open(*stream, &error)) return error;
    auto frame = makeFrame();
    if (!frame) return AppError(ErrorDomain::Media, 1, QStringLiteral("无法分配音频帧"));
    qint64 end = 0;
    bool draining = false;
    for (;;) {
        if (cancel && cancel->load()) return asrCancelledError();
        if (!draining) {
            auto packet = demuxer.readPacket(&error);
            if (!packet) {
                if (error.code() != 0) return error;
                draining = true;
                (void)decoder.send(nullptr);
            } else {
                if (packet->stream_index != index) continue;
                const int sent = decoder.send(packet.get());
                if (sent < 0) return makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("音频范围检查失败"));
            }
        }
        for (;;) {
            const int received = decoder.receive(frame.get());
            if (received == AVERROR(EAGAIN)) break;
            if (received == AVERROR_EOF) return end;
            if (received < 0) return makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("音频范围检查失败"));
            if (frame->best_effort_timestamp == AV_NOPTS_VALUE || frame->sample_rate <= 0)
                return AppError(ErrorDomain::Media, 1, QStringLiteral("无法确认音频时间映射"));
            const qint64 at = av_rescale_q(frame->best_effort_timestamp, stream->time_base, AVRational{1, sampleRate});
            end = std::max(end, at + av_rescale(frame->nb_samples, sampleRate, frame->sample_rate));
            av_frame_unref(frame.get());
        }
        if (draining) return end;
    }
}

QString asrProviderName(const QJsonObject &settings)
{
    const QString provider = AsrProviderFactory::providerIdFromSettings(settings);
    if (provider == QLatin1String("qwen3") || provider == QLatin1String("funasr")) {
        return AsrProviderCatalog::displayName(provider);
    }
    return settings.value(QStringLiteral("asrModel")).toString(QStringLiteral("云端 ASR"));
}

QString savedAsrApiKey(const ApplicationContext *context, const QJsonObject &settings)
{
    if (!context || AsrProviderFactory::providerIdFromSettings(settings)
        != QLatin1String(kAsrProviderDashScope)) {
        return {};
    }
    return context->credentials.load(QStringLiteral("SubCue/ASR/dashscope")).trimmed();
}

QString scriptDocumentText(const ScriptDocument &document)
{
    QStringList lines;
    lines.reserve(document.lines.size());
    for (const ScriptLine &line : document.lines) lines.append(line.text);
    return lines.join(QLatin1Char('\n'));
}

ScriptDocument scriptDocumentFromText(const QString &text)
{
    ScriptDocument document;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    document.lines.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        QString line = lines.at(index);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        document.lines.append({index + 1, index + 1, false, line});
    }
    return document;
}

std::optional<RoughCutDecision> parseDecision(const QString &value)
{
    if (value == QLatin1String("KEEP")) return RoughCutDecision::Keep;
    if (value == QLatin1String("CUT")) return RoughCutDecision::Cut;
    if (value == QLatin1String("REVIEW")) return RoughCutDecision::Review;
    return std::nullopt;
}

} // namespace

RoughCutController::RoughCutController(ApplicationContext *context, QObject *parent)
    : QObject(parent), context_(context)
{
    if (context_) context_->registerAudioClient(&playback_);
    filterModel_.setSourceModel(&model_);
    connect(&filterModel_, &RoughCutResultFilterModel::statusFilterChanged,
        this, &RoughCutController::statusFilterChanged);
    markSaved();
    playbackTimer_.setInterval(30);
    connect(&playbackTimer_, &QTimer::timeout, this, [this] {
        const bool wasPriming = playback_.isPriming();
        playback_.pump();
        if (previewItem_) {
            const auto frame = playback_.displayedFrame();
            if (!frame.image.isNull()) previewItem_->present(frame.image, frame.pts, frame.generation);
        }
        if (wasPriming && !playback_.isPriming() && !playback_.isPaused()
            && context_ && context_->audioDevice) {
            context_->audioDevice->resume();
        }
        if (!playback_.isPaused() && context_ && context_->audioDevice
            && !playback_.isPriming() && !context_->audioDevice->isHardware()) {
            (void)context_->audioDevice->renderFrame();
            playback_.pump();
        }
        if (timelinePlaybackIndex_ >= 0 && timelineGapDeadlineMs_ >= 0
            && timelinePlaybackClock_.elapsed() >= timelineGapDeadlineMs_) {
            timelineGapDeadlineMs_ = -1;
            startTimelineClip(timelinePlaybackIndex_);
        }
        if (auditionEndSample_ >= 0 && sampleRate_ > 0
            && playback_.position().microseconds() * sampleRate_ / 1'000'000
                >= auditionEndSample_ - sampleRate_ / 100) {
            if (timelinePlaybackIndex_ >= 0) {
                playback_.pause();
                if (context_->audioDevice) context_->audioDevice->pause();
                const int finished = timelinePlaybackIndex_++;
                auditionEndSample_ = -1;
                if (timelinePlaybackIndex_ >= timeline_.size() || !continuousAudition_) {
                    if (timelineItem_) {
                        const RoughCutTimelineClip &clip = timeline_.at(finished);
                        timelineItem_->setPlayheadUs((clip.timelineStartSample
                            + clip.sourceEndSample - clip.sourceStartSample) * 1'000'000 / sampleRate_);
                    }
                    stopTimeline();
                } else {
                    const RoughCutTimelineClip &current = timeline_.at(finished);
                    const RoughCutTimelineClip &next = timeline_.at(timelinePlaybackIndex_);
                    const qint64 currentEnd = current.timelineStartSample
                        + current.sourceEndSample - current.sourceStartSample;
                    timelineGapDeadlineMs_ = timelinePlaybackClock_.elapsed()
                        + std::max<qint64>(0, next.timelineStartSample - currentEnd) * 1000 / sampleRate_;
                }
            } else {
                playback_.pause();
                if (context_->audioDevice) context_->audioDevice->pause();
                playbackTimer_.stop();
                emit playbackChanged();
                auditionEndSample_ = -1;
            }
        }
        if (timelinePlaybackIndex_ < 0 && auditionEndSample_ < 0 && !playback_.isPaused()
            && durationMs() > 0 && positionMs() >= durationMs() - 10) {
            playback_.pause();
            if (context_->audioDevice) context_->audioDevice->pause();
            playbackTimer_.stop();
            emit playbackChanged();
        }
        emit positionChanged();
        if (sourceWaveformItem_) sourceWaveformItem_->setPlayheadUs(positionMs() * 1000);
        if (timelineItem_ && timelinePlaybackIndex_ >= 0 && timelinePlaybackIndex_ < timeline_.size()) {
            const RoughCutTimelineClip &clip = timeline_.at(timelinePlaybackIndex_);
            timelineItem_->setPlayheadUs((clip.timelineStartSample
                + std::clamp<qint64>(playback_.position().microseconds() * sampleRate_ / 1'000'000
                    - clip.sourceStartSample, 0, clip.sourceEndSample - clip.sourceStartSample))
                * 1'000'000 / sampleRate_);
        }
    });
}

RoughCutController::~RoughCutController()
{
    shutdown();
}

void RoughCutController::shutdown()
{
    if (shuttingDown_) return;
    shuttingDown_ = true;
    stopFrameRateCheck();
    cancel_ = true;
    stopWorker();
    stopWaveformWorker();
    releasePlayback();
    if (context_) context_->unregisterAudioClient(&playback_);
    playback_.close();
}

qint64 RoughCutController::positionMs() const { return playback_.position().milliseconds(); }
qint64 RoughCutController::durationMs() const
{
    return sampleRate_ > 0 ? sourceSampleCount_ * 1000 / sampleRate_ : 0;
}

void RoughCutController::loadMedia(const QUrl &url)
{
    if (busy_ || shuttingDown_) return;
    const QString path = localPath(url);
    if (path.isEmpty()) return;
    stopFrameRateCheck();
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在打开媒体"), true);
    setStatus(QStringLiteral("正在打开媒体…"));
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, path, generation] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        if (cancel_.load()) {
            postUi([](RoughCutController *controller) {
                controller->finishJobWithoutResults(QStringLiteral("校验已取消。"));
            });
            return;
        }
        const ProbeResult probed = MediaProbe::probe(path);
        if (std::holds_alternative<AppError>(probed)) {
            postUi([message = std::get<AppError>(probed).userMessage()](RoughCutController *controller) {
                controller->finishJobWithoutResults(message);
            });
            return;
        }
        const MediaInfo info = std::get<MediaInfo>(probed);
        if (info.audioStreamIndex < 0 || info.audioStreamIndex >= info.streams.size()
            || info.streams.at(info.audioStreamIndex).sampleRate <= 0) {
            postUi([](RoughCutController *controller) {
                controller->finishJobWithoutResults(QStringLiteral("所选文件没有音频轨。"));
            });
            return;
        }
        const auto audioEnd = audioEndSample(path, info.streams.at(info.audioStreamIndex).sampleRate, &cancel_);
        if (std::holds_alternative<AppError>(audioEnd) || std::get<qint64>(audioEnd) <= 0) {
            const QString message = std::holds_alternative<AppError>(audioEnd)
                ? std::get<AppError>(audioEnd).userMessage() : QStringLiteral("源文件没有有效音频");
            postUi([message](RoughCutController *controller) { controller->finishJobWithoutResults(message); });
            return;
        }
        postUi([path, info, sourceCount = std::get<qint64>(audioEnd)](RoughCutController *controller) {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("校验已取消。"));
                return;
            }
            const MediaStreamInfo stream = info.streams.at(info.audioStreamIndex);
            AppError error(ErrorDomain::Media, 0, QString());
            controller->stopWaveformWorker();
            if (controller->sourceWaveformItem_) controller->sourceWaveformItem_->setWaveform({});
            controller->playback_.close();
            if (!controller->playback_.open(path, &error)) {
                controller->finishJobWithoutResults(error.userMessage());
                return;
            }
            if (controller->ownsSharedAudio_) controller->bindSharedAudioDevice();
            controller->mediaPath_ = QFileInfo(path).canonicalFilePath();
            controller->mediaInfo_ = info;
            controller->projectData_ = {};
            controller->projectData_.state.insert(QStringLiteral("timelineOptions"), OmniReviewSettingsStore::toJsonPatch(OmniReviewSettingsStore::fromJson(controller->context_->settings)));
            if (info.videoStreamIndex >= 0) {
                const auto &video = info.streams.at(info.videoStreamIndex);
                controller->projectData_.state.insert(QStringLiteral("sequenceFrameRate"), QStringLiteral("%1/%2").arg(video.frameRateNumerator).arg(video.frameRateDenominator));
            }
            controller->analysisWords_.clear();
            controller->sampleRate_ = stream.sampleRate;
            controller->channels_ = stream.channels;
            controller->sourceSampleCount_ = sourceCount;
            controller->model_.reset({}, {}, controller->sampleRate_);
            controller->timeline_.clear();
            controller->history_.clear();
            controller->historyIndex_ = 0;
            controller->projectPath_.clear();
            controller->analysisVersion_ = 0;
            controller->auxiliaryResults_.clear();
            controller->setBusy(false);
            emit controller->mediaChanged();
            emit controller->resultsChanged();
            emit controller->canAiReviewChanged();
            emit controller->historyChanged();
            emit controller->projectChanged();
            emit controller->canSaveChanged();
            controller->refreshModified();
            controller->setStatus(QStringLiteral("已加载：%1").arg(QFileInfo(controller->mediaPath_).fileName()));
            controller->syncSceneItems();
            controller->startWaveformWorker();
            controller->startFrameRateCheck();
        });
    });
}

RoughCutProject RoughCutController::projectSnapshot() const
{
    RoughCutProject project = projectData_;
    project.mediaPath = mediaPath_;
    project.scriptPath = scriptPath_;
    project.scriptText = scriptText_;
    project.sampleRate = sampleRate_;
    project.channels = channels_;
    project.sourceSampleCount = sourceSampleCount_;
    project.analysisVersion = std::max(1, analysisVersion_);
    project.recording = model_.recording();
    project.decisions = model_.baseDecisions();
    project.auxiliaryResults = auxiliaryResults_;
    project.state.insert(QStringLiteral("mediaInfo"), mediaInfoToJson(mediaInfo_));
    QJsonArray words;
    for (const auto &word : analysisWords_)
        words.append(QJsonObject{{QStringLiteral("id"), word.id}, {QStringLiteral("text"), word.text},
            {QStringLiteral("startMs"), word.startMs}, {QStringLiteral("endMs"), word.endMs},
            {QStringLiteral("preciseTiming"), word.preciseTiming}});
    project.state.insert(QStringLiteral("words"), words);
    QJsonArray timeline;
    for (const auto &clip : timeline_)
        timeline.append(QJsonObject{{QStringLiteral("start"), clip.sourceStartSample},
            {QStringLiteral("end"), clip.sourceEndSample}, {QStringLiteral("timelineStart"), clip.timelineStartSample},
            {QStringLiteral("decision"), int(clip.decision)}, {QStringLiteral("scriptLine"), clip.scriptLineId},
            {QStringLiteral("takeGroup"), clip.retakeGroupId}, {QStringLiteral("recording"), clip.recordingIndex},
            {QStringLiteral("text"), clip.text}});
    project.state.insert(QStringLiteral("timeline"), timeline);
    project.state.insert(QStringLiteral("timelineVersion"), projectData_.state.value(QStringLiteral("timelineVersion")).toInt(1));
    QJsonObject session = project.state.value(QStringLiteral("session")).toObject();
    session.insert(QStringLiteral("positionMs"), positionMs());
    session.insert(QStringLiteral("statusFilter"), statusFilter());
    session.insert(QStringLiteral("playbackRate"), playbackRate_);
    session.insert(QStringLiteral("continuousAudition"), continuousAudition_);
    if (sourceWaveformItem_) {
        session.insert(QStringLiteral("sourceZoom"), sourceWaveformItem_->pixelsPerMs());
        session.insert(QStringLiteral("sourceScroll"), sourceWaveformItem_->scrollOffset());
    }
    if (timelineItem_) {
        session.insert(QStringLiteral("timelineZoom"), timelineItem_->pixelsPerMs());
        session.insert(QStringLiteral("timelineScroll"), timelineItem_->scrollOffset());
    }
    project.state.insert(QStringLiteral("session"), session);
    return project;
}

bool RoughCutController::saveProject(const QUrl &url)
{
    if (!canSave()) return false;
    const QString path = localPath(url);
    QString error;
    if (!safeOutputPath(path, {mediaPath_, scriptPath_}, &error)) {
        setStatus(error);
        emit projectSaveFinished(false, path, error);
        return false;
    }
    stopWorker();
    cancel_ = false;
    RoughCutProject project = projectSnapshot();
    const QByteArray fingerprint = RoughCutProjectSerializer::fingerprint(project);
    const QPointer<RoughCutController> self(this);
    auto *cancel = &cancel_;
    beginTask(QStringLiteral("正在保存工程"), false);
    worker_ = std::thread([self, cancel, path, project = std::move(project), fingerprint]() mutable {
        QString message;
        bool success = !cancel->load();
        const QFileInfo media(project.mediaPath);
        if (success && !project.mediaPath.isEmpty() && media.exists()
            && (project.mediaSha256.size() != 32
                || project.state.value(QStringLiteral("mediaSize")).toInteger(-1) != media.size()
                || project.state.value(QStringLiteral("mediaMtime")).toInteger(-1) != media.lastModified().toMSecsSinceEpoch())) {
            const auto hash = RoughCutProjectSerializer::mediaSha256(project.mediaPath, &message, cancel);
            success = hash.size() == 32 && (project.mediaSha256.isEmpty() || hash == project.mediaSha256);
            if (!success && message.isEmpty()) message = QStringLiteral("源素材内容已变化，请重新定位原素材或导入新素材。");
            if (success) project.mediaSha256 = hash;
            project.state.insert(QStringLiteral("mediaSize"), media.size());
            project.state.insert(QStringLiteral("mediaMtime"), media.lastModified().toMSecsSinceEpoch());
        }
        if (success) success = !cancel->load() && RoughCutProjectSerializer::save(path, project, &message, cancel);
        if (!success && message.isEmpty()) message = QStringLiteral("保存已取消。");
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, path, project = std::move(project), fingerprint, success, message] {
            if (!self || self->shuttingDown_) return;
            if (success) {
                self->projectData_.mediaSha256 = project.mediaSha256;
                self->projectData_.state.insert(QStringLiteral("mediaSize"), project.state.value(QStringLiteral("mediaSize")));
                self->projectData_.state.insert(QStringLiteral("mediaMtime"), project.state.value(QStringLiteral("mediaMtime")));
                self->projectPath_ = QFileInfo(path).absoluteFilePath();
                self->savedFingerprint_ = fingerprint;
                self->refreshModified();
                emit self->projectChanged();
            }
            self->setBusy(false);
            self->setStatus(success ? QStringLiteral("粗剪工程已保存：%1").arg(path) : message);
            emit self->projectSaveFinished(success, path, self->statusText_);
        }, Qt::QueuedConnection);
    });
    return true;
}

bool RoughCutController::saveCurrentProject()
{
    return !projectPath_.isEmpty() && saveProject(QUrl::fromLocalFile(projectPath_));
}

void RoughCutController::applyProject(const RoughCutProject &project, const QString &path,
    const MediaInfo &info, bool available)
{
    stopFrameRateCheck();
    stopWaveformWorker();
    stopTimeline();
    playback_.close();
    if (sourceWaveformItem_) sourceWaveformItem_->setWaveform({});
    if (previewItem_) previewItem_->clearFrame();
    projectData_ = project;
    mediaInfo_ = info;
    mediaPath_ = project.mediaPath;
    scriptPath_ = project.scriptPath;
    scriptText_ = project.scriptText;
    projectPath_ = path;
    sampleRate_ = project.sampleRate;
    channels_ = project.channels;
    sourceSampleCount_ = project.sourceSampleCount;
    analysisVersion_ = project.recording.isEmpty() ? 0 : project.analysisVersion;
    analysisWords_.clear();
    for (const auto &value : project.state.value(QStringLiteral("words")).toArray()) {
        const auto word = value.toObject();
        analysisWords_.append({word.value(QStringLiteral("id")).toInteger(), word.value(QStringLiteral("text")).toString(),
            word.value(QStringLiteral("startMs")).toInteger(), word.value(QStringLiteral("endMs")).toInteger(),
            word.value(QStringLiteral("preciseTiming")).toBool()});
    }
    auxiliaryResults_ = project.auxiliaryResults;
    model_.reset(project.recording, project.decisions, sampleRate_,
        project.state.value(QStringLiteral("analysisScriptText")).toString(scriptText_));
    history_.clear();
    historyIndex_ = 0;
    timeline_.clear();
    if (project.schemaVersion >= 6 && project.state.value(QStringLiteral("timeline")).isArray()) {
        for (const auto &value : project.state.value(QStringLiteral("timeline")).toArray()) {
            const auto clip = value.toObject();
            timeline_.append({clip.value(QStringLiteral("start")).toInteger(), clip.value(QStringLiteral("end")).toInteger(),
                clip.value(QStringLiteral("timelineStart")).toInteger(), RoughCutDecision(clip.value(QStringLiteral("decision")).toInt()),
                clip.value(QStringLiteral("scriptLine")).toInt(-1), clip.value(QStringLiteral("takeGroup")).toInt(-1),
                clip.value(QStringLiteral("recording")).toInt(-1), clip.value(QStringLiteral("text")).toString()});
        }
        syncSceneItems();
    } else rebuildTimeline();
    AppError error(ErrorDomain::Media, 0, QString());
    if (available && playback_.open(mediaPath_, &error)) {
        if (ownsSharedAudio_) bindSharedAudioDevice();
        startWaveformWorker();
        startFrameRateCheck();
    }
    const auto session = project.state.value(QStringLiteral("session")).toObject();
    setStatusFilter(session.value(QStringLiteral("statusFilter")).toString(QStringLiteral("ALL")));
    continuousAudition_ = session.value(QStringLiteral("continuousAudition")).toBool(true);
    setPlaybackRate(session.value(QStringLiteral("playbackRate")).toDouble(1.0));
    seek(std::clamp(session.value(QStringLiteral("positionMs")).toInteger(), qint64(0), durationMs()));
    if (sourceWaveformItem_) sourceWaveformItem_->setView(
        session.value(QStringLiteral("sourceZoom")).toDouble(sourceWaveformItem_->pixelsPerMs()),
        session.value(QStringLiteral("sourceScroll")).toDouble());
    if (timelineItem_) timelineItem_->setView(
        session.value(QStringLiteral("timelineZoom")).toDouble(timelineItem_->pixelsPerMs()),
        session.value(QStringLiteral("timelineScroll")).toDouble());
    setBusy(false);
    emit mediaChanged();
    emit scriptChanged();
    emit projectChanged();
    emit resultsChanged();
    emit historyChanged();
    emit canSaveChanged();
    markSaved();
}

void RoughCutController::newProject()
{
    if (busy_ || shuttingDown_) return;
    stopWorker();
    applyProject(RoughCutProject{}, {}, MediaInfo{}, false);
    setStatus(QStringLiteral("新建粗剪工程。"));
}

void RoughCutController::openProject(const QUrl &url)
{
    if (busy_ || shuttingDown_) return;
    const QString path = localPath(url);
    stopWorker();
    cancel_ = false;
    const QPointer<RoughCutController> self(this);
    auto *cancel = &cancel_;
    beginTask(QStringLiteral("正在打开工程"), false);
    worker_ = std::thread([self, cancel, path] {
        QString message;
        auto project = RoughCutProjectSerializer::load(path, &message);
        MediaInfo info;
        bool available = false;
        if (project) {
            info = mediaInfoFromJson(project->state.value(QStringLiteral("mediaInfo")).toObject());
            if (!project->mediaPath.isEmpty() && QFileInfo::exists(project->mediaPath)) {
                const auto hash = RoughCutProjectSerializer::mediaSha256(project->mediaPath, &message, cancel);
                if (hash.size() == 32 && hash == project->mediaSha256) {
                    const auto probed = MediaProbe::probe(project->mediaPath);
                    if (std::holds_alternative<MediaInfo>(probed)) {
                        const bool verified = info.cfrVerified;
                        info = std::get<MediaInfo>(probed);
                        info.cfrVerified = verified;
                        available = info.audioStreamIndex >= 0;
                    }
                }
            }
            if (cancel->load()) { project.reset(); message = QStringLiteral("打开已取消。"); }
        }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, path, project = std::move(project), info, available, message] {
            if (!self || self->shuttingDown_) return;
            if (!project) {
                self->finishJobWithoutResults(message);
                emit self->projectOpenFinished(false, path, message);
                return;
            }
            self->applyProject(*project, QFileInfo(path).absoluteFilePath(), info, available);
            self->setStatus(!project->mediaPath.isEmpty() && !self->mediaAvailable()
                ? QStringLiteral("工程已打开，素材离线，请重新定位。")
                : project->schemaVersion < 6 ? QStringLiteral("旧工程已迁移，请核对剪切边界后保存。")
                : QStringLiteral("粗剪工程已打开：%1").arg(path));
            emit self->projectOpenFinished(true, path, self->statusText_);
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::relinkMedia(const QUrl &url)
{
    if (busy_ || shuttingDown_ || projectData_.mediaSha256.size() != 32) {
        setStatus(QStringLiteral("没有可验证的素材身份，请导入新素材。"));
        return;
    }
    const QString path = localPath(url);
    stopWorker();
    cancel_ = false;
    const QPointer<RoughCutController> self(this);
    auto *cancel = &cancel_;
    const RoughCutProject project = projectSnapshot();
    const QString projectPath = projectPath_;
    beginTask(QStringLiteral("正在重新定位素材"), false);
    worker_ = std::thread([self, cancel, path, project, projectPath] {
        QString message;
        const auto hash = RoughCutProjectSerializer::mediaSha256(path, &message, cancel);
        const auto probed = hash == project.mediaSha256 ? MediaProbe::probe(path)
            : ProbeResult(AppError(ErrorDomain::Validation, 1, QStringLiteral("素材内容不同，不能作为重定位使用。")));
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, path, project = RoughCutProject(project), projectPath, probed, message]() mutable {
            if (!self || self->shuttingDown_) return;
            self->setBusy(false);
            if (std::holds_alternative<AppError>(probed)) {
                self->setStatus(message.isEmpty() ? std::get<AppError>(probed).userMessage() : message);
                return;
            }
            const auto saved = self->savedFingerprint_;
            project.mediaPath = QFileInfo(path).absoluteFilePath();
            self->applyProject(project, projectPath, std::get<MediaInfo>(probed), true);
            self->savedFingerprint_ = saved;
            self->refreshModified();
            self->setStatus(QStringLiteral("素材已重新定位，请保存工程。"));
        }, Qt::QueuedConnection);
    });
}

QString RoughCutController::sequenceFrameRate() const
{
    return projectData_.state.value(QStringLiteral("sequenceFrameRate")).toString(QStringLiteral("60/1"));
}

void RoughCutController::setSequenceFrameRate(const QString &rate)
{
    if (busy_ || sequenceFrameRate() == rate) return;
    const QStringList allowed{QStringLiteral("24/1"), QStringLiteral("25/1"), QStringLiteral("30/1"),
        QStringLiteral("50/1"), QStringLiteral("60/1"), QStringLiteral("24000/1001"),
        QStringLiteral("30000/1001"), QStringLiteral("60000/1001")};
    if (!allowed.contains(rate)) { setStatus(QStringLiteral("尚不支持此序列帧率。")); return; }
    projectData_.state.insert(QStringLiteral("sequenceFrameRate"), rate);
    refreshModified();
    emit projectChanged();
}

QString RoughCutController::xmlExportReason() const
{
    if (!mediaAvailable()) return QStringLiteral("素材离线或尚未关联媒体。");
    if (analysisStale()) return QStringLiteral("分析输入已改变，请重新分析后导出。");
    if (!projectData_.state.value(QStringLiteral("timelineFrameError")).toString().isEmpty())
        return projectData_.state.value(QStringLiteral("timelineFrameError")).toString();
    if (channels_ > 2) return QStringLiteral("XML 首期仅支持单声道或立体声。");
    if (hasVideo() && !mediaInfo_.cfrVerified)
        return mediaInfo_.variableFrameRate ? QStringLiteral("源视频为可变帧率，不能导出 XML。")
            : projectData_.state.value(QStringLiteral("frameRateError")).toString(QStringLiteral("正在检查或尚未确认固定帧率。"));
    return {};
}

void RoughCutController::stopFrameRateCheck()
{
    frameRateCancel_ = true;
    if (frameRateWorker_.joinable()) frameRateWorker_.join();
}

void RoughCutController::startFrameRateCheck()
{
    stopFrameRateCheck();
    if (!hasVideo() || mediaInfo_.cfrVerified || !mediaAvailable()) return;
    frameRateCancel_ = false;
    const QString path = mediaPath_;
    const quint64 generation = waveformGeneration_.load();
    const QPointer<RoughCutController> self(this);
    auto *cancel = &frameRateCancel_;
    frameRateWorker_ = std::thread([self, cancel, path, generation] {
        const auto result = MediaProbe::verifyFrameRate(path, cancel);
        if (cancel->load() || !self) return;
        QMetaObject::invokeMethod(self.data(), [self, path, result, generation] {
            if (!self || self->shuttingDown_ || self->mediaPath_ != path || generation != self->waveformGeneration_.load()) return;
            if (std::holds_alternative<MediaInfo>(result)) {
                self->mediaInfo_ = std::get<MediaInfo>(result);
                if (self->mediaInfo_.cfrVerified && self->projectPath_.isEmpty() && !self->timeline_.isEmpty()) {
                    self->rebuildTimeline();
                    self->refreshModified();
                }
                self->setStatus(self->mediaInfo_.cfrVerified ? QStringLiteral("源视频固定帧率已确认。") : QStringLiteral("源视频为可变帧率，XML 导出不可用。"));
            } else {
                self->projectData_.state.insert(QStringLiteral("frameRateError"), std::get<AppError>(result).userMessage());
                self->setStatus(std::get<AppError>(result).userMessage());
            }
            emit self->mediaChanged();
            emit self->canExportChanged();
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::setPreviewItem(QObject *item)
{
    previewItem_ = qobject_cast<VideoPreviewItem *>(item);
}

void RoughCutController::loadScript(const QUrl &url)
{
    if (busy_) return;
    const QString path = localPath(url);
    if (!QFileInfo::exists(path)) {
        setStatus(QStringLiteral("文案不存在。"));
        return;
    }
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在导入文案"), true);
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, canonicalPath, generation] {
        const auto imported = QFileInfo(canonicalPath).suffix().compare(QStringLiteral("docx"), Qt::CaseInsensitive) == 0
            ? ScriptDocumentImporter::loadDocx(canonicalPath, {}, &cancel_)
            : ScriptDocumentImporter::loadTxt(canonicalPath);
        const QPointer<RoughCutController> self(this);
        QMetaObject::invokeMethod(this, [self, canonicalPath, generation, imported] {
            if (!self || generation != self->workerGeneration_) return;
            self->setBusy(false);
            if (self->cancel_.load()) { self->setStatus(QStringLiteral("文案导入已取消。")); return; }
            if (std::holds_alternative<AppError>(imported)) { self->setStatus(std::get<AppError>(imported).userMessage()); return; }
            self->scriptPath_ = canonicalPath;
            self->scriptText_ = scriptDocumentText(std::get<ScriptDocument>(imported));
            self->setStatus(QStringLiteral("已选择文案：%1").arg(QFileInfo(canonicalPath).fileName()));
            emit self->scriptChanged();
            emit self->canExportChanged();
            self->refreshModified();
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::setScriptText(const QString &text)
{
    if (busy_ || scriptText_ == text) return;
    scriptText_ = text;
    scriptPath_.clear();
    emit scriptChanged();
    emit canExportChanged();
    refreshModified();
}

void RoughCutController::setAnalysisOverrides(IAsrService *asr)
{
    asrOverride_ = asr;
}

void RoughCutController::setSourceWaveformItem(QObject *item)
{
    sourceWaveformItem_ = qobject_cast<TimelineSceneItem *>(item);
    syncSceneItems();
}

void RoughCutController::setTimelineItem(QObject *item)
{
    timelineItem_ = qobject_cast<TimelineSceneItem *>(item);
    syncSceneItems();
}

void RoughCutController::startAnalysis()
{
    if (busy_ || shuttingDown_ || mediaPath_.isEmpty()) return;
    const QJsonObject settings = context_->settings;
    const QString apiKey = savedAsrApiKey(context_, settings);
    if (!asrOverride_
        && AsrProviderFactory::providerIdFromSettings(settings) == QLatin1String(kAsrProviderDashScope)
        && apiKey.isEmpty()) {
        setStatus(QStringLiteral("云端 ASR API Key 尚未配置，请在设置中保存凭据。"));
        return;
    }
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在分析"), false);
    stopTimeline();
    setStatus(QStringLiteral("正在识别音频…"));
    const QString mediaPath = mediaPath_;
    const QString scriptPath = scriptPath_;
    const QString scriptText = scriptText_;
    const int sampleRate = sampleRate_;
    const qint64 sourceSampleCount = sourceSampleCount_;
    const QString providerName = asrProviderName(settings);
    const OmniReviewSettings omniSettings = OmniReviewSettingsStore::fromJson(settings);
    IAsrService *asrOverride = asrOverride_;
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, mediaPath, scriptPath, scriptText, sampleRate, sourceSampleCount,
                           settings, apiKey, providerName, omniSettings, asrOverride, generation] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        auto failWithoutCommit = [postUi](const QString &message) {
            postUi([message](RoughCutController *controller) {
                controller->finishJobWithoutResults(message);
            });
        };
        postUi([](RoughCutController *controller) {
            if (!controller->busy_) return;
            controller->setStatus(QStringLiteral("正在分析音频并检测讲话区间…"));
        });
        SpeechAnalysisResult analyzed = SpeechSegmentAnalyzer::analyzeFile(
            mediaPath, sampleRate, sourceSampleCount, &cancel_);
        if (std::holds_alternative<AppError>(analyzed)) {
            const AppError error = std::get<AppError>(analyzed);
            failWithoutCommit(error.domain() == ErrorDomain::Asr
                && error.code() == static_cast<int>(AsrErrorCode::Cancelled)
                ? QStringLiteral("分析已取消。") : error.userMessage());
            return;
        }
        QVector<RecognizedPassage> recording = std::get<QVector<RecognizedPassage>>(std::move(analyzed));
        postUi([providerName](RoughCutController *controller) {
            if (!controller->busy_) return;
            controller->progressPercent_ = 20;
            emit controller->progressChanged();
            controller->setStatus(QStringLiteral("正在使用 %1 进行粗内容识别…")
                .arg(providerName));
        });
        AsrProviderFactory factory;
        std::unique_ptr<IAsrService> ownedAsr;
        IAsrService *asr = asrOverride;
        if (!asr) {
            ownedAsr = factory.create(settings, apiKey);
            asr = ownedAsr.get();
        }
        const AsrResult result = asr->transcribe({mediaPath, &cancel_,
            [postUi](int current, int total) {
                if (total <= 0) return;
                postUi([current, total](RoughCutController *controller) {
                    if (!controller->busy_) return;
                    controller->progressPercent_ = std::clamp(20 + current * 65 / total, 20, 85);
                    emit controller->progressChanged();
                });
            }});
        if (std::holds_alternative<AppError>(result)) {
            const AppError error = std::get<AppError>(result);
            failWithoutCommit(error.domain() == ErrorDomain::Asr
                && error.code() == static_cast<int>(AsrErrorCode::Cancelled)
                ? QStringLiteral("分析已取消。") : error.userMessage());
            return;
        }
        const Transcript transcript = std::get<Transcript>(result);
        recording = RoughCutAlignmentEvidence::buildPassages(
            std::move(recording), transcript, sampleRate, sourceSampleCount);
        postUi([](RoughCutController *controller) {
            if (!controller->busy_) return;
            controller->progressPercent_ = 85;
            emit controller->progressChanged();
            controller->setStatus(QStringLiteral("正在匹配文案与粗剪片段…"));
        });
        if (cancel_.load()) {
            failWithoutCommit(QStringLiteral("分析已取消。"));
            return;
        }
        ScriptDocument script;
        if (!scriptText.isEmpty()) {
            script = scriptDocumentFromText(scriptText);
        } else if (!scriptPath.isEmpty()) {
            const ScriptDocumentResult imported = QFileInfo(scriptPath).suffix().compare(
                QStringLiteral("docx"), Qt::CaseInsensitive) == 0
                ? ScriptDocumentImporter::loadDocx(scriptPath, {}, &cancel_)
                : ScriptDocumentImporter::loadTxt(scriptPath);
            if (std::holds_alternative<AppError>(imported)) {
                failWithoutCommit(std::get<AppError>(imported).userMessage());
                return;
            }
            script = std::get<ScriptDocument>(imported);
        }
        bool matchCancelled = false;
        const QVector<ScriptMatch> matches = ScriptMatcher::match(
            script, recording, &cancel_, &matchCancelled);
        if (matchCancelled || cancel_.load()) {
            failWithoutCommit(QStringLiteral("分析已取消。"));
            return;
        }
        const QVector<RoughCutRetakeGroup> groups = RetakeDetector::detect(recording, matches, sampleRate);
        for (const ScriptMatch &match : matches) {
            if (match.recordingIndex >= 0 && match.recordingIndex < recording.size()) {
                recording[match.recordingIndex].scriptLineIndex = match.scriptLineIndex;
                recording[match.recordingIndex].scriptLineEndIndex = match.scriptLineEndIndex;
                recording[match.recordingIndex].textSimilarity = match.similarity;
                recording[match.recordingIndex].editSimilarity = match.editSimilarity;
                recording[match.recordingIndex].continuousCoverage = match.continuousCoverage;
                recording[match.recordingIndex].scriptTokenStart = match.scriptTokenStart;
                recording[match.recordingIndex].scriptTokenEnd = match.scriptTokenEnd;
            }
        }
        for (const RoughCutRetakeGroup &group : groups) {
            for (const RoughCutTake &take : group.takes) {
                if (take.recordingIndex >= 0 && take.recordingIndex < recording.size())
                    recording[take.recordingIndex].takeGroupId = group.id;
            }
        }
        QVector<bool> trusted;
        trusted.reserve(recording.size());
        for (const RecognizedPassage &passage : recording)
            trusted.append(passage.boundaryTrustworthy && !passage.text.isEmpty());
        QVector<RoughCutSegmentDecision> decisions = RoughCutDecisionEngine::decide(
            recording, matches, groups, trusted, scriptDocumentText(script), false);
        postUi([recording = std::move(recording), decisions = std::move(decisions),
                words = transcript.words, analysisScript = scriptDocumentText(script)](
                   RoughCutController *controller) mutable {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("分析已取消。"));
                return;
            }
            controller->model_.reset(std::move(recording), std::move(decisions),
                controller->sampleRate_, analysisScript);
            controller->projectData_.state.insert(QStringLiteral("analysisScriptText"), analysisScript);
            controller->analysisWords_ = std::move(words);
            ++controller->analysisVersion_;
            controller->auxiliaryResults_.clear();
            controller->history_.clear();
            controller->historyIndex_ = 0;
            controller->rebuildTimeline();
            controller->setBusy(false);
            emit controller->resultsChanged();
            emit controller->canAiReviewChanged();
            emit controller->historyChanged();
            controller->progressPercent_ = 100;
            emit controller->progressChanged();
            controller->refreshModified();
            controller->setStatus(QStringLiteral("分析完成：%1 个片段。")
                .arg(controller->model_.rowCount()));
        });
    });
}

void RoughCutController::cancelAnalysis()
{
    if (!busy_ || cancelling_) return;
    cancelling_ = true;
    cancel_ = true;
    progressIndeterminate_ = true;
    emit busyChanged();
    emit progressChanged();
    setStatus(QStringLiteral("正在取消…"));
}

void RoughCutController::startAiReview()
{
    if (busy_ || model_.rowCount() == 0) return;
    const OmniReviewSettings omniSettings = OmniReviewSettingsStore::fromJson(context_->settings);
    const QString omniKey = OmniReviewSettingsStore::resolveApiKey({}, &context_->credentials);
    if (omniKey.isEmpty()) {
        setStatus(QStringLiteral("未配置 AI API Key。"));
        return;
    }
    RoughCutOmniRequest reviewRequest;
    reviewRequest.mediaPath = mediaPath_;
    reviewRequest.scriptText = scriptText_;
    const QVector<RecognizedPassage> recording = model_.recording();
    const QVector<RoughCutSegmentDecision> decisions = model_.baseDecisions();
    for (int index = 0; index < decisions.size(); ++index) {
        if (decisions.at(index).userDecision) continue;
        if (decisions.at(index).autoDecision == RoughCutDecision::Keep) continue;
        const RecognizedPassage &passage = recording.at(index);
        RoughCutOmniCandidate candidate;
        candidate.candidateId = QString::number(index);
        candidate.startMs = sampleRate_ > 0 ? passage.startSample * 1000 / sampleRate_ : 0;
        candidate.endMs = sampleRate_ > 0 ? passage.endSample * 1000 / sampleRate_ : 0;
        candidate.transcript = passage.text;
        if (index > 0) candidate.previousContext = recording.at(index - 1).text;
        if (index + 1 < recording.size()) candidate.nextContext = recording.at(index + 1).text;
        candidate.reasonHint = decisions.at(index).reason;
        if (decisions.at(index).replacementRecordingIndex >= 0)
            candidate.replacementCandidateId = QString::number(decisions.at(index).replacementRecordingIndex);
        reviewRequest.candidates.append(std::move(candidate));
    }
    if (reviewRequest.candidates.isEmpty()) {
        setStatus(QStringLiteral("没有需要 AI 复核的候选片段。"));
        return;
    }
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在 AI 复核"), true);
    setStatus(QStringLiteral("正在进行 AI 复核…"));
    const QString mediaPath = mediaPath_;
    const int sampleRate = sampleRate_;
    const qint64 sourceSampleCount = sourceSampleCount_;
    const QVector<TranscriptWord> words = analysisWords_;
    const int analysisVersion = analysisVersion_;
    const QVector<RoughCutSegmentDecision> beforeBase = decisions;
    const quint64 generation = ++workerGeneration_;
    IHttpClient *http = context_->aiHttp;
    std::atomic<bool> *cancel = &cancel_;
    worker_ = std::thread([this, reviewRequest, omniSettings, omniKey, mediaPath, sampleRate,
                           sourceSampleCount, words, analysisVersion, beforeBase, generation, http, cancel] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        AiReviewService service(omniSettings, omniKey, http);
        RoughCutOmniResult reviewed = service.reviewCandidates(reviewRequest, cancel);
        const QString model = service.lastModel();
        const OmniUsage usage = service.accumulatedUsage();
        postUi([reviewed = std::move(reviewed), beforeBase, analysisVersion, omniSettings, words,
                sampleRate, sourceSampleCount, model, usage](RoughCutController *controller) mutable {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("AI 复核已取消。"));
                return;
            }
            controller->setBusy(false);
            if (analysisVersion != controller->analysisVersion_
                || controller->mediaPath_.isEmpty()) {
                controller->setStatus(QStringLiteral("分析结果已变化，已丢弃复核结果。"));
                return;
            }
            if (std::holds_alternative<AppError>(reviewed)) {
                controller->setStatus(std::get<AppError>(reviewed).userMessage());
                return;
            }
            QVector<RoughCutSegmentDecision> afterBase = beforeBase;
            int changed = 0;
            for (const RoughCutOmniSuggestion &suggestion :
                 std::get<QVector<RoughCutOmniSuggestion>>(reviewed)) {
                bool ok = false;
                const int index = suggestion.candidateId.toInt(&ok);
                if (!ok || index < 0 || index >= afterBase.size()) continue;
                if (afterBase.at(index).userDecision) continue;
                afterBase[index].autoDecision = suggestion.decision;
                afterBase[index].decisionSource = QStringLiteral("omni");
                afterBase[index].reason = suggestion.reason;
                afterBase[index].modelProbability = suggestion.confidence;
                if (!suggestion.replacementCandidateId.isEmpty()) {
                    bool replacementOk = false;
                    const int replacement = suggestion.replacementCandidateId.toInt(&replacementOk);
                    if (replacementOk) afterBase[index].replacementRecordingIndex = replacement;
                }
                afterBase[index].evidence.append(
                    QStringLiteral("Omni %1 %2")
                        .arg(roughCutOmniReasonName(suggestion.reasonType),
                             QString::number(suggestion.confidence, 'f', 2)));
                ++changed;
            }
            Edit edit;
            edit.beforeBase = beforeBase;
            edit.afterBase = afterBase;
            controller->history_.resize(controller->historyIndex_);
            controller->history_.append(edit);
            ++controller->historyIndex_;
            controller->model_.replaceBaseDecisions(afterBase);
            controller->rebuildTimeline();
            emit controller->historyChanged();
            emit controller->resultsChanged();
            emit controller->canAiReviewChanged();
            controller->progressPercent_ = 100;
            emit controller->progressChanged();
            controller->refreshModified();
            controller->setStatus(QStringLiteral("AI 复核完成：已更新 %1 个片段。 %2")
                .arg(changed)
                .arg(formatOmniReviewSummary(model, usage)));
        });
    });
}

void RoughCutController::startAuxiliaryRecognition()
{
    if (busy_ || mediaPath_.isEmpty() || model_.rowCount() == 0) return;
    const QVector<RoughCutSuspiciousRange> ranges = RoughCutAuxiliaryRecognition::plan(
        model_.recording(), model_.decisions(), sampleRate_, sourceSampleCount_);
    if (ranges.isEmpty()) { setStatus(QStringLiteral("没有需要辅助识别的 REVIEW 片段。")); return; }
    QJsonObject settings = context_->settings;
    if (!settings.value(QStringLiteral("reviewUseSameAsr")).toBool(true)) {
        settings.insert(QStringLiteral("asrProvider"),
            settings.value(QStringLiteral("reviewAsrProvider")).toString(QStringLiteral("funasr")));
        settings.insert(QStringLiteral("asrModel"),
            settings.value(QStringLiteral("reviewAsrModel")).toString(QStringLiteral("Fun-ASR-Nano-2512")));
    }
    const QString apiKey = savedAsrApiKey(context_, settings);
    if (AsrProviderFactory::providerIdFromSettings(settings) == QLatin1String(kAsrProviderDashScope)
        && apiKey.isEmpty()) {
        setStatus(QStringLiteral("云端 ASR API Key 尚未配置，请在设置中保存凭据。"));
        return;
    }
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在辅助识别"), false);
    const QString mediaPath = mediaPath_;
    const int sampleRate = sampleRate_;
    const QString providerName = asrProviderName(settings);
    setStatus(QStringLiteral("正在使用 %1 执行辅助识别…").arg(providerName));
    const QVector<RecognizedPassage> recording = model_.recording();
    const QVector<RoughCutSegmentDecision> decisions = model_.decisions();
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, ranges, mediaPath, sampleRate, settings, apiKey, providerName,
                           recording, decisions, generation] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        QVector<RoughCutAuxiliaryResult> results;
        AsrProviderFactory factory;
        std::unique_ptr<IAsrService> reviewAsr = factory.create(settings, apiKey);
        QTemporaryDir temporary;
        for (int rangeIndex = 0; rangeIndex < ranges.size() && !cancel_; ++rangeIndex) {
            const RoughCutSuspiciousRange &range = ranges.at(rangeIndex);
            QString auxiliaryText;
            bool auxiliaryFailed = true;
            const AudioChunkWindow window{range.startSample / double(sampleRate),
                range.endSample / double(sampleRate), range.startSample * 1000 / sampleRate,
                range.endSample * 1000 / sampleRate};
            const MediaResult<PreparedAudioChunk> extracted = AudioChunkExtractor::extract(
                mediaPath, window, &cancel_, true);
            if (temporary.isValid() && std::holds_alternative<PreparedAudioChunk>(extracted)) {
                const QString clipPath = QDir(temporary.path()).filePath(
                    QStringLiteral("review_%1.flac").arg(rangeIndex));
                QFile clip(clipPath);
                const QByteArray &flac = std::get<PreparedAudioChunk>(extracted).flac;
                if (clip.open(QIODevice::WriteOnly) && clip.write(flac) == flac.size()) {
                    clip.close();
                    const AsrResult recognized = reviewAsr->transcribe({clipPath, &cancel_, {}});
                    if (std::holds_alternative<Transcript>(recognized)) {
                        for (const TranscriptWord &word : std::get<Transcript>(recognized).words)
                            auxiliaryText += word.text;
                        auxiliaryText = auxiliaryText.trimmed();
                        auxiliaryFailed = auxiliaryText.isEmpty();
                    }
                }
            }
            for (int index : range.recordingIndexes) {
                RoughCutAuxiliaryResult result{index, recording.at(index).text, auxiliaryText, {},
                    auxiliaryFailed, false, false};
                result.agreementDecision = decisions.at(index).failureType
                        == RoughCutFailureType::None
                    ? RoughCutDecision::Keep : RoughCutDecision::Cut;
                (void)RoughCutAuxiliaryRecognition::reconcile(
                    decisions.at(index).autoDecision, &result);
                results.append(std::move(result));
            }
            postUi([rangeIndex, total = ranges.size()](RoughCutController *controller) {
                if (!controller->busy_) return;
                controller->progressPercent_ = (rangeIndex + 1) * 100 / total;
                emit controller->progressChanged();
            });
        }
        postUi([results = std::move(results), providerName](RoughCutController *controller) mutable {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("辅助识别已取消。"));
                return;
            }
            controller->auxiliaryResults_ = results;
            controller->model_.applyAuxiliaryResults(controller->auxiliaryResults_, providerName);
            controller->rebuildTimeline();
            controller->progressPercent_ = 100;
            controller->setBusy(false);
            emit controller->progressChanged();
            emit controller->resultsChanged();
            emit controller->canAiReviewChanged();
            controller->refreshModified();
            controller->setStatus(QStringLiteral("辅助识别完成：%1 个片段；一致结果已更新，冲突保持 REVIEW。")
                .arg(controller->auxiliaryResults_.size()));
        });
    });
}

void RoughCutController::togglePlay()
{
    if (!playback_.isOpen()) return;
    if (durationMs() > 0 && positionMs() >= durationMs() - 20) {
        playback_.pause();
        if (context_->audioDevice) context_->audioDevice->pause();
        playback_.seek(MediaTime::fromMilliseconds(0));
    }
    if (playback_.isPaused()) {
        playback_.play();
        playbackTimer_.start();
        if (!playback_.isPriming() && context_->audioDevice) context_->audioDevice->resume();
    } else {
        playback_.pause();
        if (context_->audioDevice) context_->audioDevice->pause();
        playbackTimer_.stop();
    }
    emit playbackChanged();
}

void RoughCutController::setPlaybackRate(double rate)
{
    static constexpr double rates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    const auto found = std::find_if(std::begin(rates), std::end(rates), [rate](double supported) {
        return qAbs(rate - supported) < 0.001;
    });
    if (found == std::end(rates) || qFuzzyCompare(playbackRate_, *found)) return;
    playbackRate_ = *found;
    if (context_->audioDevice) context_->audioDevice->setPlaybackRate(playbackRate_);
    emit playbackChanged();
}

void RoughCutController::seek(qint64 value)
{
    auditionEndSample_ = -1;
    playback_.seek(MediaTime::fromMilliseconds(std::clamp<qint64>(value, 0, durationMs())));
    playback_.pump();
    if (hasVideo() && !playbackTimer_.isActive()) playbackTimer_.start();
    if (previewItem_) {
        const auto frame = playback_.displayedFrame();
        if (!frame.image.isNull()) previewItem_->present(frame.image, frame.pts, frame.generation);
    }
    emit positionChanged();
}

void RoughCutController::seekTimeline(qint64 value)
{
    if (timeline_.isEmpty() || sampleRate_ <= 0) return;
    const qint64 sample = std::max<qint64>(0, value) * sampleRate_ / 1000;
    for (const RoughCutTimelineClip &clip : timeline_) {
        const qint64 end = clip.timelineStartSample + clip.sourceEndSample - clip.sourceStartSample;
        if (sample > end && &clip != &timeline_.constLast()) continue;
        stopTimeline();
        playback_.seek(MediaTime::fromMicroseconds((clip.sourceStartSample
            + std::clamp<qint64>(sample - clip.timelineStartSample, 0,
                clip.sourceEndSample - clip.sourceStartSample)) * 1'000'000 / sampleRate_));
        playback_.pump();
        if (timelineItem_) timelineItem_->setPlayheadUs(std::clamp<qint64>(value * 1000, 0, timelineItem_->durationUs()));
        emit positionChanged();
        return;
    }
}

void RoughCutController::locateResult(int row)
{
    if (!timelineItem_ || row < 0 || row >= model_.rowCount()) return;
    const auto found = std::find_if(timeline_.cbegin(), timeline_.cend(), [row](const RoughCutTimelineClip &clip) {
        return clip.recordingIndex == row;
    });
    if (found == timeline_.cend()) {
        timelineItem_->setSelectedCueId(QString());
        return;
    }
    stopTimeline();
    const qint64 timeUs = found->timelineStartSample * 1'000'000 / sampleRate_;
    timelineItem_->setSelectedCueId(QString::number(row));
    timelineItem_->setPlayheadUs(timeUs);
    timelineItem_->ensureTimeVisible(timeUs);
}

void RoughCutController::audition(int row)
{
    if (row < 0 || row >= model_.recording().size()) return;
    const RecognizedPassage &passage = model_.recording().at(row);
    stopTimeline();
    playback_.seek(MediaTime::fromMicroseconds(passage.startSample * 1'000'000 / sampleRate_));
    auditionEndSample_ = passage.endSample;
    if (playback_.isPaused()) togglePlay();
}

void RoughCutController::playTimeline()
{
    if (timeline_.isEmpty() || !playback_.isOpen()) return;
    qint64 startSample = timelineItem_ ? timelineItem_->playheadUs() * sampleRate_ / 1'000'000 : 0;
    const RoughCutTimelineClip &last = timeline_.constLast();
    if (startSample >= last.timelineStartSample + last.sourceEndSample - last.sourceStartSample) {
        startSample = 0;
        if (timelineItem_) timelineItem_->setPlayheadUs(0);
    }
    stopTimeline();
    timelinePlaybackIndex_ = 0;
    while (timelinePlaybackIndex_ + 1 < timeline_.size()
        && startSample >= timeline_.at(timelinePlaybackIndex_).timelineStartSample
            + timeline_.at(timelinePlaybackIndex_).sourceEndSample
            - timeline_.at(timelinePlaybackIndex_).sourceStartSample)
        ++timelinePlaybackIndex_;
    timelinePlaybackClock_.start();
    playbackTimer_.start();
    startTimelineClip(timelinePlaybackIndex_);
    if (startSample > timeline_.at(timelinePlaybackIndex_).timelineStartSample) {
        const RoughCutTimelineClip &clip = timeline_.at(timelinePlaybackIndex_);
        playback_.seek(MediaTime::fromMicroseconds((clip.sourceStartSample
            + std::clamp<qint64>(startSample - clip.timelineStartSample, 0,
                clip.sourceEndSample - clip.sourceStartSample)) * 1'000'000 / sampleRate_));
    }
    emit playbackChanged();
}

void RoughCutController::toggleTimelinePlay()
{
    if (!timelineActive()) { playTimeline(); return; }
    if (!timelinePaused_) {
        if (timelineGapDeadlineMs_ >= 0)
            timelineGapDeadlineMs_ = std::max<qint64>(0, timelineGapDeadlineMs_ - timelinePlaybackClock_.elapsed());
        playback_.pause();
        if (context_->audioDevice) context_->audioDevice->pause();
        playbackTimer_.stop();
        timelinePaused_ = true;
    } else {
        if (timelineGapDeadlineMs_ >= 0) {
            timelinePlaybackClock_.restart();
        } else {
            playback_.play();
            if (!playback_.isPriming() && context_->audioDevice) context_->audioDevice->resume();
        }
        playbackTimer_.start();
        timelinePaused_ = false;
    }
    emit playbackChanged();
}

void RoughCutController::setContinuousAudition(bool enabled)
{
    if (continuousAudition_ == enabled) return;
    continuousAudition_ = enabled;
    emit playbackChanged();
}

void RoughCutController::stopTimeline()
{
    timelinePlaybackIndex_ = -1;
    timelineGapDeadlineMs_ = -1;
    timelinePaused_ = false;
    auditionEndSample_ = -1;
    if (!playback_.isPaused()) playback_.pause();
    if (context_->audioDevice) context_->audioDevice->pause();
    playbackTimer_.stop();
    emit playbackChanged();
}

void RoughCutController::startTimelineClip(int index)
{
    if (index < 0 || index >= timeline_.size() || sampleRate_ <= 0) { stopTimeline(); return; }
    const RoughCutTimelineClip &clip = timeline_.at(index);
    playback_.seek(MediaTime::fromMicroseconds(clip.sourceStartSample * 1'000'000 / sampleRate_));
    auditionEndSample_ = clip.sourceEndSample;
    playback_.play();
    if (!playback_.isPriming() && context_->audioDevice) context_->audioDevice->resume();
    emit playbackChanged();
}

void RoughCutController::setDecision(int row, const QString &value)
{
    if (busy_) return;
    const std::optional<RoughCutDecision> decision = parseDecision(value);
    if (!decision || row < 0 || row >= model_.decisions().size()) return;
    const Edit edit{row, model_.decisions().at(row).userDecision, decision};
    if (edit.before == edit.after) return;
    history_.resize(historyIndex_);
    history_.append(edit);
    ++historyIndex_;
    applyEdit(edit, true);
}

void RoughCutController::restoreAutoDecision(int row)
{
    if (busy_ || row < 0 || row >= model_.decisions().size() || !model_.decisions().at(row).userDecision) return;
    const Edit edit{row, model_.decisions().at(row).userDecision, std::nullopt};
    history_.resize(historyIndex_); history_.append(edit); ++historyIndex_; applyEdit(edit, true);
}

void RoughCutController::undo()
{
    if (!canUndo()) return;
    applyEdit(history_.at(--historyIndex_), false);
}

void RoughCutController::redo()
{
    if (!canRedo()) return;
    applyEdit(history_.at(historyIndex_++), true);
}

void RoughCutController::exportXml(const QUrl &url)
{
    const QString path = localPath(url);
    QString error = xmlExportReason();
    if (busy_ || timeline_.isEmpty()) error = QStringLiteral("当前没有可导出的粗剪结果或任务正在运行。");
    if (!error.isEmpty() || !safeOutputPath(path, {mediaPath_, scriptPath_, projectPath_}, &error)) {
        setStatus(error);
        emit exportFinished(false, path, error);
        return;
    }
    const auto request = exportRequest(&error);
    if (!error.isEmpty()) { setStatus(error); emit exportFinished(false, path, error); return; }
    const bool saved = XmemlExporter::save(path, request, &error);
    setStatus(saved ? QStringLiteral("XML 已导出：%1").arg(path) : error);
    emit exportFinished(saved, path, statusText_);
}

RoughCutExportRequest RoughCutController::exportRequest(QString *error) const
{
    RoughCutExportRequest request;
    request.mediaPath = mediaPath_;
    request.sampleRate = sampleRate_;
    request.channels = channels_;
    request.sourceSampleCount = sourceSampleCount_;
    request.mediaInfo = mediaInfo_;
    request.sourceRangesAreFrameAligned = projectData_.state.value(QStringLiteral("timelineVersion")).toInt() >= 2;
    request.frameRateNumerator = sequenceFrameRate().section(QLatin1Char('/'), 0, 0).toInt();
    request.frameRateDenominator = sequenceFrameRate().section(QLatin1Char('/'), 1, 1).toInt();
    for (int index = 0; index < timeline_.size(); ++index) {
        const auto &clip = timeline_.at(index);
        RoughCutSourceClip source{clip.text.isEmpty() ? QStringLiteral("Clip %1").arg(index + 1) : clip.text,
            clip.sourceStartSample, clip.sourceEndSample, clip.timelineStartSample};
        source.allowedEndSample = sourceSampleCount_;
        if (clip.recordingIndex >= 0 && clip.recordingIndex < model_.recording().size()) {
            const auto &passage = model_.recording().at(clip.recordingIndex);
            source.protectedStartSample = passage.startSample;
            source.protectedEndSample = passage.endSample;
        }
        // 帧对齐只能借用合法静音，不能跨入已剪除的语音。
        for (int row = 0; row < model_.recording().size(); ++row) {
            if (model_.decisions().at(row).effectiveDecision() != RoughCutDecision::Cut) continue;
            const auto &cut = model_.recording().at(row);
            if (cut.endSample <= clip.sourceStartSample) source.allowedStartSample = std::max(source.allowedStartSample, cut.endSample);
            else if (cut.startSample >= clip.sourceEndSample) source.allowedEndSample = std::min(source.allowedEndSample, cut.startSample);
            else {
                if (error) *error = QStringLiteral("片段 %1 与已剪除语音范围重叠。").arg(index + 1);
                return {};
            }
        }
        request.clips.append(source);
    }
    return request;
}

void RoughCutController::exportWav(const QUrl &url)
{
    const QString path = localPath(url);
    if (busy_ || path.isEmpty() || !mediaAvailable() || analysisStale() || timeline_.isEmpty()) return;
    QString outputError;
    if (!safeOutputPath(path, {mediaPath_, scriptPath_, projectPath_}, &outputError)) {
        setStatus(outputError); emit exportFinished(false, path, outputError); return;
    }
    stopWorker();
    cancel_ = false;
    beginTask(QStringLiteral("正在导出 WAV"), true);
    setStatus(QStringLiteral("正在导出精简 WAV…"));
    const QString sourcePath = mediaPath_;
    const QVector<RoughCutTimelineClip> clips = timeline_;
    const int sampleRate = sampleRate_;
    const int channels = channels_;
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, path, sourcePath, clips, sampleRate, channels, generation] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        QString error;
        const bool saved = RoughCutWavExporter::save(path, sourcePath, clips,
            sampleRate, channels, &cancel_, &error);
        postUi([path, saved, error](RoughCutController *controller) {
            controller->setBusy(false);
            controller->progressPercent_ = saved ? 100 : 0;
            emit controller->progressChanged();
            controller->setStatus(saved ? QStringLiteral("精简 WAV 已导出：%1").arg(path)
                            : error);
            emit controller->exportFinished(saved, path, controller->statusText_);
        });
    });
}

void RoughCutController::stopWorker()
{
    cancel_ = true;
    ++workerGeneration_;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
}

void RoughCutController::stopWaveformWorker()
{
    waveformCancel_ = true;
    ++waveformGeneration_;
    if (waveformWorker_.joinable()) waveformWorker_.join();
    waveformCancel_ = false;
}

void RoughCutController::startWaveformWorker()
{
    stopWaveformWorker();
    const QString path = mediaPath_;
    const quint64 generation = ++waveformGeneration_;
    waveformWorker_ = std::thread([this, path, generation] {
        const WaveformGenerator::Result result = WaveformGenerator().generate(
            path, -1, WaveformGenerator::kDefaultSampleRate, &waveformCancel_,
            &waveformGeneration_, generation);
        if (!std::holds_alternative<std::shared_ptr<const WaveformPyramid>>(result)) return;
        const std::shared_ptr<const WaveformPyramid> waveform =
            std::get<std::shared_ptr<const WaveformPyramid>>(result);
        const QPointer<RoughCutController> self(this);
        QMetaObject::invokeMethod(this, [self, generation, waveform] {
            if (!self || generation != self->waveformGeneration_) return;
            if (self->sourceWaveformItem_) self->sourceWaveformItem_->setWaveform(waveform);
            if (self->timelineItem_) self->timelineItem_->setWaveform(waveform);
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::syncSceneItems()
{
    if (sourceWaveformItem_) {
        sourceWaveformItem_->setDurationUs(durationMs() * 1000);
        sourceWaveformItem_->setPlayheadUs(positionMs() * 1000);
        sourceWaveformItem_->setView(sourceWaveformItem_->pixelsPerMs(), sourceWaveformItem_->scrollOffset());
    }
    if (!timelineItem_) return;
    QList<Subtitle> cues;
    cues.reserve(timeline_.size());
    qint64 endSample = 0;
    for (int index = 0; index < timeline_.size(); ++index) {
        const RoughCutTimelineClip &clip = timeline_.at(index);
        if (clip.decision == RoughCutDecision::Cut) continue;
        const qint64 duration = clip.sourceEndSample - clip.sourceStartSample;
        Subtitle cue;
        cue.id = QString::number(clip.recordingIndex);
        cue.start = MediaTime::fromMicroseconds(clip.timelineStartSample * 1'000'000 / sampleRate_);
        cue.end = MediaTime::fromMicroseconds((clip.timelineStartSample + duration) * 1'000'000 / sampleRate_);
        cue.text = clip.text;
        cue.status = clip.decision == RoughCutDecision::Review ? QStringLiteral("REVIEW") : QStringLiteral("MANUAL");
        cue.metadata.insert(QStringLiteral("sourceStartUs"), clip.sourceStartSample * 1'000'000 / sampleRate_);
        cue.metadata.insert(QStringLiteral("sourceEndUs"), clip.sourceEndSample * 1'000'000 / sampleRate_);
        cues.append(std::move(cue));
        endSample = std::max(endSample, clip.timelineStartSample + duration);
    }
    timelineItem_->setSubtitles(std::move(cues));
    timelineItem_->setEditable(false);
    timelineItem_->setDurationUs(sampleRate_ > 0 ? endSample * 1'000'000 / sampleRate_ : 0);
    timelineItem_->setView(timelineItem_->pixelsPerMs(), timelineItem_->scrollOffset());
}

void RoughCutController::rebuildTimeline()
{
    const OmniReviewSettings omniSettings = OmniReviewSettingsStore::fromJson(
        projectData_.state.value(QStringLiteral("timelineOptions")).toObject());
    auto recording = model_.recording();
    auto decisions = model_.decisions();
    SafeCutBoundary::applyToPassages(&recording, &decisions, analysisWords_, sampleRate_, sourceSampleCount_, omniSettings);
    timeline_ = SafeCutBoundary::buildTimeline(recording, decisions, sampleRate_, sourceSampleCount_, omniSettings);
    projectData_.state.insert(QStringLiteral("timelineVersion"), 1);
    projectData_.state.remove(QStringLiteral("timelineFrameError"));
    if (hasVideo() && mediaInfo_.cfrVerified && !timeline_.isEmpty()) {
        QString error;
        auto request = exportRequest(&error);
        const auto &video = mediaInfo_.streams.at(mediaInfo_.videoStreamIndex);
        request.frameRateNumerator = video.frameRateNumerator;
        request.frameRateDenominator = video.frameRateDenominator;
        const auto ranges = error.isEmpty() ? XmemlExporter::conformSourceRanges(request, &error) : QList<RoughCutSourceClip>{};
        if (ranges.size() == timeline_.size()) {
            const auto original = timeline_;
            qint64 endSample = 0;
            for (int i = 0; i < timeline_.size(); ++i) {
                const qint64 gap = i == 0 ? 0 : std::max<qint64>(0, original[i].timelineStartSample
                    - original[i - 1].timelineStartSample - original[i - 1].sourceEndSample + original[i - 1].sourceStartSample);
                timeline_[i].sourceStartSample = ranges[i].sourceStartSample;
                timeline_[i].sourceEndSample = ranges[i].sourceEndSample;
                timeline_[i].timelineStartSample = endSample + gap;
                endSample = timeline_[i].timelineStartSample + timeline_[i].sourceEndSample - timeline_[i].sourceStartSample;
            }
            projectData_.state.insert(QStringLiteral("timelineVersion"), 2);
        } else projectData_.state.insert(QStringLiteral("timelineFrameError"), error);
    }
    syncSceneItems();
    emit canExportChanged();
}

void RoughCutController::applyEdit(const Edit &edit, bool forward)
{
    if (!edit.beforeBase.isEmpty() || !edit.afterBase.isEmpty()) {
        model_.replaceBaseDecisions(forward ? edit.afterBase : edit.beforeBase, false);
        rebuildTimeline();
        emit historyChanged();
        emit resultsChanged();
        refreshModified();
        return;
    }
    (void)model_.setUserDecision(edit.row, forward ? edit.after : edit.before);
    rebuildTimeline();
    emit historyChanged();
    emit resultsChanged();
    refreshModified();
}

void RoughCutController::setStatus(QString value)
{
    statusText_ = std::move(value);
    emit statusChanged();
}

void RoughCutController::setBusy(bool value)
{
    if (busy_ == value) return;
    busy_ = value;
    if (!value) {
        cancelling_ = false;
        progressIndeterminate_ = false;
        busyTaskTitle_.clear();
    }
    emit busyChanged();
    emit historyChanged();
    emit canAiReviewChanged();
    emit canSaveChanged();
    emit canExportChanged();
    emit progressChanged();
}

void RoughCutController::beginTask(const QString &title, bool indeterminate)
{
    cancelling_ = false;
    busyTaskTitle_ = title;
    progressIndeterminate_ = indeterminate;
    progressPercent_ = 0;
    setBusy(true);
    emit progressChanged();
}

int RoughCutController::sourceResultRow(int filterRow) const
{
    return filterModel_.mapToSource(filterModel_.index(filterRow, 0)).row();
}

int RoughCutController::filterRowForSource(int sourceRow) const
{
    return filterModel_.mapFromSource(model_.index(sourceRow, 0)).row();
}

void RoughCutController::bindSharedAudioDevice()
{
    if (!context_ || !context_->audioDevice || !playback_.isOpen()) return;
    AppError error(ErrorDomain::Media, 0, QString());
    IAudioDevice *device = context_->audioDevice.get();
    if (!device->start(playback_.outputSampleRate(), playback_.outputChannels(), &error)) {
        context_->replaceAudioDevice(createAudioDevice(AudioDeviceKind::Virtual));
        device = context_->audioDevice.get();
        if (device) (void)device->start(playback_.outputSampleRate(), playback_.outputChannels());
    }
    if (!device) return;
    playback_.setAudioDevice(device, 80);
    device->setPlaybackRate(playbackRate_);
    device->pause();
    ownsSharedAudio_ = true;
}

void RoughCutController::releasePlayback()
{
    playback_.pause();
    if (context_ && context_->audioDevice) context_->audioDevice->pause();
    playbackTimer_.stop();
    playback_.setAudioDevice(nullptr, 0);
    ownsSharedAudio_ = false;
    emit playbackChanged();
}

void RoughCutController::claimPlayback()
{
    ownsSharedAudio_ = true;
    bindSharedAudioDevice();
    emit playbackChanged();
}

void RoughCutController::refreshModified()
{
    const bool next = projectFingerprint() != savedFingerprint_;
    if (next == modified_) return;
    modified_ = next;
    emit modifiedChanged();
}

void RoughCutController::markSaved()
{
    savedFingerprint_ = projectFingerprint();
    if (modified_) {
        modified_ = false;
        emit modifiedChanged();
    }
}

QByteArray RoughCutController::projectFingerprint() const
{
    return RoughCutProjectSerializer::fingerprint(projectSnapshot());
}

void RoughCutController::finishJobWithoutResults(const QString &message)
{
    setBusy(false);
    progressPercent_ = 0;
    emit progressChanged();
    setStatus(message);
}

} // namespace subcue
