#pragma once

#include "alignment/transcript.h"
#include "asr/asr_service.h"
#include "playback/playback_engine.h"
#include "rough_cut_result_model.h"
#include "roughcut/timeline_engine.h"
#include "roughcut/auxiliary_recognition.h"

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <atomic>
#include <thread>

namespace subcue {

class ApplicationContext;
class TimelineSceneItem;

class RoughCutController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *resultModel READ resultModel CONSTANT)
    Q_PROPERTY(QObject *resultFilterModel READ resultFilterModel CONSTANT)
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString scriptPath READ scriptPath NOTIFY scriptChanged)
    Q_PROPERTY(QString scriptText READ scriptText WRITE setScriptText NOTIFY scriptChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged)
    Q_PROPERTY(QString busyTaskTitle READ busyTaskTitle NOTIFY progressChanged)
    Q_PROPERTY(bool progressIndeterminate READ progressIndeterminate NOTIFY progressChanged)
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY busyChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(bool timelineActive READ timelineActive NOTIFY playbackChanged)
    Q_PROPERTY(bool timelinePaused READ timelinePaused NOTIFY playbackChanged)
    Q_PROPERTY(bool continuousAudition READ continuousAudition WRITE setContinuousAudition NOTIFY playbackChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY playbackChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY mediaChanged)
    Q_PROPERTY(int resultCount READ resultCount NOTIFY resultsChanged)
    Q_PROPERTY(bool canAiReview READ canAiReview NOTIFY canAiReviewChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY canExportChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY statusFilterChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY canSaveChanged)

public:
    explicit RoughCutController(ApplicationContext *context, QObject *parent = nullptr);
    ~RoughCutController() override;

    [[nodiscard]] QObject *resultModel() noexcept { return &model_; }
    [[nodiscard]] QObject *resultFilterModel() noexcept { return &filterModel_; }
    [[nodiscard]] QString mediaPath() const { return mediaPath_; }
    [[nodiscard]] QString scriptPath() const { return scriptPath_; }
    [[nodiscard]] QString scriptText() const { return scriptText_; }
    [[nodiscard]] QString projectPath() const { return projectPath_; }
    [[nodiscard]] QString statusText() const { return statusText_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] int progressPercent() const noexcept { return progressPercent_; }
    [[nodiscard]] QString busyTaskTitle() const { return busyTaskTitle_; }
    [[nodiscard]] bool progressIndeterminate() const noexcept { return progressIndeterminate_; }
    [[nodiscard]] bool cancelling() const noexcept { return cancelling_; }
    [[nodiscard]] bool playing() const noexcept { return !playback_.isPaused(); }
    [[nodiscard]] bool timelineActive() const noexcept { return timelinePlaybackIndex_ >= 0; }
    [[nodiscard]] bool timelinePaused() const noexcept { return timelinePaused_; }
    [[nodiscard]] bool continuousAudition() const noexcept { return continuousAudition_; }
    [[nodiscard]] double playbackRate() const noexcept { return playbackRate_; }
    [[nodiscard]] bool canUndo() const noexcept { return !busy_ && historyIndex_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return !busy_ && historyIndex_ < history_.size(); }
    [[nodiscard]] qint64 positionMs() const;
    [[nodiscard]] qint64 durationMs() const;
    [[nodiscard]] int resultCount() const { return model_.rowCount(); }
    [[nodiscard]] bool canAiReview() const { return model_.rowCount() > 0 && !busy_; }
    [[nodiscard]] bool canExport() const noexcept { return !timeline_.isEmpty() && !busy_; }
    [[nodiscard]] QString statusFilter() const { return filterModel_.statusFilter(); }
    void setStatusFilter(const QString &value) { filterModel_.setStatusFilter(value); }
    [[nodiscard]] bool modified() const noexcept { return modified_; }
    [[nodiscard]] bool canSave() const noexcept { return !mediaPath_.isEmpty() && !busy_; }
    [[nodiscard]] int sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] const QVector<RecognizedPassage> &recording() const noexcept { return model_.recording(); }
    [[nodiscard]] const QVector<RoughCutSegmentDecision> &decisions() const noexcept { return model_.decisions(); }

    Q_INVOKABLE void loadMedia(const QUrl &url);
    Q_INVOKABLE void loadScript(const QUrl &url);
    Q_INVOKABLE void setScriptText(const QString &text);
    Q_INVOKABLE bool saveProject(const QUrl &url);
    Q_INVOKABLE bool saveCurrentProject();
    Q_INVOKABLE void openProject(const QUrl &url);
    void setAnalysisOverrides(IAsrService *asr);
    void releasePlayback();
    void claimPlayback();
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void setSourceWaveformItem(QObject *item);
    Q_INVOKABLE void setTimelineItem(QObject *item);
    Q_INVOKABLE void startAnalysis();
    Q_INVOKABLE void cancelAnalysis();
    Q_INVOKABLE void startAuxiliaryRecognition();
    Q_INVOKABLE void startAiReview();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void seekTimeline(qint64 positionMs);
    Q_INVOKABLE void locateResult(int row);
    Q_INVOKABLE int sourceResultRow(int filterRow) const;
    Q_INVOKABLE int filterRowForSource(int sourceRow) const;
    Q_INVOKABLE int nextReviewRow(int sourceRow) const { return model_.nextReviewRow(sourceRow); }
    Q_INVOKABLE int previousReviewRow(int sourceRow) const { return model_.previousReviewRow(sourceRow); }
    Q_INVOKABLE void audition(int row);
    Q_INVOKABLE void playTimeline();
    Q_INVOKABLE void toggleTimelinePlay();
    Q_INVOKABLE void setContinuousAudition(bool enabled);
    Q_INVOKABLE void stopTimeline();
    Q_INVOKABLE void setDecision(int row, const QString &decision);
    Q_INVOKABLE void restoreAutoDecision(int row);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void exportXml(const QUrl &url);
    Q_INVOKABLE void exportWav(const QUrl &url);

