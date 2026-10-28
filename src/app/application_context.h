#pragma once

#include "playback/audio_device.h"
#include "settings/settings_manager.h"
#include "settings/cloud_config_store.h"
#include "platform/windows/dpapi_credential_store.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtCore/QFuture>
#include <QtCore/QThreadPool>
#include <QtCore/QPointer>
#include <QtCore/QStringList>

#include <memory>
#include <functional>

namespace subcue {

class IHttpClient;
class PlaybackEngine;
class ProjectSession;

class ApplicationContext final {
public:
    explicit ApplicationContext(
        QString settingsPath = {},
        QString credentialPath = {},
        AudioDeviceKind audioKind = AudioDeviceKind::Auto);
    ~ApplicationContext();

    void registerAudioClient(PlaybackEngine *engine);
    void unregisterAudioClient(PlaybackEngine *engine);
    void replaceAudioDevice(std::unique_ptr<IAudioDevice> device);
    void unbindAudioClients();
    // 串行执行磁盘任务；提交者负责取消标志，并在销毁前等待自己的 Future。
    [[nodiscard]] QFuture<void> submitProjectStorage(std::function<void()> task);
    [[nodiscard]] QFuture<void> submitAudioProcessing(std::function<void()> task);
    bool trackCloudTask(QObject *owner, const QString &accountId, std::function<void()> cancelAndWait);
    void clearCloudTask(QObject *owner);
    static bool otherCloudInstancesRunning();
    bool beginCloudMutation(const QStringList &accounts);
    void endCloudMutation();

    SettingsManager settingsManager;
    DpapiCredentialStore credentials;
    std::unique_ptr<IAudioDevice> audioDevice;
    QJsonObject settings;
    QString credentialMigrationError;
    std::unique_ptr<CloudConfigStore> cloudConfig;
    QString cloudConfigError;
    IHttpClient *aiHttp = nullptr;
    ProjectSession *projectSession = nullptr;

private:
    struct CloudTaskOwner { QPointer<QObject> owner; QString accountId; std::function<void()> cancelAndWait; };
    QVector<CloudTaskOwner> cloudTasks_;
    bool cloudMutation_ = false;
    QThreadPool projectStorageTasks_;
    QThreadPool audioProcessingTasks_;
    QVector<PlaybackEngine *> audioClients_;
};

} // namespace subcue
