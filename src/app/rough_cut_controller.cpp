#include "rough_cut_controller.h"

#include "ai/ai_review_service.h"
#include "ai/ai_review_settings.h"
#include "ai/ai_types.h"
#include "application_context.h"
#include "asr/asr_provider_factory.h"
#include "asr/asr_types.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/http_client.h"
#include "media/media_probe.h"
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

QString asrProviderName(const QJsonObject &settings)
{
    const QString provider = AsrProviderFactory::providerIdFromSettings(settings);
    if (provider == QLatin1String("qwen3")) return QStringLiteral("Qwen3-ASR");
    if (provider == QLatin1String("funasr")) return QStringLiteral("Fun-ASR");
    return settings.value(QStringLiteral("asrModel")).toString(QStringLiteral("云端 ASR"));
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
    markSaved();
    playbackTimer_.setInterval(30);
    connect(&playbackTimer_, &QTimer::timeout, this, [this] {
        const bool wasPriming = playback_.isPriming();
        playback_.pump();
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
            && playback_.position().microseconds() * sampleRate_ / 1'000'000 >= auditionEndSample_) {
            if (timelinePlaybackIndex_ >= 0) {
                playback_.pause();
                if (context_->audioDevice) context_->audioDevice->pause();
                const int finished = timelinePlaybackIndex_++;
                auditionEndSample_ = -1;
                if (timelinePlaybackIndex_ >= timeline_.size() || !continuousAudition_) {
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
                togglePlay();
                auditionEndSample_ = -1;
            }
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
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0;
    emit progressChanged();
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
        if (info.audioStreamIndex < 0 || info.audioStreamIndex >= info.streams.size()) {
            postUi([](RoughCutController *controller) {
                controller->finishJobWithoutResults(QStringLiteral("所选文件没有音频轨。"));
            });
            return;
        }
        postUi([path, info](RoughCutController *controller) {
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
            controller->analysisWords_.clear();
            controller->sampleRate_ = stream.sampleRate;
            controller->channels_ = stream.channels;
            controller->sourceSampleCount_ = std::max<qint64>(
                0, info.duration.microseconds() * controller->sampleRate_ / 1'000'000);
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
            controller->startWaveformWorker();
        });
    });
}

bool RoughCutController::saveProject(const QUrl &url)
{
    const QString path = localPath(url);
    if (busy_ || shuttingDown_ || path.isEmpty() || mediaPath_.isEmpty()) {
        if (mediaPath_.isEmpty()) setStatus(QStringLiteral("请先选择完整 WAV，再保存粗剪工程。"));
        return false;
    }
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0;
    emit progressChanged();
    setStatus(QStringLiteral("正在校验媒体并保存工程…"));
    RoughCutProject project;
    project.mediaPath = mediaPath_;
    project.scriptPath = scriptPath_;
    project.scriptText = scriptText_;
    project.sampleRate = sampleRate_;
    project.channels = channels_;
    project.sourceSampleCount = sourceSampleCount_;
    project.analysisVersion = std::max(1, analysisVersion_);
    project.recording = model_.recording();
    project.decisions = model_.decisions();
    project.auxiliaryResults = auxiliaryResults_;
    const QString mediaPath = mediaPath_;
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, path, project, mediaPath, generation]() mutable {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        QString error;
        project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(mediaPath, &error, &cancel_,
            [postUi](qint64 done, qint64 total) {
                if (total <= 0) return;
                postUi([done, total](RoughCutController *controller) {
                    if (!controller->busy_) return;
                    controller->progressPercent_ = std::clamp(int(done * 90 / total), 0, 90);
                    emit controller->progressChanged();
                });
            });
        if (project.mediaSha256.isEmpty()) {
            postUi([error](RoughCutController *controller) {
                controller->finishJobWithoutResults(error);
            });
            return;
        }
        if (!RoughCutProjectSerializer::save(path, project, &error)) {
            postUi([error](RoughCutController *controller) {
                controller->finishJobWithoutResults(error);
            });
            return;
        }
        postUi([path](RoughCutController *controller) {
            controller->projectPath_ = QFileInfo(path).absoluteFilePath();
            emit controller->projectChanged();
            controller->markSaved();
            controller->setBusy(false);
            controller->progressPercent_ = 100;
            emit controller->progressChanged();
            controller->setStatus(QStringLiteral("粗剪工程已保存：%1").arg(controller->projectPath_));
        });
    });
    return true;
}

