#include "app_controller.h"

#include "application_context.h"
#include "timeline_scene_item.h"
#include "video_preview_item.h"

#include "ai/ai_review_service.h"
#include "ai/ai_review_settings.h"
#include "ai/ai_types.h"
#include "ai/qwen_omni_provider.h"
#include "alignment/alignment_preflight.h"
#include "asr/asr_provider_catalog.h"
#include "asr/asr_provider_factory.h"
#include "asr/asr_types.h"
#include "asr/http_client.h"
#include "common/logging.h"
#include "media/media_probe.h"
#include "roughcut/script_document.h"
#include "settings/model_locator.h"
#include "settings/storage_cleaner.h"
#include "subtitle/srt_parser.h"
#include "subtitle/txt_importer.h"
#include "waveform/waveform_generator.h"

#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QThreadPool>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtCore/QUuid>
#include <QtGui/QDesktopServices>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <variant>

#ifndef SUBCUE_PROJECT_ROOT
#define SUBCUE_PROJECT_ROOT ""
#endif

namespace subcue {
namespace {

// 预览播放的水位线：解码数据先泵到约 300ms 再开始出声，避免音频线程频繁见底。
constexpr int kAudioPreRollMs = 300;

const QStringList kMediaExtensions = {
    QStringLiteral(".wav"), QStringLiteral(".mp3"), QStringLiteral(".m4a"),
    QStringLiteral(".flac"), QStringLiteral(".aac"), QStringLiteral(".mp4"),
    QStringLiteral(".mov"), QStringLiteral(".mkv"), QStringLiteral(".webm"),
    QStringLiteral(".avi"), QStringLiteral(".m4v"), QStringLiteral(".wmv"),
    QStringLiteral(".mpg"), QStringLiteral(".mpeg"), QStringLiteral(".mts"),
    QStringLiteral(".m2ts"), QStringLiteral(".ts"), QStringLiteral(".mxf"),
    QStringLiteral(".ogg"), QStringLiteral(".opus"), QStringLiteral(".wma"),
    QStringLiteral(".aif"), QStringLiteral(".aiff")
};

QString readTextFile(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return {};
    }
    QByteArray bytes = file.readAll();
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        bytes.remove(0, 3);
    }
    return QString::fromUtf8(bytes);
}

QString aiCredentialId(const QString &providerId)
{
    return QStringLiteral("SubCue/AI/%1").arg(providerId);
}

QVariantMap providerResult(const ProviderTestResult &result)
{
    QVariantMap output;
    if (std::holds_alternative<AppError>(result)) {
        output.insert(QStringLiteral("success"), false);
        output.insert(QStringLiteral("error"), std::get<AppError>(result).userMessage());
        return output;
    }
    const ConnectionTestResult connected = std::get<ConnectionTestResult>(result);
    QStringList models;
    for (const ModelDescriptor &model : connected.models) {
        models.append(model.id);
    }
    output.insert(QStringLiteral("success"), true);
    output.insert(QStringLiteral("models"), models);
    output.insert(QStringLiteral("verifiedAtUtc"), connected.verifiedAtUtc.toString(Qt::ISODate));
    return output;
}

} // namespace

AppController::AppController(ApplicationContext *context, QObject *parent)
    : QObject(parent),
      context_(context),
      model_(&document_),
      commands_(&document_),
      editor_(&document_, &commands_, &viewport_, &snap_)
{
    Q_ASSERT(context_ != nullptr);
    context_->registerAudioClient(&playback_);
    backgroundTasks_.setMaxThreadCount(3);
    tickTimer_.setInterval(10);
    tickTimer_.setTimerType(Qt::PreciseTimer);
    connect(commands_.stack(), &QUndoStack::indexChanged, this, [this] {
        setCanExport(std::any_of(document_.subtitles().cbegin(), document_.subtitles().cend(),
            [](const Subtitle &cue) { return cue.isExportable(); }));
        if (selectedCue_ >= document_.count()) {
            selectedCue_ = document_.count() - 1;
            emit selectedCueChanged();
            syncTimelineItem();
        }
    });
    QObject::connect(&tickTimer_, &QTimer::timeout, this, &AppController::onTick);
    QObject::connect(&document_, &SubtitleDocument::reset, this, [this] {
        subtitleIndex_.rebuild(document_.subtitles());
        syncTimelineItem();
        updateCurrentSubtitle();
        emit selectedCueChanged();
    });
    QObject::connect(&document_, &SubtitleDocument::inserted, this, [this] {
        subtitleIndex_.rebuild(document_.subtitles());
        syncTimelineItem();
        updateCurrentSubtitle();
    });
    QObject::connect(&document_, &SubtitleDocument::removed, this, [this] {
        subtitleIndex_.rebuild(document_.subtitles());
        syncTimelineItem();
        updateCurrentSubtitle();
    });
    QObject::connect(&document_, &SubtitleDocument::changed, this, [this](int) {
        subtitleIndex_.rebuild(document_.subtitles());
        syncTimelineItem();
        updateCurrentSubtitle();
    });
    setStatus(context_->credentialMigrationError.isEmpty()
        ? QStringLiteral("就绪")
        : QStringLiteral("旧凭据迁移失败：%1").arg(context_->credentialMigrationError));
}

AppController::~AppController()
{
    shutdown();
}

QObject *AppController::subtitleModel() const
{
    return const_cast<SubtitleModel *>(&model_);
}

QString AppController::mediaPath() const { return mediaPath_; }

QString AppController::mediaName() const
{
    return mediaPath_.isEmpty() ? QStringLiteral("未选择媒体") : QFileInfo(mediaPath_).fileName();
}

bool AppController::hasMedia() const { return !mediaPath_.isEmpty(); }

bool AppController::hasVideo() const { return hasVideo_; }

qint64 AppController::durationMs() const { return durationUs_ / 1000; }

qint64 AppController::durationUs() const { return durationUs_; }

qint64 AppController::timelineDurationMs() const
{
    return durationMs() > 0 ? durationMs() : TimelineViewport::kEmptyTimelineMs;
}

double AppController::fps() const { return fps_; }

qint64 AppController::positionMs() const { return positionUs_ / 1000; }

qint64 AppController::positionUs() const { return positionUs_; }

bool AppController::playing() const { return playing_; }

int AppController::direction() const { return direction_; }

double AppController::playbackRate() const { return playbackRate_; }

QString AppController::statusText() const { return statusText_; }

bool AppController::busy() const { return busy_; }

bool AppController::canExport() const { return canExport_; }

bool AppController::canOmniReview() const
{
    return alignmentCompleted_ && !busy_;
}

int AppController::selectedCue() const { return selectedCue_; }

QString AppController::scriptText() const { return scriptText_; }

void AppController::setScriptText(const QString &value)
{
    if (value == scriptText_) {
        return;
    }
    scriptText_ = value;
    ++scriptGeneration_;
    invalidateAlignmentEvidence();
    if (alignmentCompleted_) {
        alignmentCompleted_ = false;
        emit canReviewChanged();
        emit canOmniReviewChanged();
    }
    emit scriptTextChanged();
}

bool AppController::snapEnabled() const { return snap_.isEnabled(); }

double AppController::pixelsPerMs() const { return viewport_.pixelsPerMs(); }

int AppController::zoomPercent() const { return viewport_.zoomPercent(); }

double AppController::scrollOffset() const { return viewport_.scrollOffset(); }

qint64 AppController::inPoint() const { return inPointMs_; }

qint64 AppController::outPoint() const { return outPointMs_; }

qint64 AppController::inPointUs() const
{
    return inPointMs_ < 0 ? -1 : inPointMs_ * 1000;
}

qint64 AppController::outPointUs() const
{
    return outPointMs_ < 0 ? -1 : outPointMs_ * 1000;
}

QString AppController::currentSubtitleText() const { return currentSubtitle_; }

QString AppController::subtitleFontFamily() const
{
    return context_->settings.value(QStringLiteral("fontFamily")).toString();
}

int AppController::subtitleFontSize() const
{
    return context_->settings.value(QStringLiteral("fontSize1080p")).toInt();
}

QString AppController::subtitleFontColor() const
{
    const QString value = context_->settings.value(QStringLiteral("fontColor")).toString();
    return value.isEmpty() ? QStringLiteral("#FFFFFF") : value;
}

QString AppController::subtitleOutlineColor() const
{
    const QString value = context_->settings.value(QStringLiteral("outlineColor")).toString();
    return value.isEmpty() ? QStringLiteral("#000000") : value;
}

QString AppController::subtitleAlignment() const
{
    return context_->settings.value(QStringLiteral("alignment")).toString();
}

int AppController::subtitleBottomMargin() const
{
    return context_->settings.value(QStringLiteral("bottomMargin1080p")).toInt();
}

QString AppController::audioBackendId() const
{
    return context_->audioDevice ? context_->audioDevice->backendId() : QStringLiteral("none");
}

QString AppController::previewBackendId() const
{
    return previewItem_ ? previewItem_->backendId() : QStringLiteral("none");
}

void AppController::setPreviewItem(QObject *item)
{
    previewItem_ = qobject_cast<VideoPreviewItem *>(item);
    emit previewChanged();
    pushPreviewFrame();
}

