#include "rough_cut_controller.h"

#include "application_context.h"
#include "alignment/normalizer.h"
#include "asr/asr_provider_factory.h"
#include "asr/audio_chunk_extractor.h"
#include "media/media_probe.h"
#include "roughcut/retake_detector.h"
#include "roughcut/rough_cut_project.h"
#include "roughcut/script_document.h"
#include "roughcut/xmeml_exporter.h"
#include "timeline_scene_item.h"
#include "waveform/waveform_generator.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
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

QString transcriptText(const Transcript &transcript)
{
    QString result;
    for (const TranscriptWord &word : transcript.words) result += word.text;
    return result.trimmed();
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

QVector<RecognizedPassage> passagesFromTranscript(const Transcript &transcript, int sampleRate)
{
    QVector<RecognizedPassage> result;
    bool startNext = true;
    for (const TranscriptWord &word : transcript.words) {
        if (word.text.trimmed().isEmpty() || word.endMs <= word.startMs) continue;
        const bool newPassage = startNext || result.isEmpty()
            || word.startMs * sampleRate / 1000 - result.constLast().endSample > sampleRate * 7 / 10;
        if (newPassage) {
            result.append({QString::number(result.size() + 1), word.text,
                word.startMs * sampleRate / 1000, word.endMs * sampleRate / 1000});
        } else {
            result.last().text += word.text;
            result.last().endSample = word.endMs * sampleRate / 1000;
        }
        startNext = word.text.endsWith(QLatin1Char('。')) || word.text.endsWith(QLatin1Char('！'))
            || word.text.endsWith(QLatin1Char('？')) || word.text.endsWith(QLatin1Char('.'))
            || word.text.endsWith(QLatin1Char('!')) || word.text.endsWith(QLatin1Char('?'));
    }
    return result;
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
                if (timelinePlaybackIndex_ >= timeline_.size()) {
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
    setStatus(QStringLiteral("正在识别音频…"));
    const QString mediaPath = mediaPath_;
    const QString scriptPath = scriptPath_;
    const QString scriptText = scriptText_;
    const int sampleRate = sampleRate_;
    const QJsonObject settings = context_->settings;
    const QVector<RecognizedPassage> previousRecording = model_.recording();
    const QVector<RoughCutSegmentDecision> previousDecisions = model_.decisions();
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, mediaPath, scriptPath, scriptText, sampleRate, settings, generation,
                           previousRecording, previousDecisions] {
        AsrProviderFactory factory;
        std::unique_ptr<IAsrService> asr = factory.create(settings, QString());
        const AsrResult result = asr->transcribe({mediaPath, &cancel_, {}});
        if (std::holds_alternative<AppError>(result)) {
            const QString message = std::get<AppError>(result).userMessage();
            QMetaObject::invokeMethod(this, [this, message, generation] {
                if (generation != workerGeneration_) return;
                busy_ = false; emit busyChanged(); setStatus(message);
            }, Qt::QueuedConnection);
            return;
        }
        QVector<RecognizedPassage> recording = passagesFromTranscript(std::get<Transcript>(result), sampleRate);
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
        // 当前界面管线尚未取得逐片段 Forced Alignment，粗定位不得触发自动 CUT。
        QVector<bool> trusted(recording.size(), false);
        QVector<RoughCutSegmentDecision> decisions = RoughCutDecisionEngine::decide(
            recording, matches, groups, trusted);
        int migrated = 0;
        for (int index = 0; index < recording.size(); ++index) {
            for (int oldIndex = 0; oldIndex < previousRecording.size(); ++oldIndex) {
                if (!previousDecisions.at(oldIndex).userDecision
                    || Normalizer::normalizeText(recording.at(index).text)
                        != Normalizer::normalizeText(previousRecording.at(oldIndex).text)) continue;
                const qint64 overlap = std::min(recording.at(index).endSample,
                    previousRecording.at(oldIndex).endSample) - std::max(recording.at(index).startSample,
                    previousRecording.at(oldIndex).startSample);
                const qint64 shorter = std::min(recording.at(index).endSample - recording.at(index).startSample,
                    previousRecording.at(oldIndex).endSample - previousRecording.at(oldIndex).startSample);
                if (overlap > 0 && overlap * 10 >= shorter * 8) {
                    decisions[index].userDecision = previousDecisions.at(oldIndex).userDecision;
                    ++migrated;
                    break;
                }
            }
        }
        QMetaObject::invokeMethod(this, [this, recording = std::move(recording),
                                        decisions = std::move(decisions), migrated, generation]() mutable {
            if (generation != workerGeneration_) return;
            if (cancel_) { busy_ = false; emit busyChanged(); setStatus(QStringLiteral("分析已取消。")); return; }
            model_.reset(std::move(recording), std::move(decisions), sampleRate_);
            ++analysisVersion_;
            auxiliaryResults_.clear();
            history_.clear(); historyIndex_ = 0; rebuildTimeline();
            busy_ = false; emit busyChanged(); emit resultsChanged(); emit historyChanged();
            setStatus(QStringLiteral("分析完成：%1 个片段，迁移 %2 条人工决定；未可靠映射的旧决定未套用。")
                .arg(model_.rowCount()).arg(migrated));
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
    setStatus(QStringLiteral("正在串行执行辅助识别…"));
    const QString mediaPath = mediaPath_;
    const int sampleRate = sampleRate_;
    const QJsonObject settings = context_->settings;
    const QVector<RecognizedPassage> recording = model_.recording();
    const QVector<RoughCutSegmentDecision> decisions = model_.decisions();
    const quint64 generation = ++workerGeneration_;
    worker_ = std::thread([this, ranges, mediaPath, sampleRate, settings, recording, decisions, generation] {
        QVector<RoughCutAuxiliaryResult> results;
        QTemporaryDir temporary;
        for (int rangeIndex = 0; rangeIndex < ranges.size() && !cancel_; ++rangeIndex) {
            const RoughCutSuspiciousRange &range = ranges.at(rangeIndex);
            const AudioChunkWindow window{range.startSample / double(sampleRate),
                range.endSample / double(sampleRate), range.startSample * 1000 / sampleRate,
                range.endSample * 1000 / sampleRate};
            const auto extracted = AudioChunkExtractor::extract(mediaPath, window, &cancel_, true);
            QString funText;
            bool funFailed = true;
            if (temporary.isValid() && std::holds_alternative<PreparedAudioChunk>(extracted)) {
                const QString clipPath = QDir(temporary.path()).filePath(
                    QStringLiteral("review_%1.flac").arg(rangeIndex));
                QFile clip(clipPath);
                if (clip.open(QIODevice::WriteOnly)
                    && clip.write(std::get<PreparedAudioChunk>(extracted).flac) >= 0) {
                    clip.close();
                    AsrProviderFactory factory;
                    QJsonObject funSettings = settings;
                    funSettings.insert(QStringLiteral("asrProvider"), QStringLiteral("funasr"));
                    AsrResult fun = factory.create(funSettings, QString())->transcribe({clipPath, &cancel_, {}});
                    if (std::holds_alternative<Transcript>(fun)) {
                        funText = transcriptText(std::get<Transcript>(fun)); funFailed = funText.isEmpty();
                    }
                }
            }
            for (int index : range.recordingIndexes) {
                RoughCutAuxiliaryResult result{index, recording.at(index).text, funText, {},
                    funFailed, false, false};
                (void)RoughCutAuxiliaryRecognition::reconcile(
                    decisions.at(index).autoDecision, &result);
                results.append(std::move(result));
            }
        }
        QMetaObject::invokeMethod(this, [this, results = std::move(results), generation]() mutable {
            if (generation != workerGeneration_) return;
            if (!cancel_) {
                auxiliaryResults_ = results;
                for (const auto &result : auxiliaryResults_) model_.applyAuxiliaryResult(result);
                rebuildTimeline();
                setStatus(QStringLiteral("辅助识别完成：%1 个片段；冲突或失败均保持 REVIEW。")
                    .arg(auxiliaryResults_.size()));
            } else setStatus(QStringLiteral("辅助识别已取消。"));
            busy_ = false; emit busyChanged(); emit resultsChanged();
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
    stopTimeline();
    timelinePlaybackIndex_ = 0;
    timelinePlaybackClock_.start();
    playbackTimer_.start();
    startTimelineClip(0);
}

void RoughCutController::stopTimeline()
{
    timelinePlaybackIndex_ = -1;
    timelineGapDeadlineMs_ = -1;
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
                              clip.sourceStartSample, clip.sourceEndSample});
    }
    QString error;
    if (!XmemlExporter::save(localPath(url), request, &error)) setStatus(error);
    else setStatus(QStringLiteral("XML 已导出：%1").arg(localPath(url)));
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
        sourceWaveformItem_->fit();
    }
    if (!timelineItem_) return;
    timelineItem_->clearCues();
    qint64 endSample = 0;
    for (int index = 0; index < timeline_.size(); ++index) {
        const RoughCutTimelineClip &clip = timeline_.at(index);
        const qint64 duration = clip.sourceEndSample - clip.sourceStartSample;
        timelineItem_->addCue(QString::number(index + 1),
            clip.timelineStartSample * 1'000'000 / sampleRate_,
            (clip.timelineStartSample + duration) * 1'000'000 / sampleRate_, clip.text);
        endSample = std::max(endSample, clip.timelineStartSample + duration);
    }
    timelineItem_->setDurationUs(sampleRate_ > 0 ? endSample * 1'000'000 / sampleRate_ : 0);
    timelineItem_->fit();
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