bool RoughCutController::saveCurrentProject()
{
    if (projectPath_.isEmpty()) return false;
    return saveProject(QUrl::fromLocalFile(projectPath_));
}

void RoughCutController::openProject(const QUrl &url)
{
    if (busy_ || shuttingDown_) return;
    const QString path = localPath(url);
    if (path.isEmpty()) return;
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0;
    emit progressChanged();
    setStatus(QStringLiteral("正在校验工程与媒体…"));
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, path, generation] {
        auto postUi = [this, generation](auto fn) {
            const QPointer<RoughCutController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, fn = std::move(fn)]() mutable {
                if (!self || generation != self->workerGeneration_) return;
                fn(self.data());
            }, Qt::QueuedConnection);
        };
        QString error;
        const std::optional<RoughCutProject> project = RoughCutProjectSerializer::load(path, &error);
        if (!project) {
            postUi([error](RoughCutController *controller) {
                controller->finishJobWithoutResults(error);
            });
            return;
        }
        const QByteArray currentHash = RoughCutProjectSerializer::mediaSha256(
            project->mediaPath, &error, &cancel_,
            [postUi](qint64 done, qint64 total) {
                if (total <= 0) return;
                postUi([done, total](RoughCutController *controller) {
                    if (!controller->busy_) return;
                    controller->progressPercent_ = std::clamp(int(done * 80 / total), 0, 80);
                    emit controller->progressChanged();
                });
            });
        if (currentHash.isEmpty()) {
            postUi([error](RoughCutController *controller) {
                controller->finishJobWithoutResults(error == QStringLiteral("校验已取消。")
                    ? error : QStringLiteral("工程源媒体不可用：%1").arg(error));
            });
            return;
        }
        if (currentHash != project->mediaSha256) {
            postUi([](RoughCutController *controller) {
                controller->finishJobWithoutResults(
                    QStringLiteral("源媒体内容已变化，当前工程未改动，请重新选择媒体并分析。"));
            });
            return;
        }
        const ProbeResult probed = MediaProbe::probe(project->mediaPath);
        if (std::holds_alternative<AppError>(probed)) {
            postUi([message = std::get<AppError>(probed).userMessage()](RoughCutController *controller) {
                controller->finishJobWithoutResults(message);
            });
            return;
        }
        const MediaInfo info = std::get<MediaInfo>(probed);
        if (info.audioStreamIndex < 0 || info.audioStreamIndex >= info.streams.size()) {
            postUi([](RoughCutController *controller) {
                controller->finishJobWithoutResults(QStringLiteral("所选文件没有音频轨。"));
            });
            return;
        }
        postUi([path, project = *project, info](RoughCutController *controller) mutable {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("校验已取消。"));
                return;
            }
            AppError openError(ErrorDomain::Media, 0, QString());
            controller->stopWaveformWorker();
            if (controller->sourceWaveformItem_) controller->sourceWaveformItem_->setWaveform({});
            controller->playback_.close();
            if (!controller->playback_.open(project.mediaPath, &openError)) {
                controller->finishJobWithoutResults(openError.userMessage());
                return;
            }
            if (controller->ownsSharedAudio_) controller->bindSharedAudioDevice();
            const MediaStreamInfo stream = info.streams.at(info.audioStreamIndex);
            controller->mediaPath_ = QFileInfo(project.mediaPath).canonicalFilePath();
            controller->analysisWords_.clear();
            controller->sampleRate_ = stream.sampleRate;
            controller->channels_ = stream.channels;
            controller->sourceSampleCount_ = std::max<qint64>(
                0, info.duration.microseconds() * controller->sampleRate_ / 1'000'000);
            controller->scriptPath_ = project.scriptPath;
            controller->scriptText_ = project.scriptText;
            controller->projectPath_ = QFileInfo(path).absoluteFilePath();
            controller->analysisVersion_ = project.analysisVersion;
            controller->auxiliaryResults_ = project.auxiliaryResults;
            controller->model_.reset(project.recording, project.decisions,
                controller->sampleRate_, controller->scriptText_);
            controller->history_.clear();
            controller->historyIndex_ = 0;
            controller->rebuildTimeline();
            controller->setBusy(false);
            controller->progressPercent_ = 100;
            emit controller->progressChanged();
            emit controller->mediaChanged();
            emit controller->scriptChanged();
            emit controller->projectChanged();
            emit controller->resultsChanged();
            emit controller->canAiReviewChanged();
            emit controller->historyChanged();
            emit controller->canSaveChanged();
            controller->markSaved();
            controller->setStatus(QStringLiteral("粗剪工程已打开：%1").arg(controller->projectPath_));
            controller->startWaveformWorker();
        });
    });
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
    const ScriptDocumentResult imported = QFileInfo(canonicalPath).suffix().compare(
        QStringLiteral("docx"), Qt::CaseInsensitive) == 0
        ? ScriptDocumentImporter::loadDocx(canonicalPath)
        : ScriptDocumentImporter::loadTxt(canonicalPath);
    if (std::holds_alternative<AppError>(imported)) {
        setStatus(std::get<AppError>(imported).userMessage());
        return;
    }
    scriptPath_ = canonicalPath;
    scriptText_ = scriptDocumentText(std::get<ScriptDocument>(imported));
    setStatus(QStringLiteral("已选择文案：%1").arg(QFileInfo(scriptPath_).fileName()));
    emit scriptChanged();
    refreshModified();
}

