#pragma once
#include <QtCore/QObject>
#include <QtCore/QFuture>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtCore/QVariantList>
#include <atomic>
#include "waveform/waveform_pyramid.h"
namespace subcue {
class ApplicationContext;
class ProjectSession;
class VoiceStudioController;
class AudioPreviewController;
class VoiceAudioController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool preparing READ preparing NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(qint64 cacheBytes READ cacheBytes NOTIFY changed)
    Q_PROPERTY(qint64 waveformBytes READ waveformBytes NOTIFY changed)
public:
    VoiceAudioController(ApplicationContext *context, ProjectSession *session, VoiceStudioController *document, AudioPreviewController *preview, QObject *parent = nullptr);
    ~VoiceAudioController() override;
    bool preparing() const { return busy_ && preparing_; }
    bool busy() const { return busy_; }
    QString statusText() const { return status_; }
    int progress() const { return progress_; }
    QVariantList rows() const { return rows_; }
    qint64 cacheBytes() const { return cacheBytes_; }
    qint64 waveformBytes() const;
    Q_INVOKABLE QVariantList waveform(const QString &id) const;
    Q_INVOKABLE void prepare(bool single = false);
    Q_INVOKABLE void seek(const QString &segmentId, double fraction);
    Q_INVOKABLE void exportWav(const QUrl &file, bool normalize = false, bool sentences = false, bool allowStale = false);
    Q_INVOKABLE void cancel() { cancel_ = true; }
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool ownsPlayback() const;
    Q_INVOKABLE void setActive(bool active) { active_ = active; if (!active) stop(); }
signals:
    void changed();
    void staleExportRequested(const QUrl &file, bool normalize, bool sentences);
private:
    void run(const QUrl &file, bool normalize, bool sentences, bool allowStale, bool single);
    void cancelAndWait();
    ApplicationContext *context_;
    ProjectSession *session_;
    QPointer<VoiceStudioController> document_;
    QPointer<AudioPreviewController> preview_;
    QFuture<void> task_;
    std::atomic<bool> cancel_{false};
    quint64 epoch_ = 0;
    bool busy_ = false;
    bool preparing_ = false;
    bool active_ = false;
    int progress_ = 0;
    qint64 cacheBytes_ = 0;
    QString status_, preparedPath_, preparedHash_, segment_;
    QByteArray editKey_;
    QVariantList rows_;
    QHash<QString, QVector<WaveformPeak>> waveforms_;
};
}