void AppController::setTimelineItem(QObject *item)
{
    if (timelineItem_) {
        timelineItem_->disconnect(this);
    }
    timelineItem_ = qobject_cast<TimelineSceneItem *>(item);
    if (timelineItem_) {
        connect(timelineItem_, &TimelineSceneItem::cueDragStarted, this, [this](const QString &id, int mode) {
            selectCue(document_.indexOf(id), false);
            (void)editor_.beginDrag(id, static_cast<CueDragMode>(mode));
        });
        connect(timelineItem_, &TimelineSceneItem::cueDragUpdated, this, [this](double deltaX) {
            if (editor_.updateDragByPixels(deltaX)) {
                timelineItem_->setPreviewCue(editor_.previewCue());
            }
        });
        connect(timelineItem_, &TimelineSceneItem::cueDragFinished, this, [this](bool canceled) {
            timelineItem_->setPreviewCue(std::nullopt);
            if (canceled) editor_.cancelDrag();
            else (void)editor_.endDrag();
            syncTimelineItem();
        });
        connect(timelineItem_, &TimelineSceneItem::cueEditRequested, this, [this](const QString &id) {
            selectCue(document_.indexOf(id), false);
            createOrEditCue();
        });
        QObject::connect(timelineItem_, &TimelineSceneItem::userSeeked, this, [this](qint64 positionUs) {
            stop();
            seekUs(positionUs);
        });
        QObject::connect(timelineItem_, &TimelineSceneItem::viewChanged, this, [this] {
            if (!timelineItem_) {
                return;
            }
            viewport_.setPixelsPerMs(timelineItem_->pixelsPerMs());
            viewport_.setScrollOffset(timelineItem_->scrollOffset(), timelineItem_->width());
            emit timelineViewChanged();
        });
    }
    syncTimelineItem();
}

void AppController::loadMedia(const QUrl &url)
{
    loadMediaPath(localPath(url));
}

void AppController::loadMediaPath(const QString &path)
{
    if (!isMediaPath(path) || !QFileInfo(path).isFile()) {
        setStatus(QStringLiteral("媒体不存在或格式不支持：%1").arg(QFileInfo(path).fileName()));
        return;
    }

    ProbeResult probed = MediaProbe::probe(path);
    if (std::holds_alternative<AppError>(probed)) {
        setStatus(QStringLiteral("媒体读取失败：%1").arg(std::get<AppError>(probed).userMessage()));
        return;
    }
    const MediaInfo info = std::get<MediaInfo>(probed);

    stop();
    IAudioDevice *device = context_->audioDevice.get();
    if (ownsSharedAudio_ && device) {
        // 设备先起：它决定实际混音采样率，解码数据按这个采样率泵入环缓冲。
        AppError audioError(ErrorDomain::Media, 0, QString());
        if (!device->start(playback_.outputSampleRate(), playback_.outputChannels(), &audioError)) {
            qCInfo(subcueAppLog) << "Audio device start failed, falling back to virtual:"
                                 << audioError.userMessage();
            context_->replaceAudioDevice(createAudioDevice(AudioDeviceKind::Virtual));
            device = context_->audioDevice.get();
            if (device) {
                (void)device->start(playback_.outputSampleRate(), playback_.outputChannels(), nullptr);
            }
        }
        if (device) device->setPlaybackRate(playbackRate_);
    }

    AppError openError(ErrorDomain::Media, 0, QString());
    if (!playback_.open(path, &openError)) {
        setStatus(QStringLiteral("媒体打开失败：%1").arg(openError.userMessage()));
        return;
    }
    if (ownsSharedAudio_ && device) {
        playback_.setAudioDevice(device, kAudioPreRollMs);
        device->pause();
    }

    alignmentCancel_ = true;
    ++alignmentGeneration_;
    stopAlignmentWorker();
    omniReviewCancel_ = true;
    ++omniReviewGeneration_;
    stopOmniReviewWorker();
    setBusy(false);
    ++mediaGeneration_;
    invalidateAlignmentEvidence();
    if (alignmentCompleted_) {
        alignmentCompleted_ = false;
        emit canReviewChanged();
        emit canOmniReviewChanged();
    }
    setCanExport(std::any_of(document_.subtitles().cbegin(), document_.subtitles().cend(),
                            [](const Subtitle &subtitle) { return subtitle.isExportable(); }));

    mediaPath_ = QFileInfo(path).canonicalFilePath();
    mediaInfo_ = info;
    durationUs_ = std::max<qint64>(0, info.duration.microseconds());
    hasVideo_ = playback_.hasVideo();
    fps_ = 25.0;
    if (info.videoStreamIndex >= 0 && info.videoStreamIndex < info.streams.size()) {
        const double rate = info.streams.at(info.videoStreamIndex).averageFrameRate;
        if (rate > 0.0) {
            fps_ = rate;
        }
    }
    snap_.setFps(fps_);
    viewport_.setDuration(MediaTime::fromMicroseconds(durationUs_));
    viewport_.setHasMedia(true);
    positionUs_ = 0;
    viewport_.setPlayhead(MediaTime::fromMicroseconds(0));
    clearInPoint();
    clearOutPoint();
    if (previewItem_) {
        previewItem_->clearFrame();
    }

    if (!device) {
        setStatus(QStringLiteral("已加载：%1").arg(mediaName()));
    } else if (device->isHardware() && device->isStarted()) {
        setStatus(QStringLiteral("已加载：%1（%2s，%3 fps）")
                      .arg(mediaName())
                      .arg(QString::number(durationMs() / 1000.0, 'f', 1))
                      .arg(QString::number(fps_, 'f', 3)));
    } else {
        setStatus(QStringLiteral("已加载：%1（音频回退到虚拟输出）").arg(mediaName()));
    }

    startWaveformWorker(path);
    rememberProjectFile(mediaPath_, hasVideo_ ? QStringLiteral("视频") : QStringLiteral("音频"), durationMs());
    emitMediaChanged();
    emit positionChanged();
    playback_.seek(MediaTime::fromMicroseconds(0));
    playback_.pump();
    pushPreviewFrame();
    if (!tickTimer_.isActive()) {
        tickTimer_.start();
    }
}

void AppController::importScript(const QUrl &url)
{
    importScriptPath(localPath(url));
}

void AppController::importScriptPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    QStringList lines;
    if (suffix == QLatin1String("docx")) {
        const ScriptDocumentResult result = ScriptDocumentImporter::loadDocx(path);
        if (std::holds_alternative<AppError>(result)) {
            setStatus(QStringLiteral("文稿导入失败：%1").arg(std::get<AppError>(result).userMessage()));
            return;
        }
        for (const ScriptLine &line : std::get<ScriptDocument>(result).lines) {
            lines.append(line.text);
        }
        lines = TxtImporter::parse(lines.join(QLatin1Char('\n')));
    } else {
        QString error;
        const QString content = readTextFile(path, &error);
        if (!error.isEmpty() && content.isEmpty()) {
            setStatus(QStringLiteral("文稿导入失败：%1").arg(error));
            return;
        }
        if (suffix == QLatin1String("srt")) {
            lines = SrtParser::parseTextOnly(content);
        } else {
            lines = TxtImporter::parse(content);
        }
    }
    QStringList escaped;
    escaped.reserve(lines.size());
    for (QString line : lines) {
        escaped.append(line.replace(QLatin1Char('\n'), QStringLiteral("\\n")));
    }
    setScriptText(escaped.join(QLatin1Char('\n')));
    rememberProjectFile(path, QStringLiteral("文稿"));
    setStatus(QStringLiteral("已导入文稿：%1").arg(QFileInfo(path).fileName()));
}

void AppController::handleDroppedUrl(const QString &value)
{
    importFiles({QFileInfo(value).isFile() ? QUrl::fromLocalFile(value) : QUrl(value)});
}

bool AppController::canImportFiles(const QList<QUrl> &urls) const
{
    return std::any_of(urls.cbegin(), urls.cend(), [this](const QUrl &url) {
        const QString path = localPath(url);
        const QString suffix = QFileInfo(path).suffix().toLower();
        return QFileInfo(path).isFile()
            && (isMediaPath(path) || suffix == QLatin1String("txt")
                || suffix == QLatin1String("srt") || suffix == QLatin1String("docx"));
    });
}

void AppController::importFiles(const QList<QUrl> &urls)
{
    QString firstMedia;
    QString firstScript;
    QStringList failures;
    for (const QUrl &url : urls) {
        const QString path = localPath(url);
        if (!canImportFiles({url})) {
            failures.append(QFileInfo(path).fileName().isEmpty() ? url.toString() : QFileInfo(path).fileName());
            continue;
        }
        if (isMediaPath(path)) {
            const ProbeResult probed = MediaProbe::probe(path);
            if (std::holds_alternative<AppError>(probed)) {
                failures.append(QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(),
                    std::get<AppError>(probed).userMessage()));
                continue;
            }
            const MediaInfo &info = std::get<MediaInfo>(probed);
            if (info.videoStreamIndex < 0 && info.audioStreamIndex < 0) {
                failures.append(QFileInfo(path).fileName());
                continue;
            }
            rememberProjectFile(path, info.videoStreamIndex >= 0 ? QStringLiteral("视频") : QStringLiteral("音频"),
                                info.duration.milliseconds());
            if (firstMedia.isEmpty()) firstMedia = path;
        } else {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                failures.append(QFileInfo(path).fileName());
                continue;
            }
            rememberProjectFile(path, QStringLiteral("文稿"));
            if (firstScript.isEmpty()) firstScript = path;
        }
    }
    if (!firstMedia.isEmpty()) loadMediaPath(firstMedia);
    if (!firstScript.isEmpty()) importScriptPath(firstScript);
    if (!failures.isEmpty()) {
        setStatus(QStringLiteral("以下文件未能导入：%1").arg(failures.join(QStringLiteral("；"))));
    }
}

void AppController::rememberProjectFile(const QString &path, const QString &type, qint64 durationMs)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (const QVariant &item : projectFiles_) {
        if (item.toMap().value(QStringLiteral("path")).toString().compare(canonical, Qt::CaseInsensitive) == 0) return;
    }
    projectFiles_.append(QVariantMap{
        {QStringLiteral("path"), canonical},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("type"), type},
        {QStringLiteral("durationMs"), durationMs}});
    emit projectFilesChanged();
}

void AppController::openProjectFile(int row)
{
    if (row < 0 || row >= projectFiles_.size()) return;
    const QString path = projectFiles_.at(row).toMap().value(QStringLiteral("path")).toString();
    if (isMediaPath(path)) loadMediaPath(path);
    else importScriptPath(path);
}

void AppController::removeProjectFile(int row)
{
    if (row < 0 || row >= projectFiles_.size()) return;
    projectFiles_.removeAt(row);
    emit projectFilesChanged();
    setStatus(QStringLiteral("已从项目素材中移除引用，磁盘文件保留。"));
}

