#pragma once

#include "project/project_manager.h"
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QFuture>
#include <QtCore/QUrl>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QLockFile>
#include <atomic>
#include <memory>

namespace subcue {
class ApplicationContext;
class AppController;
class RoughCutController;
class VoiceStudioController;
class TtsTaskController;
class VoiceLibraryController;
class VoiceAudioController;

// UI 只持有最后提交的检查点；可编辑数据仍由各工作区模型拥有。
class ProjectSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool unified READ unified NOTIFY changed)
    Q_PROPERTY(QString legacyModule READ legacyModule NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString projectId READ projectId NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool savePending READ savePending NOTIFY busyChanged)
    Q_PROPERTY(QVariantMap recoveryDrafts READ recoveryDrafts NOTIFY recoveryRestored)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
public:
    ProjectSession(ApplicationContext *context, AppController *editor,
                   RoughCutController *roughCut, QObject *parent = nullptr);
    ~ProjectSession() override;
    bool active() const { return active_; }
    bool unified() const { return active_ && std::holds_alternative<ProjectStore::Snapshot>(checkpoint_.data); }
    QString legacyModule() const { return unified() || !active_ ? QString() :
        (std::holds_alternative<Project>(checkpoint_.data) ? QStringLiteral("subtitle") : QStringLiteral("roughcut")); }
    QString path() const { return path_; }
    QString storagePath() const { return !path_.isEmpty() ? path_ : draftStorage_ ? draftStorage_->filePath(QStringLiteral("draft.subcue-project")) : recoveredStoragePath_; }
    QString projectId() const { return checkpoint_.projectId; }
    bool busy() const { return busy_; }
    bool savePending() const { return !pendingSave_.isEmpty(); }
    bool assetMutationBlocked() const { return busy_ || savePending(); }
    bool storageTasksRunning() const;
    QVariantMap recoveryDrafts() const { return recoveryDrafts_.toVariantMap(); }
    Q_INVOKABLE void setRecoveryDrafts(const QVariantMap &drafts);
    Q_INVOKABLE QVariantList recoverableProjects() const;
    Q_INVOKABLE bool recoverProject(const QString &id);
    Q_INVOKABLE bool discardRecovery(const QString &id);
    void setVoiceServices(TtsTaskController *tasks, VoiceLibraryController *library, VoiceAudioController *audio);
    Q_INVOKABLE void cancelPendingSave();
    bool changingProject() const { return changingProject_; }
    quint64 generation() const { return checkpoint_.sessionGeneration; }
    bool modified() const;
    QString statusText() const { return status_; }
    void setVoiceController(VoiceStudioController *controller);
    QJsonObject voiceSnapshot() const;
    QJsonArray subtitleReferences() const;
    QJsonArray roughCutReferences() const;
    QStringList inputPaths() const;
    static bool isUnifiedUrl(const QUrl &url);
    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool openProject(const QUrl &url, const QString &requestingModule = {});
    Q_INVOKABLE bool saveProject(const QUrl &url, const QString &requestingModule = {});
    Q_INVOKABLE bool saveCurrentProject();
    Q_INVOKABLE bool convertProject(const QString &sourcePath, const QString &module, const QUrl &destination);
    Q_INVOKABLE bool backupProject(const QUrl &destination, bool portable = false);
    Q_INVOKABLE bool restoreProject(const QUrl &source, const QUrl &destination);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void shutdown(bool cleanExit = false);
signals:
    void preparingSave();
    void recoveryRestored();
    void aboutToChange();
    void changed();
    void busyChanged();
    void modifiedChanged();
    void statusChanged();
    void opened(bool success, const QString &path, const QString &message);
    void saved(bool success, const QString &path, const QString &message);
    void backedUp(bool success, const QString &path, const QString &message);
private:
    bool ready() const;
    QString recoveryRoot() const;
    void scheduleRecovery();
    void persistRecovery();
    bool saveLegacyProject(const QString &path, const QString &requestingModule);
    void setBusy(bool busy, const QString &status = {});
    bool openPrepared(const QString &path, const QString &requestingModule,
                      const QString &conversionModule = {}, const QString &source = {}, bool restoring = false);
    ApplicationContext *context_;
    QPointer<AppController> editor_;
    QPointer<RoughCutController> roughCut_;
    QPointer<VoiceStudioController> voice_;
    QPointer<TtsTaskController> tts_;
    QPointer<VoiceLibraryController> library_;
    QPointer<VoiceAudioController> audio_;
    QTimer saveWaitTimer_;
    QUrl pendingSave_;
    QString pendingSaveModule_;
    QTimer recoveryIdleTimer_, recoveryDeadlineTimer_;
    QFuture<void> recoveryTask_;
    QJsonObject recoveryDrafts_, recoveryOverlay_;
    QString recoveredStoragePath_;
    ProjectManager manager_; // 仅由串行存储任务使用；退出时先等待任务。
    ProjectStorageAdapter::Snapshot checkpoint_;
    QFuture<void> task_;
    std::atomic<bool> cancel_{false};
    bool active_ = false;
    bool busy_ = false;
    bool shuttingDown_ = false;
    bool changingProject_ = false;
    QString path_;
    QString status_;
    std::shared_ptr<QTemporaryDir> draftStorage_;
    std::unique_ptr<QLockFile> draftLock_;
};
} // namespace subcue
