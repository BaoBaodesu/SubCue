#pragma once

#include "alignment/alignment_pipeline.h"
#include "ai/ai_review_types.h"
#include "media/media_types.h"
#include "playback/playback_engine.h"
#include "subtitle/subtitle_command_manager.h"
#include "subtitle/subtitle_document.h"
#include "subtitle/subtitle_model.h"
#include "subtitle/subtitle_time_index.h"
#include "timeline/snap_engine.h"
#include "timeline/timeline_editor.h"
#include "timeline/timeline_viewport.h"
#include "waveform/waveform_pyramid.h"
#include "project/project.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QImage>

#include <atomic>
#include <functional>
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
    Q_PROPERTY(QObject *filteredSubtitleModel READ filteredSubtitleModel CONSTANT)
    Q_PROPERTY(QString selectedCueId READ selectedCueId NOTIFY selectedCueChanged)
    Q_PROPERTY(int overlapCount READ overlapCount NOTIFY overlapChanged)
    Q_PROPERTY(int overlappingCueCount READ overlappingCueCount NOTIFY overlapChanged)
    Q_PROPERTY(QVariantList currentSubtitleItems READ currentSubtitleItems NOTIFY currentSubtitleChanged)
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
    Q_PROPERTY(QString busyTaskTitle READ busyTaskTitle NOTIFY alignmentProgressChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY canExportChanged)
    Q_PROPERTY(bool canReview READ canReview NOTIFY canReviewChanged)
    Q_PROPERTY(bool canOmniReview READ canOmniReview NOTIFY canOmniReviewChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY busyChanged)
    Q_PROPERTY(bool mediaAvailable READ hasMedia NOTIFY mediaChanged)
    Q_PROPERTY(QVariantMap sessionState READ sessionState NOTIFY projectChanged)
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
    [[nodiscard]] QObject *filteredSubtitleModel() const { return const_cast<SubtitleFilterModel *>(&filteredModel_); }
    [[nodiscard]] QString selectedCueId() const { return selectedCueId_; }
    [[nodiscard]] int overlapCount() const { return static_cast<int>(subtitleIndex_.overlapRanges().size()); }
    [[nodiscard]] int overlappingCueCount() const { return static_cast<int>(subtitleIndex_.overlappingIds().size()); }
    [[nodiscard]] QVariantList currentSubtitleItems() const { return currentSubtitleItems_; }
    Q_INVOKABLE bool cueOverlaps(const QString &id) const { return subtitleIndex_.overlappingIds().contains(id); }
    Q_INVOKABLE int cueRowForId(const QString &id) const { return document_.indexOf(id); }
    Q_INVOKABLE bool applyCueEdit(const QString &id, const QString &text, qint64 startUs, qint64 endUs);
    Q_INVOKABLE void navigateOverlap(int delta);
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
    [[nodiscard]] QString busyTaskTitle() const { return busyTaskTitle_; }
    [[nodiscard]] bool canExport() const;
    [[nodiscard]] bool canReview() const { return alignmentCompleted_; }
    [[nodiscard]] bool canOmniReview() const;
    [[nodiscard]] bool modified() const noexcept { return modified_; }
    [[nodiscard]] QString projectPath() const { return projectPath_; }
    [[nodiscard]] bool canSave() const { return !busy_ && !shuttingDown_; }
    [[nodiscard]] QVariantMap sessionState() const { return projectData_.state.value(QStringLiteral("session")).toObject().toVariantMap(); }
    Q_INVOKABLE void setSessionState(const QVariantMap &state) { projectData_.state.insert(QStringLiteral("session"), QJsonObject::fromVariantMap(state)); }
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
    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool saveProject(const QUrl &url);
    Q_INVOKABLE bool saveCurrentProject();
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void relinkMedia(const QUrl &url);
    Q_INVOKABLE void importTimedSubtitles(const QUrl &url);
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
    Q_INVOKABLE void startReview();
    Q_INVOKABLE void startOmniSubtitleReview();
    Q_INVOKABLE void startWordMappingReview();
    Q_INVOKABLE void acceptOmniSubtitleSuggestion(int row);
    Q_INVOKABLE void ignoreOmniSubtitleSuggestion(int row);
    Q_INVOKABLE void cancelAlignment();
    Q_INVOKABLE void exportSubtitles(const QString &outputDirectory = {});
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
    Q_INVOKABLE void navigatePendingCue(int delta);
    Q_INVOKABLE void navigateMismatchCue(int delta);
    Q_INVOKABLE void seekToCurrentStart();
    Q_INVOKABLE void seekToCurrentEnd();
    Q_INVOKABLE void seekToTimelineStart();
    Q_INVOKABLE void seekToTimelineEnd();
    Q_INVOKABLE void nudgeCurrentStart(int frames);
    Q_INVOKABLE void nudgeCurrentEnd(int frames);
    Q_INVOKABLE void setCueText(int row, const QString &text);
    Q_INVOKABLE void locateCue(int row);
    Q_INVOKABLE void locateCueAt(const QString &id, qint64 startMs);
    Q_INVOKABLE void confirmCue(int row);
    Q_INVOKABLE void confirmCurrentCue();
    Q_INVOKABLE void clearModified();
    Q_INVOKABLE void undo() { commands_.undo(); }
    Q_INVOKABLE void redo() { commands_.redo(); }
    Q_INVOKABLE void deleteCue();
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
    Q_INVOKABLE void testAiConnection(const QString &apiKey = {});
    Q_INVOKABLE void testAsrConnection(const QVariantMap &values, const QString &apiKey = {});
    Q_INVOKABLE bool saveSettings(const QVariantMap &values, const QString &asrApiKey = {},
                                  const QString &aiApiKey = {});
    Q_INVOKABLE QString aiKeySource(const QString &uiKey = {}, bool ignoreSaved = false) const;
    Q_INVOKABLE QString credentialStatus(const QString &credentialId = QStringLiteral("SubCue/ASR/dashscope")) const;
    Q_INVOKABLE void requestAsrModels(const QString &providerId, const QString &directory, int requestId);
    Q_INVOKABLE QString requestCredentialStatus(const QString &credentialId = QStringLiteral("SubCue/ASR/dashscope"), int requestId = 0);
    Q_INVOKABLE QString verificationStatus(const QString &section, const QString &providerId = {}) const;
    Q_INVOKABLE QVariantList modelStatus(const QString &modelsRoot = {}) const;
    Q_INVOKABLE void requestStorageTargets();
    Q_INVOKABLE void cleanupStorage(const QStringList &ids);
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void applySubtitles(const QList<Subtitle> &subtitles);
    void releasePlayback();
    void claimPlayback();

