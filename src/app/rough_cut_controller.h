#pragma once

#include "playback/playback_engine.h"
#include "rough_cut_result_model.h"
#include "roughcut/timeline_engine.h"
#include "roughcut/auxiliary_recognition.h"

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
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString scriptPath READ scriptPath NOTIFY scriptChanged)
    Q_PROPERTY(QString scriptText READ scriptText WRITE setScriptText NOTIFY scriptChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged)
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

public:
    explicit RoughCutController(ApplicationContext *context, QObject *parent = nullptr);
    ~RoughCutController() override;

    [[nodiscard]] QObject *resultModel() noexcept { return &model_; }
    [[nodiscard]] QString mediaPath() const { return mediaPath_; }
    [[nodiscard]] QString scriptPath() const { return scriptPath_; }
    [[nodiscard]] QString scriptText() const { return scriptText_; }
    [[nodiscard]] QString projectPath() const { return projectPath_; }
    [[nodiscard]] QString statusText() const { return statusText_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] int progressPercent() const noexcept { return progressPercent_; }
    [[nodiscard]] bool playing() const noexcept { return !playback_.isPaused(); }
    [[nodiscard]] bool timelineActive() const noexcept { return timelinePlaybackIndex_ >= 0; }
    [[nodiscard]] bool timelinePaused() const noexcept { return timelinePaused_; }
    [[nodiscard]] bool continuousAudition() const noexcept { return continuousAudition_; }
    [[nodiscard]] double playbackRate() const noexcept { return playbackRate_; }
    [[nodiscard]] bool canUndo() const noexcept { return historyIndex_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return historyIndex_ < history_.size(); }
    [[nodiscard]] qint64 positionMs() const;
    [[nodiscard]] qint64 durationMs() const;
    [[nodiscard]] int resultCount() const { return model_.rowCount(); }

    Q_INVOKABLE void loadMedia(const QUrl &url);
    Q_INVOKABLE void loadScript(const QUrl &url);
    Q_INVOKABLE void setScriptText(const QString &text);
    Q_INVOKABLE void saveProject(const QUrl &url);
    Q_INVOKABLE void openProject(const QUrl &url);
    Q_INVOKABLE void setSourceWaveformItem(QObject *item);
    Q_INVOKABLE void setTimelineItem(QObject *item);
    Q_INVOKABLE void startAnalysis();
    Q_INVOKABLE void cancelAnalysis();
    Q_INVOKABLE void startAuxiliaryRecognition();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void seekTimeline(qint64 positionMs);
    Q_INVOKABLE void locateResult(int row);
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

private:
    struct Edit final {
        int row = -1;
        std::optional<RoughCutDecision> before;
        std::optional<RoughCutDecision> after;
    };

    void stopWorker();
    void stopWaveformWorker();
    void startWaveformWorker();
    void syncSceneItems();
    void rebuildTimeline();
    void applyEdit(const Edit &edit, bool forward);
    void startTimelineClip(int index);
    void setStatus(QString value);

    ApplicationContext *context_ = nullptr;
    RoughCutResultModel model_;
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
    QVector<RoughCutAuxiliaryResult> auxiliaryResults_;
    bool busy_ = false;
    int progressPercent_ = 0;
    double playbackRate_ = 1.0;
    std::atomic<bool> cancel_{false};
    std::atomic<quint64> workerGeneration_{0};
    std::thread worker_;
    std::atomic<bool> waveformCancel_{false};
    std::atomic<quint64> waveformGeneration_{0};
    std::thread waveformWorker_;
    QPointer<TimelineSceneItem> sourceWaveformItem_;
    QPointer<TimelineSceneItem> timelineItem_;
};

} // namespace subcue
