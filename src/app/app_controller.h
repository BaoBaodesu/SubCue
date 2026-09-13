#pragma once

#include "alignment/alignment_pipeline.h"
#include "media/media_types.h"
#include "playback/playback_engine.h"
#include "subtitle/subtitle_command_manager.h"
#include "subtitle/subtitle_document.h"
#include "subtitle/subtitle_model.h"
#include "timeline/snap_engine.h"
#include "timeline/timeline_editor.h"
#include "timeline/timeline_viewport.h"
#include "waveform/waveform_pyramid.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QImage>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace subcue {

class ApplicationContext;
class TimelineSceneItem;
class VideoPreviewItem;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *subtitleModel READ subtitleModel CONSTANT)
    Q_PROPERTY(QVariantList projectFiles READ projectFiles NOTIFY projectFilesChanged)
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY mediaChanged)
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY mediaChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY mediaChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY mediaChanged)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY mediaChanged)
    Q_PROPERTY(qint64 timelineDurationMs READ timelineDurationMs NOTIFY mediaChanged)
    Q_PROPERTY(double fps READ fps NOTIFY mediaChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(int direction READ direction NOTIFY playbackChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY playbackChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString alignmentProgressText READ alignmentProgressText NOTIFY alignmentProgressChanged)
    Q_PROPERTY(int alignmentProgress READ alignmentProgress NOTIFY alignmentProgressChanged)
    Q_PROPERTY(QString alignmentStage READ alignmentStage NOTIFY alignmentProgressChanged)
    Q_PROPERTY(int alignmentCompleted READ alignmentCompleted NOTIFY alignmentProgressChanged)
    Q_PROPERTY(int alignmentTotal READ alignmentTotal NOTIFY alignmentProgressChanged)
    Q_PROPERTY(bool alignmentIndeterminate READ alignmentIndeterminate NOTIFY alignmentProgressChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY canExportChanged)
    Q_PROPERTY(int selectedCue READ selectedCue NOTIFY selectedCueChanged)
    Q_PROPERTY(QString scriptText READ scriptText WRITE setScriptText NOTIFY scriptTextChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled NOTIFY snapEnabledChanged)
    Q_PROPERTY(double pixelsPerMs READ pixelsPerMs NOTIFY timelineViewChanged)
    Q_PROPERTY(int zoomPercent READ zoomPercent NOTIFY timelineViewChanged)
    Q_PROPERTY(double scrollOffset READ scrollOffset NOTIFY timelineViewChanged)
    Q_PROPERTY(qint64 inPoint READ inPoint NOTIFY rangeChanged)
    Q_PROPERTY(qint64 outPoint READ outPoint NOTIFY rangeChanged)
    Q_PROPERTY(qint64 inPointUs READ inPointUs NOTIFY rangeChanged)
    Q_PROPERTY(qint64 outPointUs READ outPointUs NOTIFY rangeChanged)
    Q_PROPERTY(QString currentSubtitleText READ currentSubtitleText NOTIFY currentSubtitleChanged)
    Q_PROPERTY(QString subtitleFontFamily READ subtitleFontFamily NOTIFY settingsChanged)
    Q_PROPERTY(int subtitleFontSize READ subtitleFontSize NOTIFY settingsChanged)
    Q_PROPERTY(QString subtitleFontColor READ subtitleFontColor NOTIFY settingsChanged)
    Q_PROPERTY(QString subtitleOutlineColor READ subtitleOutlineColor NOTIFY settingsChanged)
    Q_PROPERTY(QString subtitleAlignment READ subtitleAlignment NOTIFY settingsChanged)
    Q_PROPERTY(int subtitleBottomMargin READ subtitleBottomMargin NOTIFY settingsChanged)
    Q_PROPERTY(QString audioBackendId READ audioBackendId NOTIFY mediaChanged)
    Q_PROPERTY(QString previewBackendId READ previewBackendId NOTIFY previewChanged)

public:
    explicit AppController(ApplicationContext *context, QObject *parent = nullptr);
    ~AppController() override;

    [[nodiscard]] QObject *subtitleModel() const;
    [[nodiscard]] QVariantList projectFiles() const { return projectFiles_; }
    [[nodiscard]] SubtitleDocument *document() noexcept { return &document_; }
    [[nodiscard]] SubtitleCommandManager *commands() noexcept { return &commands_; }
    [[nodiscard]] PlaybackEngine &playback() noexcept { return playback_; }
    [[nodiscard]] const PlaybackEngine &playback() const noexcept { return playback_; }

    [[nodiscard]] QString mediaPath() const;
    [[nodiscard]] QString mediaName() const;
    [[nodiscard]] bool hasMedia() const;
    [[nodiscard]] bool hasVideo() const;
    [[nodiscard]] qint64 durationMs() const;
    [[nodiscard]] qint64 durationUs() const;
    [[nodiscard]] qint64 timelineDurationMs() const;
    [[nodiscard]] double fps() const;
    [[nodiscard]] qint64 positionMs() const;
    [[nodiscard]] qint64 positionUs() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] int direction() const;
    [[nodiscard]] double playbackRate() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] QString alignmentProgressText() const { return alignmentProgressText_; }
    [[nodiscard]] int alignmentProgress() const { return alignmentProgress_; }
    [[nodiscard]] QString alignmentStage() const { return alignmentState_.stage; }
    [[nodiscard]] int alignmentCompleted() const { return alignmentState_.completed; }
    [[nodiscard]] int alignmentTotal() const { return alignmentState_.total; }
    [[nodiscard]] bool alignmentIndeterminate() const { return alignmentState_.indeterminate(); }
    [[nodiscard]] bool canExport() const;
    [[nodiscard]] int selectedCue() const;
    [[nodiscard]] QString scriptText() const;
    void setScriptText(const QString &value);
    [[nodiscard]] bool snapEnabled() const;
    [[nodiscard]] double pixelsPerMs() const;
    [[nodiscard]] int zoomPercent() const;
    [[nodiscard]] double scrollOffset() const;
    [[nodiscard]] qint64 inPoint() const;
    [[nodiscard]] qint64 outPoint() const;
    [[nodiscard]] qint64 inPointUs() const;
    [[nodiscard]] qint64 outPointUs() const;
    [[nodiscard]] QString currentSubtitleText() const;
    [[nodiscard]] QString subtitleFontFamily() const;
    [[nodiscard]] int subtitleFontSize() const;
    [[nodiscard]] QString subtitleFontColor() const;
    [[nodiscard]] QString subtitleOutlineColor() const;
    [[nodiscard]] QString subtitleAlignment() const;
    [[nodiscard]] int subtitleBottomMargin() const;
    [[nodiscard]] QString audioBackendId() const;
    [[nodiscard]] QString previewBackendId() const;

    Q_INVOKABLE void setPreviewItem(QObject *item);
    Q_INVOKABLE void setTimelineItem(QObject *item);
    Q_INVOKABLE void loadMedia(const QUrl &url);
    Q_INVOKABLE void loadMediaPath(const QString &path);
    Q_INVOKABLE void importScript(const QUrl &url);
    Q_INVOKABLE void importScriptPath(const QString &path);
    Q_INVOKABLE void handleDroppedUrl(const QString &value);
    Q_INVOKABLE void importFiles(const QList<QUrl> &urls);
    Q_INVOKABLE bool canImportFiles(const QList<QUrl> &urls) const;
    Q_INVOKABLE void openProjectFile(int row);
    Q_INVOKABLE void removeProjectFile(int row);
    Q_INVOKABLE void showProjectFileFolder(int row);
    Q_INVOKABLE QString localPath(const QUrl &url) const;
    Q_INVOKABLE void startAlignment();
    Q_INVOKABLE void cancelAlignment();
    Q_INVOKABLE void exportSubtitles();
    Q_INVOKABLE void createNextScriptCue();
    void setAlignmentOverrides(IAsrService *asr, IAiProvider *ai = nullptr);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void playForward();
    Q_INVOKABLE void playReverse();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void seekUs(qint64 positionUs);
    Q_INVOKABLE void stepFrames(int frames);
    Q_INVOKABLE int frameDeltaMs(int frames) const;
    Q_INVOKABLE void selectCue(int row, bool seekToCue = true);
    Q_INVOKABLE void navigateCue(int delta);
    Q_INVOKABLE void setCueText(int row, const QString &text);
    Q_INVOKABLE void locateCue(int row);
    Q_INVOKABLE void locateCueAt(const QString &id, qint64 startMs);
    Q_INVOKABLE void confirmCue(int row);
    Q_INVOKABLE void undo() { commands_.undo(); }
    Q_INVOKABLE void redo() { commands_.redo(); }
    Q_INVOKABLE void deleteCue() {
        if (selectedCue_ >= 0 && selectedCue_ < document_.count())
            commands_.remove(document_.subtitles().at(selectedCue_).id);
    }
    Q_INVOKABLE void createOrEditCue();
    Q_INVOKABLE void splitCurrentCue();
    Q_INVOKABLE void joinAroundPlayhead();
    Q_INVOKABLE void setCurrentStart();
    Q_INVOKABLE void setCurrentEnd();
    Q_INVOKABLE void setFollowingStart();
    Q_INVOKABLE void setPreviousEnd();
    Q_INVOKABLE void toggleSnap();
    Q_INVOKABLE void setInPoint();
    Q_INVOKABLE void setOutPoint();
    Q_INVOKABLE void clearInPoint();
    Q_INVOKABLE void clearOutPoint();
    Q_INVOKABLE void adjustZoomPercent(int delta, double viewportWidth);
    Q_INVOKABLE void fitTimeline(double viewportWidth);
    Q_INVOKABLE void syncTimelineView(double pixelsPerMs, double scrollOffset);
    Q_INVOKABLE QString formatTime(qint64 ms) const;
    Q_INVOKABLE QVariant setting(const QString &key) const;
    Q_INVOKABLE QVariantList asrProviders() const;
    Q_INVOKABLE QVariantList asrModels(const QString &providerId) const;
    Q_INVOKABLE bool whisperCudaAvailable() const;
    Q_INVOKABLE QVariantList aiProviders() const;
    Q_INVOKABLE QVariantList aiModels(const QString &providerId) const;
    Q_INVOKABLE QString newProviderId() const;
    Q_INVOKABLE void testAiConnection(const QVariantMap &provider, const QString &apiKey = {});
    Q_INVOKABLE void testAsrConnection(const QVariantMap &values, const QString &apiKey = {});
    Q_INVOKABLE void downloadWhisperModel(const QString &modelId, const QString &directory = {});
    Q_INVOKABLE bool saveSettings(const QVariantMap &values, const QString &asrApiKey = {},
                                  const QString &aiApiKey = {});
    Q_INVOKABLE QString credentialStatus(const QString &credentialId = QStringLiteral("SubCue/ASR/dashscope")) const;
    Q_INVOKABLE void requestAsrModels(const QString &providerId, const QString &directory, int requestId);
    Q_INVOKABLE QString requestCredentialStatus(const QString &credentialId = QStringLiteral("SubCue/ASR/dashscope"), int requestId = 0);
    Q_INVOKABLE QString verificationStatus(const QString &section, const QString &providerId = {}) const;
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void applySubtitles(const QList<Subtitle> &subtitles);