void AppController::showProjectFileFolder(int row)
{
    if (row < 0 || row >= projectFiles_.size()) return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(
            projectFiles_.at(row).toMap().value(QStringLiteral("path")).toString()).absolutePath()))) {
        setStatus(QStringLiteral("无法打开素材所在文件夹。"));
    }
}

void AppController::setAlignmentOverrides(IAsrService *asr, IAiProvider *ai)
{
    asrOverride_ = asr;
    aiOverride_ = ai;
}

void AppController::startAlignment()
{
    startAlignmentRun(false);
}

void AppController::startReview()
{
    if (!alignmentCompleted_) {
        setStatus(QStringLiteral("请先完成自动打轴。"));
        return;
    }
    startAlignmentRun(true);
}

void AppController::startOmniSubtitleReview()
{
    if (busy_) return;
    if (!alignmentCompleted_ || document_.count() <= 0 || mediaPath_.isEmpty()) {
        setStatus(QStringLiteral("请先完成自动打轴后再进行 AI 复核。"));
        return;
    }
    const OmniReviewSettings settings = OmniReviewSettingsStore::fromJson(context_->settings);
    const QString apiKey = OmniReviewSettingsStore::resolveApiKey({}, &context_->credentials);
    if (apiKey.isEmpty()) {
        setStatus(QStringLiteral("未配置 AI API Key。字幕编辑仍可继续使用。"));
        return;
    }
    stopOmniReviewWorker();
    omniReviewCancel_ = false;
    resetProgress(QStringLiteral("正在复核字幕内容"), QStringLiteral("正在准备 AI 复核…"));
    setBusy(true);
    setStatus(QStringLiteral("正在复核字幕内容…"));
    SubtitleOmniRequest request;
    request.mediaPath = mediaPath_;
    request.scriptText = scriptText_;
    const QList<Subtitle> cues = document_.subtitles();
    request.segments.reserve(cues.size());
    for (int index = 0; index < cues.size(); ++index) {
        const Subtitle &cue = cues.at(index);
        SubtitleOmniSegment segment;
        segment.segmentId = cue.id;
        segment.text = cue.text;
        segment.startMs = cue.start.milliseconds();
        segment.endMs = cue.end.milliseconds();
        if (index > 0) segment.previousText = cues.at(index - 1).text;
        if (index + 1 < cues.size()) segment.nextText = cues.at(index + 1).text;
        request.segments.append(std::move(segment));
    }
    const quint64 generation = ++omniReviewGeneration_;
    const quint64 mediaGeneration = mediaGeneration_;
    const quint64 scriptGeneration = scriptGeneration_;
    const quint64 evidenceGeneration = alignmentEvidenceGeneration_;
    IHttpClient *http = context_->aiHttp;
    std::atomic<bool> *cancel = &omniReviewCancel_;
    QPointer<AppController> self(this);
    omniReviewThread_ = std::thread([self, request, settings, apiKey, generation,
                                     mediaGeneration, scriptGeneration, evidenceGeneration, http, cancel] {
        AiReviewService service(settings, apiKey, http);
        const OmniReviewProgress progress = [self, generation](int completed, int total, const QString &message) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, generation, completed, total, message] {
                if (self) self->reportOmniProgress(generation, completed, total, message);
            }, Qt::QueuedConnection);
        };
        SubtitleOmniResult result = service.reviewSubtitles(request, cancel, progress);
        const QString model = service.lastModel();
        const OmniUsage usage = service.accumulatedUsage();
        if (self) {
            QMetaObject::invokeMethod(self, [self, generation, mediaGeneration, scriptGeneration,
                                             evidenceGeneration, result = std::move(result), model, usage]() mutable {
                if (self) self->finishOmniSubtitleReview(
                    generation, std::move(result), mediaGeneration, scriptGeneration, evidenceGeneration,
                    model, usage);
            }, Qt::QueuedConnection);
        }
    });
}

void AppController::startWordMappingReview()
{
    if (busy_) return;
    if (!alignmentCompleted_ || document_.count() <= 0 || mediaPath_.isEmpty()) {
        setStatus(QStringLiteral("请先完成打轴。"));
        return;
    }
    if (alignmentWords_.isEmpty()) {
        setStatus(QStringLiteral("缺少有效词级证据，请先完成打轴。"));
        return;
    }
    const OmniReviewSettings settings = OmniReviewSettingsStore::fromJson(context_->settings);
    const QString apiKey = OmniReviewSettingsStore::resolveApiKey({}, &context_->credentials);
    if (apiKey.isEmpty()) {
        setStatus(QStringLiteral("未配置 AI API Key。字幕编辑仍可继续使用。"));
        return;
    }
    stopOmniReviewWorker();
    omniReviewCancel_ = false;
    resetProgress(QStringLiteral("正在复核时间映射"), QStringLiteral("正在准备时间映射复核…"));
    setBusy(true);
    setStatus(QStringLiteral("正在复核时间映射…"));
    WordMappingOmniRequest request;
    request.words = alignmentWords_;
    for (const Subtitle &cue : document_.subtitles()) request.subtitles.append(cue);
    const quint64 generation = ++omniReviewGeneration_;
    const quint64 mediaGeneration = mediaGeneration_;
    const quint64 scriptGeneration = scriptGeneration_;
    const quint64 evidenceGeneration = alignmentEvidenceGeneration_;
    IHttpClient *http = context_->aiHttp;
    std::atomic<bool> *cancel = &omniReviewCancel_;
    QPointer<AppController> self(this);
    omniReviewThread_ = std::thread([self, request, settings, apiKey, generation,
                                     mediaGeneration, scriptGeneration, evidenceGeneration, http, cancel] {
        AiReviewService service(settings, apiKey, http);
        const OmniReviewProgress progress = [self, generation](int completed, int total, const QString &message) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, generation, completed, total, message] {
                if (self) self->reportOmniProgress(generation, completed, total, message);
            }, Qt::QueuedConnection);
        };
        WordMappingOmniResult result = service.reviewWordMapping(request, cancel, progress);
        const QString model = service.lastModel();
        const OmniUsage usage = service.accumulatedUsage();
        if (self) {
            QMetaObject::invokeMethod(self, [self, generation, mediaGeneration, scriptGeneration,
                                             evidenceGeneration, result = std::move(result), model, usage]() mutable {
                if (self) self->finishWordMappingReview(
                    generation, std::move(result), mediaGeneration, scriptGeneration, evidenceGeneration,
                    model, usage);
            }, Qt::QueuedConnection);
        }
    });
}

void AppController::acceptOmniSubtitleSuggestion(int row)
{
    if (row < 0 || row >= document_.count()) return;
    Subtitle cue = document_.subtitles().at(row);
    const QString suggested = cue.metadata.value(QStringLiteral("omniSuggestedText")).toString();
    cue.metadata.insert(QStringLiteral("omniReviewStatus"),
        omniReviewStatusName(OmniReviewStatus::Accepted));
    document_.replaceSubtitle(cue.id, cue);
    if (!suggested.trimmed().isEmpty() && suggested != cue.text) {
        commands_.editText(cue.id, suggested);
    }
    setStatus(QStringLiteral("已接受 AI 字幕建议。"));
}

void AppController::ignoreOmniSubtitleSuggestion(int row)
{
    if (row < 0 || row >= document_.count()) return;
    Subtitle cue = document_.subtitles().at(row);
    cue.metadata.insert(QStringLiteral("omniReviewStatus"),
        omniReviewStatusName(OmniReviewStatus::Ignored));
    document_.replaceSubtitle(cue.id, cue);
    setStatus(QStringLiteral("已忽略 AI 字幕建议。"));
}

