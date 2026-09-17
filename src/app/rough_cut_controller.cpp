#include "rough_cut_controller.h"

#include "application_context.h"
#include "asr/asr_provider_factory.h"
#include "media/media_probe.h"
#include "roughcut/retake_detector.h"
#include "roughcut/rough_cut_project.h"
#include "roughcut/script_document.h"
#include "roughcut/speech_segment_analyzer.h"
#include "roughcut/xmeml_exporter.h"
#include "roughcut/wav_exporter.h"
#include "timeline_scene_item.h"
#include "waveform/waveform_generator.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>

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

QVector<RecognizedPassage> attachTranscriptToSegments(
    QVector<RecognizedPassage> segments, const Transcript &transcript, int sampleRate)
{
    for (RecognizedPassage &segment : segments) {
        for (const TranscriptWord &word : transcript.words) {
            if (word.text.trimmed().isEmpty() || word.endMs <= word.startMs) continue;
            const qint64 midpoint = (word.startMs + word.endMs) * sampleRate / 2000;
            if (midpoint >= segment.startSample && midpoint < segment.endSample)
                segment.text += word.text;
        }
        segment.text = segment.text.trimmed();
        if (segment.text.isEmpty()) segment.boundaryTrustworthy = false;
    }
    return segments;
}

} // namespace