signals:
    void asrModelsReady(int requestId, const QVariantList &models);
    void credentialStatusReady(const QString &credentialId, const QString &status, int requestId);
    void projectFilesChanged();
    void mediaChanged();
    void positionChanged();
    void playbackChanged();
    void statusChanged();
    void busyChanged();
    void alignmentProgressChanged();
    void canExportChanged();
    void selectedCueChanged();
    void scriptTextChanged();
    void snapEnabledChanged();
    void timelineViewChanged();
    void rangeChanged();
    void currentSubtitleChanged();
    void settingsChanged();
    void previewChanged();
    void editCueRequested(int row, const QString &text);
    void alignmentPreflightFailed(const QVariantList &issues);
    void aiConnectionTestFinished(const QString &providerId, const QVariantMap &result);
    void asrConnectionTestFinished(const QVariantMap &result);
    void whisperDownloadFinished(const QString &modelId, const QVariantMap &result);

private:
    void rememberProjectFile(const QString &path, const QString &type, qint64 durationMs = 0);
    void setStatus(const QString &text);
    void setBusy(bool value);
    void setCanExport(bool value);
    void onTick();
    void updatePositionFromClock();
    void updateCurrentSubtitle();
    void pushPreviewFrame();
    void syncTimelineItem();
    void stopWaveformWorker();
    void startWaveformWorker(const QString &path);
    void stopAlignmentWorker();
    void finishAlignment(quint64 generation, AlignmentRunResult result);
    [[nodiscard]] bool isMediaPath(const QString &path) const;
    [[nodiscard]] int activeRow() const;
    [[nodiscard]] bool setTimingMs(int row, qint64 startMs, qint64 endMs);
    [[nodiscard]] std::pair<int, int> rowsAroundPlayhead() const;
    [[nodiscard]] std::pair<qint64, qint64> defaultNewRange(int row) const;
    void emitMediaChanged();

    ApplicationContext *context_ = nullptr;
    SubtitleDocument document_;
    SubtitleModel model_;
    SubtitleCommandManager commands_;
    TimelineViewport viewport_;
    SnapEngine snap_;
    TimelineEditor editor_;
    PlaybackEngine playback_;
    QTimer tickTimer_;
    QElapsedTimer elapsed_;
    QPointer<VideoPreviewItem> previewItem_;
    QPointer<TimelineSceneItem> timelineItem_;
    std::shared_ptr<const WaveformPyramid> waveform_;
    std::thread waveformThread_;
    std::atomic<bool> waveformCancel_{false};
    std::atomic<quint64> waveformGeneration_{0};
    std::thread alignmentThread_;
    std::atomic<bool> alignmentCancel_{false};
    std::atomic<quint64> alignmentGeneration_{0};
    IAsrService *asrOverride_ = nullptr;
    IAiProvider *aiOverride_ = nullptr;
    MediaInfo mediaInfo_;
    QStringList lastOutputPaths_;
    QVariantList projectFiles_;

    QString mediaPath_;
    QString statusText_ = QStringLiteral("就绪");
    QString scriptText_;
    QString currentSubtitle_;
    qint64 durationUs_ = 0;
    qint64 positionUs_ = 0;
    qint64 inPointMs_ = -1;
    qint64 outPointMs_ = -1;
    double fps_ = 25.0;
    double playbackRate_ = 1.0;
    int selectedCue_ = -1;
    int direction_ = 1;
    bool playing_ = false;
    bool busy_ = false;
    QString alignmentProgressText_;
    int alignmentProgress_ = 0;
    AlignmentProgressState alignmentState_;
    bool canExport_ = false;
    bool hasVideo_ = false;
};

} // namespace subcue
