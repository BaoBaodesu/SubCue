#include "asr/asr_provider_factory.h"

#include "asr/dashscope_asr_service.h"
#include "asr/whisper_cpp_service.h"
#include "settings/settings_manager.h"

#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>

namespace subcue {

AsrProviderFactory::AsrProviderFactory(IHttpClient *http, IWhisperEngine *whisperEngine)
    : http_(http),
      whisperEngine_(whisperEngine)
{
}

QString AsrProviderFactory::providerIdFromSettings(const QJsonObject &settings)
{
    const QString provider = settings.value(QStringLiteral("asrProvider")).toString();
    if (provider == QLatin1String(kAsrProviderWhisper)) {
        return QString::fromLatin1(kAsrProviderWhisper);
    }
    return QString::fromLatin1(kAsrProviderDashScope);
}

QString AsrProviderFactory::defaultWhisperModelsDirectory()
{
    const QString appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    const QString base = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        : QDir(appData).filePath(QStringLiteral("SubCue"));
    return QDir(base).filePath(QStringLiteral("models"));
}

std::unique_ptr<IAsrService> AsrProviderFactory::create(
    const QJsonObject &settings,
    const QString &apiKey) const
{
    if (providerIdFromSettings(settings) == QLatin1String(kAsrProviderWhisper)) {
        QString directory = settings.value(QStringLiteral("whisperModelsDirectory")).toString();
        if (directory.isEmpty()) {
            directory = defaultWhisperModelsDirectory();
        }
        const QString modelId = settings.value(QStringLiteral("whisperModel")).toString(
            QStringLiteral("small"));
        const QString device = settings.value(QStringLiteral("whisperDevice")).toString(
            QStringLiteral("auto"));
        return std::make_unique<WhisperCppService>(
            modelId, directory, device, http_, whisperEngine_);
    }

    const QJsonObject defaults = SettingsManager::defaults();
    const QString model = settings.value(QStringLiteral("asrModel")).toString(
        defaults.value(QStringLiteral("asrModel")).toString());
    const QString region = settings.value(QStringLiteral("region")).toString(
        defaults.value(QStringLiteral("region")).toString());
    const QString apiHost = settings.value(QStringLiteral("asrApiHost")).toString();
    return std::make_unique<DashScopeAsrService>(apiKey, model, region, apiHost, http_);
}

} // namespace subcue
