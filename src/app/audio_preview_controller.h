#pragma once
#include "playback/playback_engine.h"
#include <QtCore/QFuture>
#include <QtCore/QTimer>
namespace subcue {
class ApplicationContext;
class ProjectSession;
class AudioPreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY changed)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY changed)
    Q_PROPERTY(QString displaySource READ displaySource NOTIFY changed)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY changed)
public:
    AudioPreviewController(ApplicationContext *context, ProjectSession *session, QObject *parent = nullptr);
    ~AudioPreviewController() override;
    bool playing() const { return playing_; }
    bool loading() const { return loading_; }
    bool paused() const { return paused_; }
    QString statusText() const { return status_; }
    qint64 positionUs() const { return playing_ ? playback_.position().microseconds() : stoppedPositionUs_; }
    qint64 durationUs() const { return durationUs_; }
    double volume() const { return volume_; }
    bool muted() const { return muted_; }
    void setVolume(double value);
    void setMuted(bool value);
    QString displaySource() const { return replayPath_; }
    Q_INVOKABLE void replay() { if (!replayPath_.isEmpty()) play(replayPath_, replayHash_); }
    QString sourcePath() const { return sourcePath_; }
    Q_INVOKABLE void play(const QString &path, const QString &sha256, qint64 startUs = 0);
    Q_INVOKABLE void seek(qint64 positionUs);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
signals:
    void changed();
    void requested();
    void released();
private:
    void tick();
    ApplicationContext *context_;
    PlaybackEngine playback_;
    QTimer timer_;
    QFuture<void> task_;
    std::atomic<bool> cancel_{false};
    quint64 epoch_ = 0;
    bool playing_ = false;
    bool loading_ = false;
    bool paused_ = false;
    QString status_;
    QString sourcePath_;
    QString replayPath_, replayHash_;
    qint64 stoppedPositionUs_ = 0;
    qint64 durationUs_ = 0;
    double volume_ = 1.0;
    bool muted_ = false;
};
}