void AppController::startAlignmentRun(bool review)
{
    if (busy_) {
        return;
    }
    if (!review && alignmentCompleted_) {
        alignmentCompleted_ = false;
        emit canReviewChanged();
        emit canOmniReviewChanged();
    }
    invalidateAlignmentEvidence();
    const QStringList lines = TxtImporter::parse(QString(scriptText_).replace(QStringLiteral("\\n"), QStringLiteral("\n")));
    if (mediaPath_.isEmpty() || lines.isEmpty()) {
        QVariantList values;
        if (mediaPath_.isEmpty()) values.append(QVariantMap{
            {QStringLiteral("code"), QStringLiteral("media_missing")},
            {QStringLiteral("title"), QStringLiteral("请先导入媒体")},
            {QStringLiteral("detail"), QString()}, {QStringLiteral("settingsSection"), QString()}});
        if (lines.isEmpty()) values.append(QVariantMap{
            {QStringLiteral("code"), QStringLiteral("script_missing")},
            {QStringLiteral("title"), QStringLiteral("请先输入字幕文稿")},
            {QStringLiteral("detail"), QString()}, {QStringLiteral("settingsSection"), QString()}});
        if (!context_->settings.value(QStringLiteral("outputSrt")).toBool()
            && !context_->settings.value(QStringLiteral("outputAss")).toBool())
            values.append(QVariantMap{{QStringLiteral("code"), QStringLiteral("output_format_missing")},
                {QStringLiteral("title"), QStringLiteral("请至少启用一种输出格式")},
                {QStringLiteral("detail"), QString()}, {QStringLiteral("settingsSection"), QStringLiteral("output")}});
        setStatus(QStringLiteral("请先导入媒体并输入字幕文稿。"));
        emit alignmentPreflightFailed(values);
        return;
    }
    alignmentCancel_ = false;
    stopAlignmentWorker();
    const quint64 generation = ++alignmentGeneration_;
    const QString mediaPath = mediaPath_;
    const MediaInfo mediaInfo = mediaInfo_;
    QJsonObject settings = context_->settings;
    if (review && !settings.value(QStringLiteral("reviewUseSameAsr")).toBool(true)) {
        settings.insert(QStringLiteral("asrProvider"),
                        settings.value(QStringLiteral("reviewAsrProvider")).toString(QStringLiteral("funasr")));
        settings.insert(QStringLiteral("asrModel"),
                        settings.value(QStringLiteral("reviewAsrModel")).toString(QStringLiteral("Fun-ASR-Nano-2512")));
    }
    reviewRun_ = review;
    IAsrService *asr = asrOverride_;
    Q_UNUSED(aiOverride_);
    resetProgress(review ? QStringLiteral("正在 ASR 复核") : QStringLiteral("正在自动打轴"),
                  QStringLiteral("正在检查配置与模型…"));
    setBusy(true);
    setCanExport(false);
    alignmentState_ = {QStringLiteral("Preflight"), QStringLiteral("正在检查配置与模型…")};
    alignmentProgressText_ = alignmentState_.message;
    alignmentProgress_ = 0;
    emit alignmentProgressChanged();

    alignmentThread_ = std::thread([this, generation, mediaPath, mediaInfo, lines, settings, asr] {
        QElapsedTimer preflightTimer;
        preflightTimer.start();
        const QString asrProvider = AsrProviderFactory::providerIdFromSettings(settings);
        const QJsonObject asrVerification = settings
            .value(QStringLiteral("asrVerification")).toObject();
        const bool asrVerified = asr != nullptr
            || (asrVerification.value(QStringLiteral("providerId")).toString() == asrProvider
                && asrVerification.value(QStringLiteral("selectedModel")).toString()
                    == settings.value(QStringLiteral("asrModel")).toString()
                && asrVerification.value(QStringLiteral("configRevision")).toInt()
                    == settings.value(QStringLiteral("asrConfigRevision")).toInt()
                && asrVerification.value(QStringLiteral("credentialRevision")).toInt()
                    == settings.value(QStringLiteral("asrCredentialRevision")).toInt()
                && !asrVerification.value(QStringLiteral("verifiedAtUtc")).toString().isEmpty());
        const PreflightState preflightState{
            asr != nullptr || context_->credentials.exists(QStringLiteral("SubCue/ASR/dashscope")),
            asrVerified,
        };
        const QVector<PreflightIssue> issues = AlignmentPreflight::check(
            mediaPath, mediaInfo, lines, settings, preflightState);
        if (!issues.isEmpty()) {
            QVariantList values;
            values.reserve(issues.size());
            for (const PreflightIssue &issue : issues) {
                values.append(QVariantMap{{QStringLiteral("code"), issue.code},
                                          {QStringLiteral("title"), issue.title},
                                          {QStringLiteral("detail"), issue.detail},
                                          {QStringLiteral("settingsSection"), issue.settingsSection}});
            }
            QPointer<AppController> self(this);
            QMetaObject::invokeMethod(this, [self, generation, values = values] {
                if (!self || generation != self->alignmentGeneration_.load()) return;
                self->finishAlignment(generation, AppError(ErrorDomain::Validation, 1,
                    QStringLiteral("自动打轴前检查未通过")));
                if (!self->alignmentCancel_) emit self->alignmentPreflightFailed(values);
            }, Qt::QueuedConnection);
            return;
        }

        const AlignmentCredentials credentials{
            asrProvider == QLatin1String(kAsrProviderDashScope)
                ? context_->credentials.load(QStringLiteral("SubCue/ASR/dashscope")) : QString(),
            {},
        };
        qCInfo(subcueAppLog) << "preflight elapsed_ms=" << preflightTimer.elapsed();
        AlignmentPipeline pipeline(settings, credentials, asr);
        const AlignmentRunResult result = pipeline.run(
            mediaPath,
            lines,
            &alignmentCancel_,
            [this, generation](const AlignmentProgressState &progress) {
                const QString stage = progress.stage;
                const QString message = progress.message;
                const int completed = progress.completed;
                const int total = progress.total;
                QPointer<AppController> self(this);
                QMetaObject::invokeMethod(this, [self, generation, stage, message, completed, total] {
                    if (!self || generation != self->alignmentGeneration_.load() || self->alignmentCancel_) {
                        return;
                    }
                    self->alignmentState_ = {stage, message, completed, total};
                    self->alignmentProgressText_ = message;
                    self->alignmentProgress_ = self->alignmentState_.percent();
                    emit self->alignmentProgressChanged();
                    self->setStatus(message);
                }, Qt::QueuedConnection);
            });
        QPointer<AppController> self(this);
        QMetaObject::invokeMethod(this, [self, generation, result = result] {
            if (!self) {
                return;
            }
            self->finishAlignment(generation, result);
        }, Qt::QueuedConnection);
    });
}

void AppController::cancelAlignment()
{
    if (!busy_) {
        return;
    }
    alignmentCancel_ = true;
    omniReviewCancel_ = true;
    alignmentProgressText_ = QStringLiteral("正在取消…");
    emit alignmentProgressChanged();
    setStatus(alignmentProgressText_);
}

void AppController::exportSubtitles(const QString &outputDirectory)
{
    AlignmentResult current;
    current.subtitles.reserve(document_.count());
    for (const Subtitle &subtitle : document_.subtitles()) {
        current.subtitles.append(subtitle);
    }
    if (current.exportableSubtitles().isEmpty()) {
        setStatus(QStringLiteral("没有可导出的字幕。"));
        emit exportFinished(false, statusText_, {});
        return;
    }

    QJsonObject settings = context_->settings;
    if (!outputDirectory.trimmed().isEmpty()) {
        settings.insert(QStringLiteral("outputDirectory"), outputDirectory.trimmed());
    }
    const std::variant<QStringList, AppError> exported = AlignmentPipeline::exportResult(
        mediaPath_, current, mediaInfo_, settings);
    if (std::holds_alternative<AppError>(exported)) {
        setStatus(std::get<AppError>(exported).userMessage());
        emit exportFinished(false, statusText_, {});
        return;
    }
    lastOutputPaths_ = std::get<QStringList>(exported);
    setCanExport(true);
    setStatus(QStringLiteral("导出完成：%1").arg(lastOutputPaths_.join(QStringLiteral("；"))));
    emit exportFinished(true, statusText_, lastOutputPaths_);
}

void AppController::togglePlay()
{
    if (playing_) {
        stop();
    } else if (direction_ < 0) {
        playReverse();
    } else {
        playForward();
    }
}

void AppController::playForward()
{
    if (!playback_.isOpen()) {
        return;
    }
    direction_ = 1;
    playing_ = true;
    playback_.play();
    if (!playback_.isPriming() && context_->audioDevice) {
        context_->audioDevice->resume();
    }
    elapsed_.start();
    if (!tickTimer_.isActive()) {
        tickTimer_.start();
    }
    emit playbackChanged();
}

void AppController::playReverse()
{
    if (!playback_.isOpen()) {
        return;
    }
    stop();
    direction_ = -1;
    playing_ = true;
    if (context_->audioDevice) {
        context_->audioDevice->pause();
    }
    elapsed_.start();
    if (!tickTimer_.isActive()) {
        tickTimer_.start();
    }
    emit playbackChanged();
}

void AppController::stop()
{
    const bool wasPlaying = playing_;
    playing_ = false;
    direction_ = 1;
    playback_.pause();
    if (context_->audioDevice) {
        context_->audioDevice->pause();
    }
    if (wasPlaying) {
        emit playbackChanged();
    }
}

