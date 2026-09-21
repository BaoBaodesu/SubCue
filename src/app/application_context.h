#pragma once

#include "playback/audio_device.h"
#include "settings/settings_manager.h"
#include "platform/windows/dpapi_credential_store.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <memory>

namespace subcue {

class IHttpClient;
class PlaybackEngine;

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

    SettingsManager settingsManager;
    DpapiCredentialStore credentials;
    std::unique_ptr<IAudioDevice> audioDevice;
    QJsonObject settings;
    QString credentialMigrationError;
    IHttpClient *aiHttp = nullptr;

private:
    QVector<PlaybackEngine *> audioClients_;
};

} // namespace subcue
