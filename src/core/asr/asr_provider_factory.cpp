#include "asr/asr_provider_factory.h"

#include "asr/dashscope_asr_service.h"
#include "asr/local_python_asr_service.h"
#include "asr/whisper_cpp_service.h"
#include "settings/settings_manager.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
namespace subcue {

AsrProviderFactory::AsrProviderFactory(IHttpClient *http, IWhisperEngine *whisperEngine)
    : http_(http),
      whisperEngine_(whisperEngine)
{
}

QString AsrProviderFactory::providerIdFromSettings(const QJsonObject &settings)
{
    const QString provider = settings.value(QStringLiteral("asrProvider")).toString();
    if (provider == QLatin1String("whisper") || provider == QLatin1String("qwen3") || provider == QLatin1String("funasr")) return provider;
    return QString::fromLatin1(kAsrProviderDashScope);
}

QString AsrProviderFactory::defaultWhisperModelsDirectory()
{
    return QDir::cleanPath(QString::fromUtf8(SUBCUE_PROJECT_WHISPER_MODELS_DIR));
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
    const QString provider = providerIdFromSettings(settings);
    if (provider == QLatin1String("qwen3") || provider == QLatin1String("funasr")) {
        const QString key = provider == QLatin1String("qwen3") ? QStringLiteral("qwen3AsrModelsDirectory") : QStringLiteral("funAsrModelsDirectory");
        const QString fallback = QDir(QString::fromUtf8(SUBCUE_PROJECT_MODELS_DIR)).filePath(
            provider == QLatin1String("qwen3") ? QStringLiteral("qwen3-asr-0.6b") : QStringLiteral("fun-asr-nano-2512"));
        QString python = settings.value(QStringLiteral("localAsrPython")).toString();
        const QString projectPython = QString::fromUtf8(SUBCUE_PROJECT_INFERENCE_PYTHON);
        if ((python.isEmpty() || python == QLatin1String("python"))
            && QFileInfo::exists(projectPython)) {
            python = projectPython;
        }
        return std::make_unique<LocalPythonAsrService>(provider,
            settings.value(key).toString(fallback),
            provider == QLatin1String("qwen3")
                ? settings.value(QStringLiteral("qwen3ForcedAlignerModelsDirectory")).toString(
                    QDir(QString::fromUtf8(SUBCUE_PROJECT_MODELS_DIR)).filePath(
                        QStringLiteral("qwen3-forced-aligner-0.6b"))) : QString(),
            python);
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