void AppController::setPlaybackRate(double rate)
{
    static constexpr double rates[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    const auto found = std::find_if(std::begin(rates), std::end(rates), [rate](double supported) {
        return qAbs(rate - supported) < 0.001;
    });
    if (found == std::end(rates) || qFuzzyCompare(playbackRate_, *found)) {
        return;
    }
    playbackRate_ = *found;
    if (context_->audioDevice) {
        context_->audioDevice->setPlaybackRate(playbackRate_);
    }
    emit playbackChanged();
}

void AppController::seek(qint64 positionMs)
{
    seekUs(MediaTime::fromMilliseconds(std::max<qint64>(0, positionMs)).microseconds());
}

void AppController::seekUs(qint64 positionUs)
{
    const qint64 maximum = durationUs_ > 0 ? durationUs_ : TimelineViewport::kEmptyTimelineMs * 1000;
    positionUs_ = std::clamp<qint64>(positionUs, 0, maximum);
    viewport_.setPlayhead(MediaTime::fromMicroseconds(positionUs_));
    if (playback_.isOpen()) {
        playback_.seek(MediaTime::fromMicroseconds(positionUs_));
        playback_.pump();
    }
    emit positionChanged();
    updateCurrentSubtitle();
    pushPreviewFrame();
    syncTimelineItem();
}

void AppController::stepFrames(int frames)
{
    stop();
    seek(positionMs() + frameDeltaMs(frames));
}

int AppController::frameDeltaMs(int frames) const
{
    const double fps = std::max(1.0, fps_);
    return static_cast<int>(std::lround(static_cast<double>(frames) * 1000.0 / fps));
}

void AppController::selectCue(int row, bool seekToCue)
{
    if (row < 0 || row >= document_.count()) {
        return;
    }
    selectedCue_ = row;
    emit selectedCueChanged();
    const Subtitle &subtitle = document_.subtitles().at(row);
    if (seekToCue && subtitle.isTimed()) {
        seekUs(subtitle.start.microseconds());
        if (timelineItem_) {
            viewport_.setScrollOffset(std::max(0.0, subtitle.start.milliseconds()
                * viewport_.pixelsPerMs() - timelineItem_->width() * 0.25), timelineItem_->width());
            timelineItem_->setView(viewport_.pixelsPerMs(), viewport_.scrollOffset());
            emit timelineViewChanged();
        }
    }
    syncTimelineItem();
}

void AppController::navigateCue(int delta)
{
    if (document_.count() <= 0) {
        return;
    }
    const int row = selectedCue_ < 0
        ? 0
        : std::clamp(selectedCue_ + delta, 0, document_.count() - 1);
    selectCue(row);
}

void AppController::setCueText(int row, const QString &text)
{
    if (row < 0 || row >= document_.count()) {
        return;
    }
    const Subtitle &current = document_.subtitles().at(row);
    commands_.editText(current.id, QString(text).replace(QLatin1Char('\n'), QLatin1Char(' '))
        .replace(QLatin1Char('\r'), QLatin1Char(' ')).trimmed());
    updateCurrentSubtitle();
}

void AppController::locateCue(int row)
{
    if (row < 0 || row >= document_.count()) return;
    locateCueAt(document_.subtitles().at(row).id, positionMs());
}

void AppController::locateCueAt(const QString &id, qint64 startMs)
{
    const int row = document_.indexOf(id);
    if (busy_ || row < 0 || document_.subtitles().at(row).isTimed()) return;
    const auto bounds = editor_.neighborBounds(id);
    const qint64 endMs = std::min({startMs + 2'000, bounds.nextStart.milliseconds(), timelineDurationMs()});
    if (startMs < bounds.previousEnd.milliseconds() || endMs - startMs < 250) {
        setStatus(QStringLiteral("此处没有至少 250ms 的可用空间，请放到相邻字幕之间。"));
        return;
    }
    if (setTimingMs(row, startMs, endMs)) {
        selectCue(row, false);
        setStatus(QStringLiteral("已创建人工时间段，可拖动或修剪。"));
    }
}

void AppController::confirmCue(int row)
{
    if (row < 0 || row >= document_.count()) return;
    const Subtitle cue = document_.subtitles().at(row);
    if (cue.isTimed()) (void)commands_.setTiming(cue.id, cue.start, cue.end);
}

void AppController::createNextScriptCue()
{
    for (int row = 0; row < document_.count(); ++row) {
        const Subtitle subtitle = document_.subtitles().at(row);
        if (subtitle.isTimed()) {
            continue;
        }
        const std::pair<qint64, qint64> range = defaultNewRange(row);
        if (setTimingMs(row, range.first, range.second)) {
            selectCue(row, false);
            emit editCueRequested(row, subtitle.text);
        }
        return;
    }
    setStatus(QStringLiteral("没有尚未打轴的文稿。"));
}

void AppController::createOrEditCue()
{
    const int row = activeRow();
    if (row >= 0 && row < document_.count()) {
        selectedCue_ = row;
        emit selectedCueChanged();
        emit editCueRequested(row, document_.subtitles().at(row).text);
        return;
    }
    if (durationUs_ <= 0) {
        return;
    }
    const std::pair<qint64, qint64> range = defaultNewRange(document_.count());
    Subtitle subtitle;
    subtitle.start = MediaTime::fromMilliseconds(range.first);
    subtitle.end = MediaTime::fromMilliseconds(range.second);
    subtitle.source = QStringLiteral("manual");
    subtitle.status = QStringLiteral("MANUAL");
    if (subtitle.end.microseconds() - subtitle.start.microseconds() < TimelineEditor::kMinCueUs) {
        setStatus(QStringLiteral("当前位置没有足够空间创建字幕。"));
        return;
    }
    const int index = document_.count();
    if (!commands_.create(index, subtitle)) {
        return;
    }
    selectedCue_ = index;
    emit selectedCueChanged();
    emit editCueRequested(index, subtitle.text);
    setCanExport(true);
}

void AppController::splitCurrentCue()
{
    const int row = activeRow();
    if (row < 0) {
        setStatus(QStringLiteral("播放头必须位于字幕内部，且两侧至少保留 250ms。"));
        return;
    }
    viewport_.setPlayhead(MediaTime::fromMicroseconds(positionUs_));
    if (!editor_.splitAtPlayhead(document_.subtitles().at(row).id)) {
        setStatus(QStringLiteral("播放头必须位于字幕内部，且两侧至少保留 250ms。"));
        return;
    }
    setCanExport(true);
    syncTimelineItem();
}

void AppController::joinAroundPlayhead()
{
    viewport_.setPlayhead(MediaTime::fromMicroseconds(positionUs_));
    if (!editor_.joinAroundPlayhead()) {
        setStatus(QStringLiteral("播放头两侧没有可衔接的字幕，或调整后短于 250ms。"));
        return;
    }
    setCanExport(true);
    syncTimelineItem();
}

void AppController::setCurrentStart()
{
    const int row = activeRow();
    if (row < 0) {
        return;
    }
    const Subtitle &subtitle = document_.subtitles().at(row);
    (void)setTimingMs(row, positionMs(), subtitle.end.milliseconds());
}

void AppController::setCurrentEnd()
{
    const int row = activeRow();
    if (row < 0) {
        return;
    }
    const Subtitle &subtitle = document_.subtitles().at(row);
    (void)setTimingMs(row, subtitle.start.milliseconds(), positionMs());
}

void AppController::setFollowingStart()
{
    const auto [before, after] = rowsAroundPlayhead();
    Q_UNUSED(before);
    if (after < 0) {
        return;
    }
    const Subtitle &subtitle = document_.subtitles().at(after);
    (void)setTimingMs(after, positionMs(), subtitle.end.milliseconds());
}

void AppController::setPreviousEnd()
{
    const auto [before, after] = rowsAroundPlayhead();
    Q_UNUSED(after);
    if (before < 0) {
        return;
    }
    const Subtitle &subtitle = document_.subtitles().at(before);
    (void)setTimingMs(before, subtitle.start.milliseconds(), positionMs());
}

void AppController::toggleSnap()
{
    snap_.setEnabled(!snap_.isEnabled());
    emit snapEnabledChanged();
}

void AppController::setInPoint()
{
    inPointMs_ = positionMs();
    editor_.setInPoint(MediaTime::fromMilliseconds(inPointMs_));
    emit rangeChanged();
    syncTimelineItem();
}

void AppController::setOutPoint()
{
    outPointMs_ = positionMs();
    editor_.setOutPoint(MediaTime::fromMilliseconds(outPointMs_));
    emit rangeChanged();
    syncTimelineItem();
}

void AppController::clearInPoint()
{
    inPointMs_ = -1;
    editor_.setInPoint(std::nullopt);
    emit rangeChanged();
    syncTimelineItem();
}

void AppController::clearOutPoint()
{
    outPointMs_ = -1;
    editor_.setOutPoint(std::nullopt);
    emit rangeChanged();
    syncTimelineItem();
}

void AppController::adjustZoomPercent(int delta, double viewportWidth)
{
    viewport_.setPlayhead(MediaTime::fromMicroseconds(positionUs_));
    viewport_.adjustZoomPercent(delta, viewportWidth);
    if (timelineItem_) {
        timelineItem_->setView(viewport_.pixelsPerMs(), viewport_.scrollOffset());
    }
    emit timelineViewChanged();
}

void AppController::fitTimeline(double viewportWidth)
{
    viewport_.fit(viewportWidth);
    if (timelineItem_) {
        timelineItem_->setView(viewport_.pixelsPerMs(), viewport_.scrollOffset());
    }
    emit timelineViewChanged();
}

void AppController::syncTimelineView(double pixelsPerMs, double scrollOffset)
{
    viewport_.setPixelsPerMs(pixelsPerMs);
    viewport_.setScrollOffset(scrollOffset, 1280.0);
    emit timelineViewChanged();
}

QString AppController::formatTime(qint64 ms) const
{
    ms = std::max<qint64>(0, ms);
    const qint64 hours = ms / 3'600'000;
    ms %= 3'600'000;
    const qint64 minutes = ms / 60'000;
    ms %= 60'000;
    const qint64 seconds = ms / 1'000;
    const qint64 millis = ms % 1'000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QChar(u'0'))
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'))
        .arg(millis, 3, 10, QChar(u'0'));
}

QVariant AppController::setting(const QString &key) const
{
    return context_->settings.value(key);
}

QVariantList AppController::asrProviders() const
{
    QVariantList result;
    for (const AsrProviderInfo &info : AsrProviderCatalog::providers()) {
        result.append(QVariantMap{
            {QStringLiteral("id"), info.id},
            {QStringLiteral("name"), info.name},
        });
    }
    return result;
}

QVariantList AppController::asrModels(const QString &providerId) const
{
    QVariantList result;
    const AsrProviderInfo info = AsrProviderCatalog::byId(providerId);
    if (info.local) {
        const QString directory = ModelLocator::directoryFor(info.modelKind, context_->settings);
        result.append(QVariantMap{
            {QStringLiteral("id"), info.modelId},
            {QStringLiteral("name"), info.modelName},
            {QStringLiteral("ready"), ModelLocator::modelReady(directory)},
            {QStringLiteral("directory"), directory},
        });
        return result;
    }
    result.append(QVariantMap{
        {QStringLiteral("id"), QStringLiteral("fun-asr-flash-2026-06-15")},
        {QStringLiteral("name"), QStringLiteral("Fun-ASR Flash 2026-06-15")},
        {QStringLiteral("ready"), true},
    });
    result.append(QVariantMap{
        {QStringLiteral("id"), QStringLiteral("fun-asr-flash")},
        {QStringLiteral("name"), QStringLiteral("Fun-ASR Flash（最新版）")},
        {QStringLiteral("ready"), true},
    });
    return result;
}

void AppController::requestAsrModels(const QString &providerId, const QString &directory, int requestId)
{
    Q_UNUSED(directory);
    emit asrModelsReady(requestId, asrModels(providerId));
}

void AppController::testAiConnection(const QString &apiKey)
{
    const OmniReviewSettings omni = OmniReviewSettingsStore::fromJson(context_->settings);
    const QString key = OmniReviewSettingsStore::resolveApiKey(apiKey, &context_->credentials);
    if (key.isEmpty()) {
        emit aiConnectionTestFinished({
            {QStringLiteral("success"), false},
            {QStringLiteral("error"), QStringLiteral("请先输入 API Key，或设置 DASHSCOPE_API_KEY")}});
        return;
    }
    if (shuttingDown_) return;
    QPointer<AppController> self(this);
    IHttpClient *http = context_->aiHttp;
    backgroundTasks_.start([self, key, omni, http, cancel = &backgroundCancel_] {
        QwenOmniProvider service(key, omni, http);
        OmniTestResult tested = service.testConnection(cancel);
        QVariantMap result;
        if (std::holds_alternative<AppError>(tested)) {
            result.insert(QStringLiteral("success"), false);
            result.insert(QStringLiteral("error"), std::get<AppError>(tested).userMessage());
        } else {
            const OmniConnectionTestResult connected = std::get<OmniConnectionTestResult>(tested);
            result.insert(QStringLiteral("success"), true);
            result.insert(QStringLiteral("model"), connected.model);
            result.insert(QStringLiteral("latencyMs"), connected.latencyMs);
            result.insert(QStringLiteral("promptTokens"), connected.usage.promptTokens);
            result.insert(QStringLiteral("completionTokens"), connected.usage.completionTokens);
            result.insert(QStringLiteral("totalTokens"), connected.usage.totalTokens);
            result.insert(QStringLiteral("verifiedAtUtc"), connected.verifiedAtUtc.toString(Qt::ISODate));
        }
        if (self && !cancel->load()) {
            QMetaObject::invokeMethod(self, [self, result] {
                if (self) emit self->aiConnectionTestFinished(result);
            }, Qt::QueuedConnection);
        }
    });
}

void AppController::testAsrConnection(const QVariantMap &values, const QString &apiKey)
{
    QJsonObject settings = context_->settings;
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        settings.insert(iterator.key(), QJsonValue::fromVariant(iterator.value()));
    }
    const QString provider = AsrProviderFactory::providerIdFromSettings(settings);
    const QString key = apiKey.trimmed().isEmpty()
        ? context_->credentials.load(QStringLiteral("SubCue/ASR/dashscope")) : apiKey.trimmed();
    if (provider == QLatin1String(kAsrProviderDashScope) && key.isEmpty()) {
        emit asrConnectionTestFinished({
            {QStringLiteral("success"), false},
            {QStringLiteral("error"), QStringLiteral("请先输入云端 ASR API Key")}});
        return;
    }
    if (shuttingDown_) return;
    QPointer<AppController> self(this);
    backgroundTasks_.start([self, settings, key, cancel = &backgroundCancel_] {
        std::unique_ptr<IAsrService> service = AsrProviderFactory().create(settings, key);
        const QVariantMap result = providerResult(service->testConnection(cancel));
        if (self && !cancel->load()) {
            QMetaObject::invokeMethod(self, [self, result] {
                if (self) emit self->asrConnectionTestFinished(result);
            }, Qt::QueuedConnection);
        }
    });
}

bool AppController::saveSettings(
    const QVariantMap &values,
    const QString &asrApiKey,
    const QString &aiApiKey)
{
    const QJsonObject originalSettings = context_->settings;
    const QStringList droppedKeys = {
        QStringLiteral("aiAssistEnabled"), QStringLiteral("aiProviderId"), QStringLiteral("aiProviders"),
        QStringLiteral("omniReviewEnabled"), QStringLiteral("omniReviewProvider"),
        QStringLiteral("omniReviewModel"), QStringLiteral("omniReviewBaseUrl"),
        QStringLiteral("omniReviewReasoningEffort"), QStringLiteral("clearAiApiKey"),
    };
    const bool clearAiKey = values.value(QStringLiteral("clearAiApiKey")).toBool();
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        if (droppedKeys.contains(iterator.key())) continue;
        context_->settings.insert(iterator.key(), QJsonValue::fromVariant(iterator.value()));
    }
    QString error;
    const QString asrCredentialId = QStringLiteral("SubCue/ASR/dashscope");
    const QString omniCredentialId = QString::fromLatin1(kOmniReviewCredentialId);
    const QString previousAsrSecret = asrApiKey.trimmed().isEmpty()
        ? QString() : context_->credentials.load(asrCredentialId);
    const auto restoreCredential = [this](const QString &id, const QString &secret) {
        if (id.isEmpty()) return;
        if (secret.isEmpty()) (void)context_->credentials.remove(id);
        else (void)context_->credentials.save(id, secret);
    };
    if (!asrApiKey.trimmed().isEmpty()) {
        if (!context_->credentials.save(asrCredentialId, asrApiKey.trimmed(), &error)) {
            context_->settings = originalSettings;
            setStatus(QStringLiteral("设置保存失败：%1").arg(error));
            return false;
        }
        context_->settings.insert(QStringLiteral("asrCredentialRevision"),
            context_->settings.value(QStringLiteral("asrCredentialRevision")).toInt() + 1);
    }
    QString previousOmniSecret = context_->credentials.load(omniCredentialId);
    bool omniSecretChanged = false;
    if (clearAiKey) {
        omniSecretChanged = true;
        if (!context_->credentials.remove(omniCredentialId, &error)) {
            if (!asrApiKey.trimmed().isEmpty()) restoreCredential(asrCredentialId, previousAsrSecret);
            context_->settings = originalSettings;
            setStatus(QStringLiteral("设置保存失败：%1").arg(error));
            return false;
        }
    } else if (!aiApiKey.trimmed().isEmpty()) {
        omniSecretChanged = true;
        if (!context_->credentials.save(omniCredentialId, aiApiKey.trimmed(), &error)) {
            if (!asrApiKey.trimmed().isEmpty()) restoreCredential(asrCredentialId, previousAsrSecret);
            context_->settings = originalSettings;
            setStatus(QStringLiteral("设置保存失败：%1").arg(error));
            return false;
        }
    }
    if (!context_->settingsManager.save(context_->settings, &error)) {
        if (!asrApiKey.trimmed().isEmpty()) restoreCredential(asrCredentialId, previousAsrSecret);
        if (omniSecretChanged) restoreCredential(omniCredentialId, previousOmniSecret);
        context_->settings = originalSettings;
        setStatus(QStringLiteral("设置保存失败：%1").arg(error));
        return false;
    }
    QJsonArray leftoverIds = context_->settings.value(QStringLiteral("legacyAiCredentialIds")).toArray();
    for (const QJsonValue &id : leftoverIds) {
        (void)context_->credentials.remove(aiCredentialId(id.toString()));
    }
    context_->settings.insert(QStringLiteral("legacyAiCredentialIds"), QJsonArray{});
    (void)context_->settingsManager.save(context_->settings);
    context_->settings = context_->settingsManager.load();
    emit settingsChanged();
    emit canOmniReviewChanged();
    setStatus(QStringLiteral("设置已保存。"));
    return true;
}

QString AppController::aiKeySource(const QString &uiKey, bool ignoreSaved) const
{
    const AiKeySource source = OmniReviewSettingsStore::resolveApiKeySource(
        uiKey, &context_->credentials, ignoreSaved);
    if (ignoreSaved && source == AiKeySource::Environment) {
        return QStringLiteral("已清除应用保存的密钥，仍可使用环境变量 DASHSCOPE_API_KEY");
    }
    return OmniReviewSettingsStore::resolveApiKeyLabel(uiKey, &context_->credentials, ignoreSaved);
}

QString AppController::credentialStatus(const QString &credentialId) const
{
    return context_->credentials.exists(credentialId)
        ? QStringLiteral("已安全保存") : QStringLiteral("尚未配置");
}

QString AppController::requestCredentialStatus(const QString &credentialId, int requestId)
{
    const QPointer<AppController> self(this);
    const auto store = context_->credentials;
    QThreadPool::globalInstance()->start([self, store, credentialId, requestId] {
        QElapsedTimer timer;
        timer.start();
        const QString status = store.exists(credentialId)
            ? QStringLiteral("已安全保存") : QStringLiteral("尚未配置");
        qCInfo(subcueAppLog) << "credential_status elapsed_ms=" << timer.elapsed();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, credentialId, status, requestId] {
            if (self) emit self->credentialStatusReady(credentialId, status, requestId);
        }, Qt::QueuedConnection);
    });
    return QStringLiteral("正在查询…");
}

