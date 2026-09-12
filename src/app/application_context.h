#pragma once

#include "playback/audio_device.h"
#include "settings/settings_manager.h"
#include "platform/windows/dpapi_credential_store.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>

namespace subcue {

class ApplicationContext final {
public:
    explicit ApplicationContext(
        QString settingsPath = {},
        QString credentialPath = {},
        AudioDeviceKind audioKind = AudioDeviceKind::Auto);

    SettingsManager settingsManager;
    DpapiCredentialStore credentials;
    std::unique_ptr<IAudioDevice> audioDevice;
    QJsonObject settings;
    QString credentialMigrationError;
};

} // namespace subcue