RoughCutController::RoughCutController(ApplicationContext *context, QObject *parent)
    : QObject(parent), context_(context)
{
    playbackTimer_.setInterval(30);
    connect(&playbackTimer_, &QTimer::timeout, this, [this] {
        playback_.pump();
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
    stopWorker();
    stopWaveformWorker();
    playback_.close();
}

qint64 RoughCutController::positionMs() const { return playback_.position().milliseconds(); }
qint64 RoughCutController::durationMs() const
{
    return sampleRate_ > 0 ? sourceSampleCount_ * 1000 / sampleRate_ : 0;
}

void RoughCutController::loadMedia(const QUrl &url)
{
    const QString path = localPath(url);
    const ProbeResult probed = MediaProbe::probe(path);
    if (std::holds_alternative<AppError>(probed)) {
        setStatus(std::get<AppError>(probed).userMessage());
        return;
    }
    const MediaInfo info = std::get<MediaInfo>(probed);
    if (info.audioStreamIndex < 0 || info.audioStreamIndex >= info.streams.size()) {
        setStatus(QStringLiteral("所选文件没有音频轨。"));
        return;
    }
    const MediaStreamInfo stream = info.streams.at(info.audioStreamIndex);
    AppError error(ErrorDomain::Media, 0, QString());
    stopWorker();
    if (busy_) { busy_ = false; emit busyChanged(); }
    progressPercent_ = 0; emit progressChanged();
    stopWaveformWorker();
    if (sourceWaveformItem_) sourceWaveformItem_->setWaveform({});
    playback_.close();
    if (!playback_.open(path, &error)) {
        setStatus(error.userMessage());
        return;
    }
    if (context_->audioDevice) {
        if (!context_->audioDevice->start(playback_.outputSampleRate(), playback_.outputChannels(), &error)) {
            context_->audioDevice = createAudioDevice(AudioDeviceKind::Virtual);
            (void)context_->audioDevice->start(playback_.outputSampleRate(), playback_.outputChannels());
        }
        playback_.setAudioDevice(context_->audioDevice.get(), 80);
        context_->audioDevice->setPlaybackRate(playbackRate_);
        context_->audioDevice->pause();
    }
    mediaPath_ = QFileInfo(path).canonicalFilePath();
    sampleRate_ = stream.sampleRate;
    channels_ = stream.channels;
    sourceSampleCount_ = std::max<qint64>(0, info.duration.microseconds() * sampleRate_ / 1'000'000);
    model_.reset({}, {}, sampleRate_);
    timeline_.clear();
    history_.clear();
    historyIndex_ = 0;
    projectPath_.clear();
    analysisVersion_ = 0;
    auxiliaryResults_.clear();
    setStatus(QStringLiteral("已加载：%1").arg(QFileInfo(mediaPath_).fileName()));
    emit mediaChanged();
    emit resultsChanged();
    emit historyChanged();
    emit projectChanged();
    startWaveformWorker();
}

void RoughCutController::saveProject(const QUrl &url)
{
    const QString path = localPath(url);
    if (path.isEmpty() || mediaPath_.isEmpty() || model_.rowCount() == 0) return;
    QString error;
    const QByteArray hash = RoughCutProjectSerializer::mediaSha256(mediaPath_, &error);
    if (hash.isEmpty()) { setStatus(error); return; }
    RoughCutProject project;
    project.mediaPath = mediaPath_;
    project.mediaSha256 = hash;
    project.scriptPath = scriptPath_;
    project.scriptText = scriptText_;
    project.sampleRate = sampleRate_;
    project.channels = channels_;
    project.sourceSampleCount = sourceSampleCount_;
    project.analysisVersion = std::max(1, analysisVersion_);
    project.recording = model_.recording();
    project.decisions = model_.decisions();
    project.auxiliaryResults = auxiliaryResults_;
    if (!RoughCutProjectSerializer::save(path, project, &error)) { setStatus(error); return; }
    projectPath_ = QFileInfo(path).absoluteFilePath();
    emit projectChanged();
    setStatus(QStringLiteral("粗剪工程已保存：%1").arg(projectPath_));
}

void RoughCutController::openProject(const QUrl &url)
{
    const QString path = localPath(url);
    QString error;
    const std::optional<RoughCutProject> project = RoughCutProjectSerializer::load(path, &error);
    if (!project) { setStatus(error); return; }
    const QByteArray currentHash = RoughCutProjectSerializer::mediaSha256(project->mediaPath, &error);
    if (currentHash.isEmpty()) { setStatus(QStringLiteral("工程源媒体不可用：%1").arg(error)); return; }
    if (currentHash != project->mediaSha256) {
        model_.reset({}, {}, project->sampleRate);
        timeline_.clear();
        syncSceneItems();
        setStatus(QStringLiteral("源媒体内容已变化，旧切点已暂停使用，请重新选择媒体并分析。"));
        emit resultsChanged();
        return;
    }
    loadMedia(QUrl::fromLocalFile(project->mediaPath));
    if (mediaPath_.isEmpty()) return;
    scriptPath_ = project->scriptPath;
    scriptText_ = project->scriptText;
    projectPath_ = QFileInfo(path).absoluteFilePath();
    analysisVersion_ = project->analysisVersion;
    auxiliaryResults_ = project->auxiliaryResults;
    model_.reset(project->recording, project->decisions, sampleRate_);
    history_.clear();
    historyIndex_ = 0;
    rebuildTimeline();
    emit scriptChanged();
    emit projectChanged();
    emit resultsChanged();
    emit historyChanged();
    setStatus(QStringLiteral("粗剪工程已打开：%1").arg(projectPath_));
}

void RoughCutController::loadScript(const QUrl &url)
{
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
}

void RoughCutController::setScriptText(const QString &text)
{
    if (scriptText_ == text) return;
    scriptText_ = text;
    scriptPath_.clear();
    emit scriptChanged();
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
    if (busy_ || mediaPath_.isEmpty()) return;
    stopWorker();
    cancel_ = false;
    busy_ = true;
    emit busyChanged();
    progressPercent_ = 0; emit progressChanged();
    stopTimeline();
    model_.reset({}, {}, sampleRate_);
    timeline_.clear();
    auxiliaryResults_.clear();
    history_.clear();
    historyIndex_ = 0;
    syncSceneItems();
    emit resultsChanged();
    emit historyChanged();
    setStatus(QStringLiteral("正在识别音频…"));
    const QString mediaPath = mediaPath_;
    const QString scriptPath = scriptPath_;
    const QString scriptText = scriptText_;
    const int sampleRate = sampleRate_;
    const qint64 sourceSampleCount = sourceSampleCount_;
    const QJsonObject settings = context_->settings;
    const QString providerName = asrProviderName(settings);
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, mediaPath, scriptPath, scriptText, sampleRate, sourceSampleCount,
                           settings, providerName, generation] {
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation != workerGeneration_ || !busy_) return;
            setStatus(QStringLiteral("正在分析音频并检测讲话区间…"));
        }, Qt::QueuedConnection);
        SpeechAnalysisResult analyzed = SpeechSegmentAnalyzer::analyzeFile(
            mediaPath, sampleRate, sourceSampleCount, &cancel_);
        if (std::holds_alternative<AppError>(analyzed)) {
            const QString message = std::get<AppError>(analyzed).userMessage();
            QMetaObject::invokeMethod(this, [this, message, generation] {
                if (generation != workerGeneration_) return;
                busy_ = false; emit busyChanged(); setStatus(message);
            }, Qt::QueuedConnection);
            return;
        }
        QVector<RecognizedPassage> recording = std::get<QVector<RecognizedPassage>>(std::move(analyzed));
        QMetaObject::invokeMethod(this, [this, providerName, generation] {
            if (generation != workerGeneration_ || !busy_) return;
            progressPercent_ = 20; emit progressChanged();
            setStatus(QStringLiteral("正在使用 %1 进行粗内容识别…")
                .arg(providerName));
        }, Qt::QueuedConnection);
        AsrProviderFactory factory;
        std::unique_ptr<IAsrService> asr = factory.create(settings, QString());
        const AsrResult result = asr->transcribe({mediaPath, &cancel_,
            [this, generation](int current, int total) {
                if (total <= 0) return;
                QMetaObject::invokeMethod(this, [this, generation, current, total] {
                    if (generation != workerGeneration_ || !busy_) return;
                    progressPercent_ = std::clamp(20 + current * 65 / total, 20, 85);
                    emit progressChanged();
                }, Qt::QueuedConnection);
            }});
        if (std::holds_alternative<AppError>(result)) {
            const QString message = std::get<AppError>(result).userMessage();
            QMetaObject::invokeMethod(this, [this, message, generation] {
                if (generation != workerGeneration_) return;
                busy_ = false; emit busyChanged(); setStatus(message);
            }, Qt::QueuedConnection);
            return;
        }
        recording = attachTranscriptToSegments(
            std::move(recording), std::get<Transcript>(result), sampleRate);
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation != workerGeneration_ || !busy_) return;
            progressPercent_ = 85; emit progressChanged();
            setStatus(QStringLiteral("正在匹配文案与粗剪片段…"));
        }, Qt::QueuedConnection);
        ScriptDocument script;
        if (!scriptText.isEmpty()) {
            script = scriptDocumentFromText(scriptText);
        } else if (!scriptPath.isEmpty()) {
            const ScriptDocumentResult imported = QFileInfo(scriptPath).suffix().compare(
                QStringLiteral("docx"), Qt::CaseInsensitive) == 0
                ? ScriptDocumentImporter::loadDocx(scriptPath, {}, &cancel_)
                : ScriptDocumentImporter::loadTxt(scriptPath);
            if (std::holds_alternative<AppError>(imported)) {
                const QString message = std::get<AppError>(imported).userMessage();
                QMetaObject::invokeMethod(this, [this, message, generation] {
                    if (generation != workerGeneration_) return;
                    busy_ = false; emit busyChanged(); setStatus(message);
                }, Qt::QueuedConnection);
                return;
            }
            script = std::get<ScriptDocument>(imported);
        }
        const QVector<ScriptMatch> matches = ScriptMatcher::match(script, recording);
        const QVector<RoughCutRetakeGroup> groups = RetakeDetector::detect(recording, matches, sampleRate);
        for (const ScriptMatch &match : matches) {
            if (match.recordingIndex >= 0 && match.recordingIndex < recording.size())
                recording[match.recordingIndex].scriptLineIndex = match.scriptLineIndex;
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
            recording, matches, groups, trusted);
        QMetaObject::invokeMethod(this, [this, recording = std::move(recording),
                                        decisions = std::move(decisions), generation]() mutable {
            if (generation != workerGeneration_) return;
            if (cancel_) { busy_ = false; emit busyChanged(); setStatus(QStringLiteral("分析已取消。")); return; }
            model_.reset(std::move(recording), std::move(decisions), sampleRate_);
            ++analysisVersion_;
            auxiliaryResults_.clear();
            history_.clear(); historyIndex_ = 0; rebuildTimeline();
            busy_ = false; emit busyChanged(); emit resultsChanged(); emit historyChanged();
            progressPercent_ = 100; emit progressChanged();
            setStatus(QStringLiteral("分析完成：%1 个片段。").arg(model_.rowCount()));
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::cancelAnalysis()
{
    if (!busy_) return;
    cancel_ = true;
    setStatus(QStringLiteral("正在取消…"));
}

void RoughCutController::startAuxiliaryRecognition()
{
    if (busy_ || mediaPath_.isEmpty() || model_.rowCount() == 0) return;
    const QVector<RoughCutSuspiciousRange> ranges = RoughCutAuxiliaryRecognition::plan(
        model_.recording(), model_.decisions(), sampleRate_, sourceSampleCount_);
    if (ranges.isEmpty()) { setStatus(QStringLiteral("没有需要辅助识别的 REVIEW 片段。")); return; }
    stopWorker();
    cancel_ = false; busy_ = true; emit busyChanged();
    progressPercent_ = 0; emit progressChanged();
    const QString mediaPath = mediaPath_;
    const int sampleRate = sampleRate_;
    const QJsonObject settings = context_->settings;
    const QString providerName = asrProviderName(settings);
    setStatus(QStringLiteral("正在使用 %1 执行辅助识别…").arg(providerName));
    const QVector<RecognizedPassage> recording = model_.recording();
    const QVector<RoughCutSegmentDecision> decisions = model_.decisions();
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, ranges, mediaPath, sampleRate, settings, providerName,
                           recording, decisions, generation] {
        QVector<RoughCutAuxiliaryResult> results;
        AsrProviderFactory factory;
        const AsrResult recognized = factory.create(settings, QString())->transcribe({mediaPath, &cancel_,
            [this, generation](int current, int total) {
                if (total <= 0) return;
                QMetaObject::invokeMethod(this, [this, generation, current, total] {
                    if (generation != workerGeneration_ || !busy_) return;
                    progressPercent_ = std::clamp(current * 90 / total, 0, 90);
                    emit progressChanged();
                }, Qt::QueuedConnection);
            }});
        if (std::holds_alternative<AppError>(recognized)) {
            const QString message = std::get<AppError>(recognized).userMessage();
            QMetaObject::invokeMethod(this, [this, message, generation] {
                if (generation != workerGeneration_) return;
                busy_ = false;
                progressPercent_ = 0;
                emit busyChanged();
                emit progressChanged();
                setStatus(message);
            }, Qt::QueuedConnection);
            return;
        }
        const Transcript transcript = std::get<Transcript>(recognized);
        for (int rangeIndex = 0; rangeIndex < ranges.size() && !cancel_; ++rangeIndex) {
            const RoughCutSuspiciousRange &range = ranges.at(rangeIndex);
            const QString auxiliaryText = transcriptText(transcript,
                range.startSample * 1000 / sampleRate, range.endSample * 1000 / sampleRate);
            const bool auxiliaryFailed = auxiliaryText.isEmpty();
            for (int index : range.recordingIndexes) {
                RoughCutAuxiliaryResult result{index, recording.at(index).text, auxiliaryText, {},
                    auxiliaryFailed, false, false};
                (void)RoughCutAuxiliaryRecognition::reconcile(
                    decisions.at(index).autoDecision, &result);
                results.append(std::move(result));
            }
            QMetaObject::invokeMethod(this, [this, generation, rangeIndex, total = ranges.size()] {
                if (generation != workerGeneration_ || !busy_) return;
                progressPercent_ = 90 + (rangeIndex + 1) * 10 / total;
                emit progressChanged();
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this, results = std::move(results), providerName,
                                        generation]() mutable {
            if (generation != workerGeneration_) return;
            if (!cancel_) {
                auxiliaryResults_ = results;
                for (const auto &result : auxiliaryResults_)
                    model_.applyAuxiliaryResult(result, providerName);
                rebuildTimeline();
                progressPercent_ = 100;
                setStatus(QStringLiteral("辅助识别完成：%1 个片段；冲突或失败均保持 REVIEW。")
                    .arg(auxiliaryResults_.size()));
            } else {
                progressPercent_ = 0;
                setStatus(QStringLiteral("辅助识别已取消。"));
            }
            busy_ = false; emit busyChanged(); emit progressChanged(); emit resultsChanged();
        }, Qt::QueuedConnection);
    });
}

void RoughCutController::togglePlay()
{
    if (!playback_.isOpen()) return;
    if (playback_.isPaused()) {
        playback_.play();
        (void)playback_.primeAudio();
        if (context_->audioDevice) context_->audioDevice->resume();
        playbackTimer_.start();
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
        (void)playback_.primeAudio();
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
            (void)playback_.primeAudio();
            if (context_->audioDevice) context_->audioDevice->resume();
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
    (void)playback_.primeAudio();
    if (context_->audioDevice) context_->audioDevice->resume();
    emit playbackChanged();
}

void RoughCutController::setDecision(int row, const QString &value)
{
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
    if (row < 0 || row >= model_.decisions().size() || !model_.decisions().at(row).userDecision) return;
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
    busy_ = true;
    progressPercent_ = 0;
    emit busyChanged();
    emit progressChanged();
    setStatus(QStringLiteral("正在导出精简 WAV…"));
    const QString sourcePath = mediaPath_;
    const QVector<RoughCutTimelineClip> clips = timeline_;
    const int sampleRate = sampleRate_;
    const int channels = channels_;
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, path, sourcePath, clips, sampleRate, channels, generation] {
        QString error;
        const bool saved = RoughCutWavExporter::save(path, sourcePath, clips,
            sampleRate, channels, &cancel_, &error);
        QMetaObject::invokeMethod(this, [this, path, saved, error, generation] {
            if (generation != workerGeneration_) return;
            busy_ = false;
            progressPercent_ = saved ? 100 : 0;
            emit busyChanged();
            emit progressChanged();
            setStatus(saved ? QStringLiteral("精简 WAV 已导出：%1").arg(path)
                            : error);
        }, Qt::QueuedConnection);
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
        QMetaObject::invokeMethod(this, [this, generation, waveform] {
            if (generation != waveformGeneration_) return;
            if (sourceWaveformItem_) sourceWaveformItem_->setWaveform(waveform);
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
    timeline_ = RoughCutTimelineEngine::build(model_.recording(), model_.decisions(),
                                               sampleRate_, sourceSampleCount_);
    syncSceneItems();
}

void RoughCutController::applyEdit(const Edit &edit, bool forward)
{
    (void)model_.setUserDecision(edit.row, forward ? edit.after : edit.before);
    rebuildTimeline();
    emit historyChanged();
    emit resultsChanged();
}

void RoughCutController::setStatus(QString value)
{
    statusText_ = std::move(value);
    emit statusChanged();
}

} // namespace subcue