QString AppController::verificationStatus(const QString &section, const QString &providerId) const
{
    Q_UNUSED(providerId);
    QString verifiedAt;
    if (section == QLatin1String("asr")) {
        verifiedAt = context_->settings.value(QStringLiteral("asrVerification")).toObject()
            .value(QStringLiteral("verifiedAtUtc")).toString();
    }
    if (verifiedAt.isEmpty()) {
        return QStringLiteral("未验证");
    }
    const QDateTime date = QDateTime::fromString(verifiedAt, Qt::ISODate).toLocalTime();
    return QStringLiteral("已验证 · %1").arg(date.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
}

QVariantList AppController::modelStatus(const QString &modelsRoot) const
{
    QVariantList result;
    QJsonObject settings = context_->settings;
    const QString trimmedRoot = modelsRoot.trimmed();
    if (!trimmedRoot.isEmpty()) {
        settings.insert(QStringLiteral("modelsRoot"), trimmedRoot);
    }
    const auto append = [&](ModelKind kind, const QString &id, const QString &name) {
        const QString directory = ModelLocator::directoryFor(kind, settings);
        result.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("ready"), ModelLocator::modelReady(directory)},
        });
    };
    append(ModelKind::Qwen3Asr, QStringLiteral("qwen3-asr-0.6b"), QStringLiteral("Qwen3-ASR 0.6B"));
    append(ModelKind::Qwen3ForcedAligner, QStringLiteral("qwen3-forced-aligner-0.6b"),
        QStringLiteral("Qwen3 Forced Aligner 0.6B"));
    append(ModelKind::FunAsrNano, QStringLiteral("fun-asr-nano-2512"), QStringLiteral("Fun-ASR Nano"));
    return result;
}

void AppController::requestStorageTargets()
{
    if (shuttingDown_) return;
    QPointer<AppController> self(this);
    const QJsonObject settings = context_->settings;
    backgroundTasks_.start([self, settings, cancel = &backgroundCancel_] {
        if (!self || cancel->load(std::memory_order_acquire)) return;
        StorageCleaner cleaner(
            QString::fromUtf8(SUBCUE_PROJECT_ROOT),
            ModelLocator::root(settings),
            QCoreApplication::applicationDirPath());
        const QVector<StorageTarget> scanned = cleaner.scan();
        QVariantList targets;
        targets.reserve(scanned.size());
        for (const StorageTarget &target : scanned) {
            targets.append(QVariantMap{
                {QStringLiteral("id"), target.id},
                {QStringLiteral("group"), target.group},
                {QStringLiteral("label"), target.label},
                {QStringLiteral("path"), target.path},
                {QStringLiteral("bytes"), target.bytes},
            });
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, targets] {
            if (!self) return;
            emit self->storageTargetsReady(targets);
        }, Qt::QueuedConnection);
    });
}

