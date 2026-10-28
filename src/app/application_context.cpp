#include "application_context.h"

#include "playback/playback_engine.h"
#include "inference/inference_manager.h"

#include <utility>
#include <QtCore/QPromise>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <exception>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

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
    projectStorageTasks_.setMaxThreadCount(1);
    audioProcessingTasks_.setMaxThreadCount(2);
    (void)InferenceManager::instance();
    if (!settings.value(QStringLiteral("legacyCloudRetired")).toBool()) (void)credentials.migrateLegacyDashScope(&credentialMigrationError);
    cloudConfig = std::make_unique<CloudConfigStore>(
        QFileInfo(settingsManager.path()).absoluteDir().filePath(QStringLiteral("connections.sqlite")), &credentials);
    if (cloudConfig->initialize(settings, &cloudConfigError)) (void)cloudConfig->cleanup(&cloudConfigError);
}

ApplicationContext::~ApplicationContext()
{
    audioProcessingTasks_.waitForDone();
    projectStorageTasks_.waitForDone();
    unbindAudioClients();
    if (audioDevice) audioDevice->stop();
}

namespace {
QFuture<void> submitTask(QThreadPool &pool, std::function<void()> task)
{
    auto promise = std::make_shared<QPromise<void>>();
    auto future = promise->future();
    promise->start();
    pool.start([promise, task = std::move(task)] {
        try {
            task();
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    return future;
}
}
QFuture<void> ApplicationContext::submitProjectStorage(std::function<void()> task)
{
    return submitTask(projectStorageTasks_, std::move(task));
}
QFuture<void> ApplicationContext::submitAudioProcessing(std::function<void()> task)
{
    return submitTask(audioProcessingTasks_, std::move(task));
}

bool ApplicationContext::trackCloudTask(QObject *owner, const QString &accountId, std::function<void()> cancelAndWait)
{
    if (cloudMutation_ || !owner || accountId.isEmpty()) return false;
    clearCloudTask(owner);
    cloudTasks_.append({owner, accountId, std::move(cancelAndWait)});
    return true;
}
void ApplicationContext::clearCloudTask(QObject *owner)
{
    cloudTasks_.removeIf([owner](const CloudTaskOwner &task) { return !task.owner || task.owner == owner; });
}
bool ApplicationContext::otherCloudInstancesRunning()
{
#ifdef Q_OS_WIN
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool running = false;
    if (!Process32FirstW(snapshot, &entry)) running = true;
    else do {
        const auto name = QString::fromWCharArray(entry.szExeFile);
        if (entry.th32ProcessID != GetCurrentProcessId()
            && (name.compare(QStringLiteral("SubCue.exe"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("AutoSubtitleAligner.exe"), Qt::CaseInsensitive) == 0)) { running = true; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return running;
#else
    return false;
#endif
}
bool ApplicationContext::beginCloudMutation(const QStringList &accounts)
{
    if (cloudMutation_ || otherCloudInstancesRunning()) return false;
    cloudMutation_ = true;
    const auto tasks = cloudTasks_;
    for (const auto &task : tasks) if (task.owner && accounts.contains(task.accountId)) task.cancelAndWait();
    return true;
}
void ApplicationContext::endCloudMutation() { cloudMutation_ = false; }

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
