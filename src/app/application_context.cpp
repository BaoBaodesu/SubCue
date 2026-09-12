#include "application_context.h"

#include <utility>

namespace subcue {

ApplicationContext::ApplicationContext(
    QString settingsPath,
    QString credentialPath,
    AudioDeviceKind audioKind)
    : settingsManager(std::move(settingsPath)),
      credentials(std::move(credentialPath)),
      audioDevice(createAudioDevice(audioKind)),
      settings(settingsManager.load())
{
    (void)credentials.migrateLegacyDashScope(&credentialMigrationError);
}

} // namespace subcue