void AppController::cleanupStorage(const QStringList &ids)
{
    if (shuttingDown_) return;
    QPointer<AppController> self(this);
    const QJsonObject settings = context_->settings;
    backgroundTasks_.start([self, settings, ids, cancel = &backgroundCancel_] {
        StorageCleanupResult result;
        result.ok = false;
        result.error = QStringLiteral("任务已取消");
        if (self && !cancel->load(std::memory_order_acquire)) {
            StorageCleaner cleaner(
                QString::fromUtf8(SUBCUE_PROJECT_ROOT),
                ModelLocator::root(settings),
                QCoreApplication::applicationDirPath());
            result = cleaner.remove(ids, cancel);
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, result] {
            if (!self) return;
            emit self->storageCleanupFinished(QVariantMap{
                {QStringLiteral("success"), result.ok},
                {QStringLiteral("bytesRemoved"), result.bytesRemoved},
                {QStringLiteral("error"), result.error},
                {QStringLiteral("removed"), result.removed},
            });
        }, Qt::QueuedConnection);
    });
}

void AppController::shutdown()
{
    if (shuttingDown_) return;
    shuttingDown_ = true;
    backgroundCancel_ = true;
    backgroundTasks_.clear();
    alignmentCancel_ = true;
    omniReviewCancel_ = true;
    stopAlignmentWorker();
    stopOmniReviewWorker();
    stopWaveformWorker();
    backgroundTasks_.waitForDone();
    stop();
    tickTimer_.stop();
    playback_.setAudioDevice(nullptr, 0);
    if (context_) {
        context_->unregisterAudioClient(&playback_);
        context_->unbindAudioClients();
    }
    playback_.close();
    waveform_.reset();
    if (timelineItem_) timelineItem_->setWaveform({});
    if (context_ && context_->audioDevice) {
        context_->audioDevice->stop();
    }
    if (context_) {
        (void)context_->settingsManager.save(context_->settings);
    }
}

void AppController::releasePlayback()
{
    const bool wasPlaying = playing_;
    playing_ = false;
    ownsSharedAudio_ = false;
    playback_.pause();
    tickTimer_.stop();
    playback_.setAudioDevice(nullptr, 0);
    if (context_ && context_->audioDevice) context_->audioDevice->pause();
    if (wasPlaying) emit playbackChanged();
}

void AppController::claimPlayback()
{
    ownsSharedAudio_ = true;
    if (!playback_.isOpen() || !context_ || !context_->audioDevice) return;
    AppError audioError(ErrorDomain::Media, 0, QString());
    IAudioDevice *device = context_->audioDevice.get();
    if (!device->isStarted()
        && !device->start(playback_.outputSampleRate(), playback_.outputChannels(), &audioError)) {
        context_->replaceAudioDevice(createAudioDevice(AudioDeviceKind::Virtual));
        device = context_->audioDevice.get();
        if (device) (void)device->start(playback_.outputSampleRate(), playback_.outputChannels());
    }
    if (!device) return;
    device->setPlaybackRate(playbackRate_);
    playback_.setAudioDevice(device, kAudioPreRollMs);
    device->pause();
}

void AppController::applySubtitles(const QList<Subtitle> &subtitles)
{
    document_.setSubtitles(subtitles);
    selectedCue_ = subtitles.isEmpty() ? -1 : 0;
    emit selectedCueChanged();
    setCanExport(std::any_of(subtitles.cbegin(), subtitles.cend(),
                             [](const Subtitle &subtitle) { return subtitle.isExportable(); }));
    updateCurrentSubtitle();
    syncTimelineItem();
}

void AppController::setStatus(const QString &text)
{
    if (text == statusText_) {
        return;
    }
    statusText_ = text;
    emit statusChanged();
}

void AppController::setBusy(bool value)
{
    if (value == busy_) {
        return;
    }
    busy_ = value;
    emit busyChanged();
    emit canOmniReviewChanged();
}

void AppController::setCanExport(bool value)
{
    if (value == canExport_) {
        return;
    }
    canExport_ = value;
    emit canExportChanged();
}