void RoughCutController::setScriptText(const QString &text)
{
    if (busy_ || scriptText_ == text) return;
    scriptText_ = text;
    scriptPath_.clear();
    emit scriptChanged();
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
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0; emit progressChanged();
    stopTimeline();
    setStatus(QStringLiteral("正在识别音频…"));
    const QString mediaPath = mediaPath_;
    const QString scriptPath = scriptPath_;
    const QString scriptText = scriptText_;
    const int sampleRate = sampleRate_;
    const qint64 sourceSampleCount = sourceSampleCount_;
    const QJsonObject settings = context_->settings;
    const QString providerName = asrProviderName(settings);
    const OmniReviewSettings omniSettings = OmniReviewSettingsStore::fromJson(settings);
    IAsrService *asrOverride = asrOverride_;
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, mediaPath, scriptPath, scriptText, sampleRate, sourceSampleCount,
                           settings, providerName, omniSettings, asrOverride, generation] {
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
            ownedAsr = factory.create(settings, QString());
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
            recording, matches, groups, trusted, scriptDocumentText(script));
        RoughCutDecisionEngine::protectCuts(recording, &decisions, scriptDocumentText(script));
        SafeCutBoundary::applyToPassages(
            &recording, &decisions, transcript.words, sampleRate, sourceSampleCount, omniSettings);
        postUi([recording = std::move(recording), decisions = std::move(decisions),
                words = transcript.words](
                   RoughCutController *controller) mutable {
            if (controller->cancel_) {
                controller->finishJobWithoutResults(QStringLiteral("分析已取消。"));
                return;
            }
            controller->model_.reset(std::move(recording), std::move(decisions),
                controller->sampleRate_, controller->scriptText_);
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
    if (!busy_) return;
    cancel_ = true;
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
    setBusy(true);
    progressPercent_ = 0;
    emit progressChanged();
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
            QVector<RecognizedPassage> recording = controller->model_.recording();
            RoughCutDecisionEngine::protectCuts(recording, &afterBase, controller->scriptText_);
            SafeCutBoundary::applyToPassages(
                &recording, &afterBase, words, sampleRate, sourceSampleCount, omniSettings);
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
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0; emit progressChanged();
    const QString mediaPath = mediaPath_;
    const int sampleRate = sampleRate_;
    QJsonObject settings = context_->settings;
    if (!settings.value(QStringLiteral("reviewUseSameAsr")).toBool(true)) {
        settings.insert(QStringLiteral("asrProvider"),
            settings.value(QStringLiteral("reviewAsrProvider")).toString(QStringLiteral("funasr")));
        settings.insert(QStringLiteral("asrModel"),
            settings.value(QStringLiteral("reviewAsrModel")).toString(QStringLiteral("Fun-ASR-Nano-2512")));
    }
    const QString providerName = asrProviderName(settings);
    setStatus(QStringLiteral("正在使用 %1 执行辅助识别…").arg(providerName));
    const QVector<RecognizedPassage> recording = model_.recording();
    const QVector<RoughCutSegmentDecision> decisions = model_.decisions();
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, ranges, mediaPath, sampleRate, settings, providerName,
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
        std::unique_ptr<IAsrService> reviewAsr = factory.create(settings, QString());
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
    const qint64 startSample = timelineItem_ ? timelineItem_->playheadUs() * sampleRate_ / 1'000'000 : 0;
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
    RoughCutExportRequest request;
    request.mediaPath = mediaPath_;
    request.sampleRate = sampleRate_;
    request.channels = channels_;
    request.sourceSampleCount = sourceSampleCount_;
    for (int index = 0; index < timeline_.size(); ++index) {
        const RoughCutTimelineClip &clip = timeline_.at(index);
        request.clips.append({clip.text.isEmpty() ? QStringLiteral("Clip %1").arg(index + 1) : clip.text,
                              clip.sourceStartSample, clip.sourceEndSample,
                              clip.timelineStartSample});
    }
    QString error;
    if (!XmemlExporter::save(localPath(url), request, &error)) setStatus(error);
    else setStatus(QStringLiteral("XML 已导出：%1").arg(localPath(url)));
}

void RoughCutController::exportWav(const QUrl &url)
{
    const QString path = localPath(url);
    if (busy_ || path.isEmpty() || mediaPath_.isEmpty() || timeline_.isEmpty()) return;
    stopWorker();
    cancel_ = false;
    setBusy(true);
    progressPercent_ = 0;
    emit progressChanged();
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
    timelineItem_->clearCues();
    qint64 endSample = 0;
    for (int index = 0; index < timeline_.size(); ++index) {
        const RoughCutTimelineClip &clip = timeline_.at(index);
        const qint64 duration = clip.sourceEndSample - clip.sourceStartSample;
        timelineItem_->addCue(QString::number(clip.recordingIndex),
            clip.timelineStartSample * 1'000'000 / sampleRate_,
            (clip.timelineStartSample + duration) * 1'000'000 / sampleRate_, clip.text);
        endSample = std::max(endSample, clip.timelineStartSample + duration);
    }
    timelineItem_->setDurationUs(sampleRate_ > 0 ? endSample * 1'000'000 / sampleRate_ : 0);
    timelineItem_->setView(timelineItem_->pixelsPerMs(), timelineItem_->scrollOffset());
}

void RoughCutController::rebuildTimeline()
{
    const OmniReviewSettings omniSettings = OmniReviewSettingsStore::fromJson(context_->settings);
    timeline_ = SafeCutBoundary::buildTimeline(
        model_.recording(), model_.decisions(), sampleRate_, sourceSampleCount_, omniSettings);
    syncSceneItems();
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
    emit busyChanged();
    emit historyChanged();
    emit canAiReviewChanged();
    emit canSaveChanged();
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
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << mediaPath_ << scriptPath_ << scriptText_ << analysisVersion_;
    const auto &recording = model_.recording();
    const auto &decisions = model_.decisions();
    stream << int(recording.size());
    for (int index = 0; index < recording.size(); ++index) {
        const RecognizedPassage &passage = recording.at(index);
        stream << passage.id << passage.text << passage.startSample << passage.endSample;
        if (index < decisions.size()) {
            const RoughCutSegmentDecision &decision = decisions.at(index);
            stream << int(decision.autoDecision)
                   << (decision.userDecision ? int(*decision.userDecision) : -1)
                   << decision.reason;
        }
    }
    stream << int(auxiliaryResults_.size());
    for (const RoughCutAuxiliaryResult &result : auxiliaryResults_)
        stream << result.recordingIndex << result.funAsrText << result.conflict;
    return bytes;
}

void RoughCutController::finishJobWithoutResults(const QString &message)
{
    setBusy(false);
    progressPercent_ = 0;
    emit progressChanged();
    setStatus(message);
}

} // namespace subcue
