#pragma once
#include "tts/local_tts_provider.h"
#include "tts/cloud_tts_provider.h"
#include <variant>
#include <QtCore/QObject>
#include <QtCore/QFuture>
#include <QtCore/QThreadPool>
#include <QtCore/QVariantList>

namespace subcue {
class ApplicationContext;
class ProjectSession;
class TtsTaskController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QVariantList records READ records NOTIFY recordsChanged)
    Q_PROPERTY(bool recordsLoading READ recordsLoading NOTIFY recordsChanged)
    Q_PROPERTY(QString recordsError READ recordsError NOTIFY recordsChanged)
public:
    TtsTaskController(ApplicationContext *context, ProjectSession *session, QObject *parent = nullptr);
    ~TtsTaskController() override;
    QString segmentState(const QString &id) const { return segmentStates_.value(id).toString(); }
    bool busy() const { return busy_; }
    QString statusText() const { return status_; }
    QVariantList records() const { return records_; }
    bool recordsLoading() const { return recordsLoading_; }
    QString recordsError() const { return recordsError_; }
    Q_INVOKABLE void loadRecords(int offset = 0, int limit = 50, const QString &batchId = {});
    bool start(std::variant<LocalTtsProvider, CloudTtsProvider> provider, QVector<TtsJob> jobs);
    Q_INVOKABLE void cancel();
    void cancelAndWait();
signals:
    void recordsChanged();
    void busyChanged();
    void statusChanged();
    void candidateCommitted(const QString &jobId, const QString &takeId, const QString &path, bool mock);
    void finished(bool success, const QString &message);
private:
    ApplicationContext *context_;
    ProjectSession *session_;
    QThreadPool workers_;
    QFuture<void> task_;
    QFuture<void> recordsTask_;
    QVariantList records_;
    QString recordsError_;
    quint64 recordsEpoch_ = 0;
    bool recordsLoading_ = false;
    std::atomic<bool> cancel_{false};
    quint64 epoch_ = 0;
    bool busy_ = false;
    QString status_;
    QVariantMap segmentStates_;
};
} // namespace subcue