void AppController::onTick()
{
    if (!playback_.isOpen()) {
        return;
    }
    if (playing_ && direction_ < 0) {
        const qint64 next = positionUs_ - static_cast<qint64>(
            elapsed_.nsecsElapsed() / 1'000 * playbackRate_);
        elapsed_.start();
        if (next <= 0) {
            seekUs(0);
            stop();
        } else {
            seekUs(next);
        }
        return;
    }
    if (playing_ && direction_ > 0 && !playback_.hasAudio()) {
        const qint64 elapsedUs = elapsed_.isValid() ? elapsed_.nsecsElapsed() / 1'000 : 10'000;
        elapsed_.start();
        playback_.consumeAudio(MediaTime::fromMicroseconds(static_cast<qint64>(
            std::max<qint64>(1, elapsedUs) * playbackRate_)));
    }
    // 音频渲染在设备自己的事件驱动线程里跑，主线程只负责泵入解码数据与画面调度。
    const bool wasPriming = playback_.isPriming();
    playback_.pump();
    if (wasPriming && !playback_.isPriming() && playing_ && context_->audioDevice) {
        context_->audioDevice->resume();
    }
    if (playing_ && direction_ > 0 && context_->audioDevice && !playback_.isPriming()
        && !context_->audioDevice->isHardware()) {
        (void)context_->audioDevice->renderFrame();
        playback_.pump();
    }
    updatePositionFromClock();
    pushPreviewFrame();
}

void AppController::updatePositionFromClock()
{
    const MediaTime now = playback_.position();
    if (!now.isValidRange()) {
        return;
    }
    const qint64 microseconds = std::clamp<qint64>(
        now.microseconds(), 0, durationUs_ > 0 ? durationUs_ : now.microseconds());
    if (microseconds == positionUs_) {
        updateCurrentSubtitle();
        return;
    }
    positionUs_ = microseconds;
    viewport_.setPlayhead(now);
    emit positionChanged();
    updateCurrentSubtitle();
    if (timelineItem_) {
        timelineItem_->setPlayheadUs(positionUs_);
    }
}

void AppController::updateCurrentSubtitle()
{
    QString text;
    const int index = subtitleIndex_.lookup(positionMs(), direction_);
    if (index >= 0 && index < document_.count()) text = document_.subtitles().at(index).text;
    if (text != currentSubtitle_) {
        currentSubtitle_ = text;
        emit currentSubtitleChanged();
    }
}

void AppController::pushPreviewFrame()
{
    if (!previewItem_ || !playback_.isOpen()) {
        return;
    }
    const DisplayedVideoFrame frame = playback_.displayedFrame();
    if (frame.image.isNull()) {
        return;
    }
    previewItem_->present(frame.image, frame.pts, frame.generation);
}

void AppController::syncTimelineItem()
{
    if (!timelineItem_) {
        return;
    }
    timelineItem_->setDurationUs(durationUs_ > 0 ? durationUs_ : TimelineViewport::kEmptyTimelineMs * 1000);
    timelineItem_->setPlayheadUs(positionUs_);
    timelineItem_->setView(viewport_.pixelsPerMs(), viewport_.scrollOffset());
    timelineItem_->setInPointUs(inPointUs());
    timelineItem_->setOutPointUs(outPointUs());
    if (selectedCue_ >= 0 && selectedCue_ < document_.count()) {
        timelineItem_->setSelectedCueId(document_.subtitles().at(selectedCue_).id);
    } else {
        timelineItem_->setSelectedCueId({});
    }
    timelineItem_->setSubtitles(document_.subtitles());
    timelineItem_->setWaveform(waveform_);
}

void AppController::stopAlignmentWorker()
{
    if (alignmentThread_.joinable()) {
        alignmentThread_.join();
    }
}

void AppController::stopOmniReviewWorker()
{
    if (omniReviewThread_.joinable()) {
        omniReviewThread_.join();
    }
}

void AppController::finishOmniSubtitleReview(quint64 generation, SubtitleOmniResult result,
    quint64 mediaGeneration, quint64 scriptGeneration, quint64 evidenceGeneration,
    const QString &model, const OmniUsage &usage)
{
    if (generation != omniReviewGeneration_.load()) return;
    setBusy(false);
    if (omniReviewCancel_) {
        setStatus(QStringLiteral("AI 复核已取消。"));
        return;
    }
    if (mediaGeneration != mediaGeneration_ || scriptGeneration != scriptGeneration_
        || evidenceGeneration != alignmentEvidenceGeneration_) {
        setStatus(QStringLiteral("媒体或文稿已变化，已丢弃复核结果。"));
        return;
    }
    if (std::holds_alternative<AppError>(result)) {
        setStatus(std::get<AppError>(result).userMessage());
        return;
    }
    const QVector<SubtitleOmniSuggestion> suggestions =
        std::get<QVector<SubtitleOmniSuggestion>>(std::move(result));
    QVector<Subtitle> updated;
    int changed = 0;
    for (const Subtitle &cue : document_.subtitles()) {
        Subtitle copy = cue;
        for (const SubtitleOmniSuggestion &suggestion : suggestions) {
            if (suggestion.segmentId != cue.id) continue;
            AiReviewService::applySubtitleSuggestion(&copy, suggestion);
            ++changed;
            break;
        }
        if (copy.metadata != cue.metadata) updated.append(copy);
    }
    if (!updated.isEmpty()) {
        commands_.replaceMany(updated, QStringLiteral("字幕内容复核标记"));
    }
    emit canOmniReviewChanged();
    setStatus(QStringLiteral("字幕内容复核完成：%1 条建议待确认。 %2")
        .arg(changed)
        .arg(formatOmniReviewSummary(model, usage)));
}

void AppController::finishWordMappingReview(quint64 generation, WordMappingOmniResult result,
    quint64 mediaGeneration, quint64 scriptGeneration, quint64 evidenceGeneration,
    const QString &model, const OmniUsage &usage)
{
    if (generation != omniReviewGeneration_.load()) return;
    setBusy(false);
    if (omniReviewCancel_) {
        setStatus(QStringLiteral("AI 复核已取消。"));
        return;
    }
    if (mediaGeneration != mediaGeneration_ || scriptGeneration != scriptGeneration_
        || evidenceGeneration != alignmentEvidenceGeneration_) {
        setStatus(QStringLiteral("媒体或文稿已变化，已丢弃复核结果。"));
        return;
    }
    if (std::holds_alternative<AppError>(result)) {
        setStatus(std::get<AppError>(result).userMessage());
        return;
    }
    const QVector<Subtitle> reviewed = std::get<QVector<Subtitle>>(std::move(result));
    QVector<Subtitle> changed;
    const QList<Subtitle> current = document_.subtitles();
    if (reviewed.size() != current.size()) {
        setStatus(QStringLiteral("时间映射复核结果与当前字幕不一致，已丢弃。"));
        return;
    }
    for (int index = 0; index < reviewed.size(); ++index) {
        if (reviewed.at(index).id != current.at(index).id) {
            setStatus(QStringLiteral("时间映射复核结果与当前字幕不一致，已丢弃。"));
            return;
        }
        const Subtitle &before = current.at(index);
        const Subtitle &after = reviewed.at(index);
        if (before.start != after.start || before.end != after.end
            || before.startWordId != after.startWordId || before.endWordId != after.endWordId
            || before.metadata != after.metadata || before.status != after.status
            || before.source != after.source) {
            changed.append(after);
        }
    }
    const QString summary = formatOmniReviewSummary(model, usage);
    if (changed.isEmpty()) {
        setStatus(QStringLiteral("时间映射复核完成：没有可应用的映射。 %1").arg(summary));
        return;
    }
    commands_.replaceMany(changed, QStringLiteral("复核时间映射"));
    setCanExport(std::any_of(document_.subtitles().cbegin(), document_.subtitles().cend(),
        [](const Subtitle &subtitle) { return subtitle.isExportable(); }));
    setStatus(QStringLiteral("时间映射复核完成：已更新 %1 条。 %2").arg(changed.size()).arg(summary));
}

void AppController::invalidateAlignmentEvidence()
{
    alignmentWords_.clear();
    ++alignmentEvidenceGeneration_;
}

void AppController::resetProgress(const QString &title, const QString &message)
{
    busyTaskTitle_ = title;
    alignmentProgress_ = 0;
    alignmentState_ = {title, message, 0, 0};
    alignmentProgressText_ = message;
    emit alignmentProgressChanged();
}

void AppController::reportOmniProgress(quint64 generation, int completed, int total, const QString &message)
{
    if (generation != omniReviewGeneration_.load() || omniReviewCancel_) return;
    alignmentState_ = {busyTaskTitle_, message, completed, total};
    alignmentProgressText_ = message;
    alignmentProgress_ = alignmentState_.percent();
    emit alignmentProgressChanged();
}

void AppController::finishAlignment(quint64 generation, AlignmentRunResult result)
{
    if (generation != alignmentGeneration_.load()) {
        return;
    }
    setCanExport(std::any_of(document_.subtitles().cbegin(), document_.subtitles().cend(),
                            [](const Subtitle &subtitle) { return subtitle.isExportable(); }));
    if (alignmentCancel_) {
        alignmentState_.stage = QStringLiteral("Canceled");
        emit alignmentProgressChanged();
        setBusy(false);
        setStatus(QStringLiteral("任务已取消。"));
        return;
    }
    if (std::holds_alternative<AppError>(result)) {
        const AppError error = std::get<AppError>(result);
        alignmentState_.stage = QStringLiteral("Failed");
        emit alignmentProgressChanged();
        setBusy(false);
        if (isAiCancelError(error)
            || error.userMessage() == QStringLiteral("任务已取消")
            || (error.domain() == ErrorDomain::Asr
                && error.code() == static_cast<int>(AsrErrorCode::Cancelled))) {
            alignmentState_.stage = QStringLiteral("Canceled");
            emit alignmentProgressChanged();
            setStatus(QStringLiteral("任务已取消。"));
            return;
        }
        setStatus(QStringLiteral("处理失败：%1").arg(error.userMessage()));
        return;
    }

    AlignmentTaskOutput output = std::get<AlignmentTaskOutput>(std::move(result));
    const bool completedReview = reviewRun_;
    QList<Subtitle> cues;
    cues.reserve(output.result.subtitles.size());
    for (const Subtitle &subtitle : output.result.subtitles) {
        cues.append(subtitle);
    }
    applySubtitles(cues);
    lastOutputPaths_ = output.outputPaths;
    mediaInfo_ = output.mediaInfo;
    alignmentWords_ = output.words;
    ++alignmentEvidenceGeneration_;
    setBusy(false);
    setCanExport(!output.result.exportableSubtitles().isEmpty());
    if (!alignmentCompleted_) {
        alignmentCompleted_ = true;
        emit canReviewChanged();
        emit canOmniReviewChanged();
    }
    setStatus(QStringLiteral("%1完成：已定位 %2 条，低置信 %3 条，音频未检出 %4 条")
                  .arg(completedReview ? QStringLiteral("复核") : QStringLiteral("打轴"))
                  .arg(output.result.exportableSubtitles().size())
                  .arg(output.result.lowCount())
                  .arg(output.result.skippedCount()));
    reviewRun_ = false;
    if (!completedReview && context_->settings.value(QStringLiteral("autoReviewEnabled")).toBool()) {
        QMetaObject::invokeMethod(this, &AppController::startReview, Qt::QueuedConnection);
    }
}

void AppController::stopWaveformWorker()
{
    waveformCancel_ = true;
    ++waveformGeneration_;
    if (waveformThread_.joinable()) {
        waveformThread_.join();
    }
    waveformCancel_ = false;
}

void AppController::startWaveformWorker(const QString &path)
{
    stopWaveformWorker();
    waveform_.reset();
    if (timelineItem_) timelineItem_->setWaveform({});
    const quint64 generation = ++waveformGeneration_;
    waveformThread_ = std::thread([this, path, generation] {
        WaveformGenerator generator;
        const WaveformGenerator::Result result = generator.generate(
            path, -1, WaveformGenerator::kDefaultSampleRate, &waveformCancel_, &waveformGeneration_,
            generation);
        QPointer<AppController> self(this);
        QMetaObject::invokeMethod(this, [self, generation, result = result] {
            if (!self || generation != self->waveformGeneration_.load()) {
                return;
            }
            if (std::holds_alternative<std::shared_ptr<const WaveformPyramid>>(result)) {
                self->waveform_ = std::get<std::shared_ptr<const WaveformPyramid>>(result);
                self->syncTimelineItem();
            }
        }, Qt::QueuedConnection);
    });
}

QString AppController::localPath(const QUrl &url) const
{
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    // 兼容 Windows 盘符路径；远程 URL 不作为本地素材打开。
    const QString text = QUrl::fromPercentEncoding(url.toEncoded());
    return QDir::isAbsolutePath(text) ? QDir::fromNativeSeparators(text) : QString();
}

bool AppController::isMediaPath(const QString &path) const
{
    return kMediaExtensions.contains(QFileInfo(path).suffix().prepend(QLatin1Char('.')).toLower());
}

int AppController::activeRow() const
{
    if (selectedCue_ >= 0 && selectedCue_ < document_.count()) {
        return selectedCue_;
    }
    const qint64 position = positionMs();
    for (int index = 0; index < document_.count(); ++index) {
        const Subtitle &subtitle = document_.subtitles().at(index);
        if (subtitle.isTimed() && subtitle.start.milliseconds() <= position
            && position < subtitle.end.milliseconds()) {
            return index;
        }
    }
    return -1;
}

bool AppController::setTimingMs(int row, qint64 startMs, qint64 endMs)
{
    if (row < 0 || row >= document_.count()) {
        return false;
    }
    const Subtitle &subtitle = document_.subtitles().at(row);
    if (!editor_.setTiming(subtitle.id, MediaTime::fromMilliseconds(startMs),
                           MediaTime::fromMilliseconds(endMs))) {
        setStatus(QStringLiteral("字幕时长不能短于 250ms。"));
        return false;
    }
    setCanExport(true);
    updateCurrentSubtitle();
    syncTimelineItem();
    return true;
}

std::pair<qint64, qint64> AppController::defaultNewRange(int row) const
{
    if (inPointMs_ >= 0 && outPointMs_ > inPointMs_ && outPointMs_ <= durationMs()) {
        return {inPointMs_, outPointMs_};
    }
    const qint64 position = positionMs();
    qint64 following = durationMs();
    for (int index = row; index < document_.count(); ++index) {
        const Subtitle &subtitle = document_.subtitles().at(index);
        if (subtitle.isTimed() && subtitle.start.milliseconds() > position) {
            following = subtitle.start.milliseconds();
            break;
        }
    }
    return {position, std::min({durationMs(), position + 2'000, following})};
}

std::pair<int, int> AppController::rowsAroundPlayhead() const
{
    int before = -1;
    int after = -1;
    const qint64 position = positionMs();
    for (int index = 0; index < document_.count(); ++index) {
        const Subtitle &subtitle = document_.subtitles().at(index);
        if (!subtitle.isTimed()) {
            continue;
        }
        if (subtitle.end.milliseconds() <= position) {
            before = index;
        } else if (subtitle.start.milliseconds() >= position && after < 0) {
            after = index;
        }
    }
    return {before, after};
}

void AppController::emitMediaChanged()
{
    emit mediaChanged();
    syncTimelineItem();
}

} // namespace subcue