signals:
    void projectChanged();
    void projectSaveFinished(bool success, const QString &path, const QString &message);
    void projectOpenFinished(bool success, const QString &path, const QString &message);
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
    void canReviewChanged();
    void canOmniReviewChanged();
    void modifiedChanged();
    void selectedCueChanged();
    void scriptTextChanged();
    void snapEnabledChanged();
    void timelineViewChanged();
    void rangeChanged();
    void currentSubtitleChanged();
    void settingsChanged();
    void previewChanged();
    void editCueRequested(int row, const QString &text);
    void cueDraftRequested(const QString &id, const QString &text, qint64 startUs, qint64 endUs);
    void overlapChanged();
    void alignmentPreflightFailed(const QVariantList &issues);
    void aiConnectionTestFinished(const QVariantMap &result);
    void asrConnectionTestFinished(const QVariantMap &result);
    void exportFinished(bool success, const QString &message, const QStringList &paths);
    void storageTargetsReady(const QVariantList &targets);
    void storageCleanupFinished(const QVariantMap &result);

private:
    [[nodiscard]] Project projectSnapshot() const;
    [[nodiscard]] QByteArray projectFingerprint() const;
    void applyProject(const Project &project, const QString &path, bool available);
    void rememberProjectFile(const QString &path, const QString &type, qint64 durationMs = 0);
    void setStatus(const QString &text);
    void setBusy(bool value);
    void setCanExport(bool value);
    void setModified(bool value);
    void stampMultilineFlag(Subtitle *subtitle) const;
    void refreshMultilineFlags();
    [[nodiscard]] bool textWrapsToMultipleLines(const QString &text) const;
    [[nodiscard]] bool cueNeedsConfirm(const Subtitle &cue) const;
    [[nodiscard]] bool cueMismatchesAudio(const Subtitle &cue) const;
    [[nodiscard]] int findCueRow(int delta, const std::function<bool(const Subtitle &)> &predicate) const;
    void onTick();
    void updatePositionFromClock();
    void updateCurrentSubtitle();
    void pushPreviewFrame();
    void syncTimelineItem();
    void onDocumentChanged();
    void stopWaveformWorker();
    void startWaveformWorker(const QString &path);
    void stopAlignmentWorker();
    void stopOmniReviewWorker();
    void finishAlignment(quint64 generation, AlignmentRunResult result);
    void finishOmniSubtitleReview(quint64 generation, SubtitleOmniResult result,
        quint64 mediaGeneration, quint64 scriptGeneration, quint64 evidenceGeneration,
        const QString &model, const OmniUsage &usage);
    void finishWordMappingReview(quint64 generation, WordMappingOmniResult result,
        quint64 mediaGeneration, quint64 scriptGeneration, quint64 evidenceGeneration,
        const QString &model, const OmniUsage &usage);
    void startAlignmentRun(bool review);
    void invalidateAlignmentEvidence();
    void resetProgress(const QString &title, const QString &message);
    void reportOmniProgress(quint64 generation, int completed, int total, const QString &message);
    [[nodiscard]] bool isMediaPath(const QString &path) const;
    [[nodiscard]] int activeRow() const;
    [[nodiscard]] bool setTimingMs(int row, qint64 startMs, qint64 endMs);
    [[nodiscard]] std::pair<int, int> rowsAroundPlayhead() const;
    [[nodiscard]] std::pair<qint64, qint64> defaultNewRange(int row) const;
    void emitMediaChanged();

    ApplicationContext *context_ = nullptr;
    SubtitleDocument document_;
    SubtitleTimeIndex subtitleIndex_;
    SubtitleModel model_;
    SubtitleFilterModel filteredModel_;
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
    std::thread omniReviewThread_;
    std::atomic<bool> omniReviewCancel_{false};
    std::atomic<quint64> omniReviewGeneration_{0};
    QThreadPool backgroundTasks_;
    std::atomic<bool> backgroundCancel_{false};
    IAsrService *asrOverride_ = nullptr;
    IAiProvider *aiOverride_ = nullptr;
    MediaInfo mediaInfo_;
    QStringList lastOutputPaths_;
    QVariantList projectFiles_;
    Project projectData_;
    QString projectPath_;
    QByteArray savedFingerprint_;

    QString mediaPath_;
    QString statusText_ = QStringLiteral("就绪");
    QString scriptText_;
    QString currentSubtitle_;
    QString selectedCueId_;
    QVariantList currentSubtitleItems_;
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
    QString busyTaskTitle_;
    int alignmentProgress_ = 0;
    AlignmentProgressState alignmentState_;
    bool canExport_ = false;
    bool alignmentCompleted_ = false;
    bool reviewRun_ = false;
    bool hasVideo_ = false;
    bool shuttingDown_ = false;
    bool ownsSharedAudio_ = true;
    bool modified_ = false;
    bool refreshingMultiline_ = false;
    quint64 mediaGeneration_ = 0;
    quint64 scriptGeneration_ = 0;
    quint64 alignmentEvidenceGeneration_ = 0;
    QVector<TranscriptWord> alignmentWords_;
};

} // namespace subcue
