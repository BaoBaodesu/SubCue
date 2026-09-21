#include "application_context.h"

#include "playback/playback_engine.h"

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

ApplicationContext::~ApplicationContext()
{
    unbindAudioClients();
    if (audioDevice) audioDevice->stop();
}

void ApplicationContext::registerAudioClient(PlaybackEngine *engine)
{
    if (!engine || audioClients_.contains(engine)) return;
    audioClients_.append(engine);
}

void ApplicationContext::unregisterAudioClient(PlaybackEngine *engine)
{
    if (!engine) return;
    engine->setAudioDevice(nullptr, 0);
    audioClients_.removeAll(engine);
}

void ApplicationContext::replaceAudioDevice(std::unique_ptr<IAudioDevice> device)
{
    unbindAudioClients();
    if (audioDevice) audioDevice->stop();
    audioDevice = std::move(device);
}

void ApplicationContext::unbindAudioClients()
{
    // 先断开所有播放器的采样回调，再允许停止或替换共享音频设备。
    for (PlaybackEngine *engine : audioClients_) {
        if (engine) engine->setAudioDevice(nullptr, 0);
    }
}

} // namespace subcue