signals:
    void mediaChanged();
    void scriptChanged();
    void projectChanged();
    void statusChanged();
    void busyChanged();
    void progressChanged();
    void playbackChanged();
    void historyChanged();
    void positionChanged();
    void resultsChanged();
    void canAiReviewChanged();
    void canExportChanged();
    void statusFilterChanged();
    void modifiedChanged();
    void canSaveChanged();

private:
    struct Edit final {
        int row = -1;
        std::optional<RoughCutDecision> before;
        std::optional<RoughCutDecision> after;
        QVector<RoughCutSegmentDecision> beforeBase;
        QVector<RoughCutSegmentDecision> afterBase;
    };

    void stopWorker();
    void stopWaveformWorker();
    void startWaveformWorker();
    void syncSceneItems();
    void rebuildTimeline();
    void applyEdit(const Edit &edit, bool forward);
    void startTimelineClip(int index);
    void setStatus(QString value);
    void setBusy(bool value);
    void beginTask(const QString &title, bool indeterminate);
    void bindSharedAudioDevice();
    void refreshModified();
    void markSaved();
    [[nodiscard]] QByteArray projectFingerprint() const;
    void finishJobWithoutResults(const QString &message);

    ApplicationContext *context_ = nullptr;
    RoughCutResultModel model_;
    RoughCutResultFilterModel filterModel_;
    PlaybackEngine playback_;
    QTimer playbackTimer_;
    QString mediaPath_;
    QString scriptPath_;
    QString scriptText_;
    QString projectPath_;
    QString statusText_ = QStringLiteral("请选择完整 WAV。");
    int sampleRate_ = 0;
    int channels_ = 0;
    qint64 sourceSampleCount_ = 0;
    qint64 auditionEndSample_ = -1;
    int timelinePlaybackIndex_ = -1;
    qint64 timelineGapDeadlineMs_ = -1;
    bool timelinePaused_ = false;
    bool continuousAudition_ = true;
    QElapsedTimer timelinePlaybackClock_;
    QVector<RoughCutTimelineClip> timeline_;
    QVector<Edit> history_;
    int historyIndex_ = 0;
    int analysisVersion_ = 0;
    QVector<TranscriptWord> analysisWords_;
    QVector<RoughCutAuxiliaryResult> auxiliaryResults_;
    bool busy_ = false;
    bool cancelling_ = false;
    bool progressIndeterminate_ = false;
    int progressPercent_ = 0;
    QString busyTaskTitle_;
    double playbackRate_ = 1.0;
    std::atomic<bool> cancel_{false};
    std::atomic<quint64> workerGeneration_{0};
    std::thread worker_;
    std::atomic<bool> waveformCancel_{false};
    std::atomic<quint64> waveformGeneration_{0};
    std::thread waveformWorker_;
    QPointer<TimelineSceneItem> sourceWaveformItem_;
    QPointer<TimelineSceneItem> timelineItem_;
    IAsrService *asrOverride_ = nullptr;
    QByteArray savedFingerprint_;
    bool modified_ = false;
    bool ownsSharedAudio_ = false;
    bool shuttingDown_ = false;
};

} // namespace subcue
