#include "asr/asr_provider_factory.h"

#include "asr/asr_provider_catalog.h"
#include "asr/dashscope_asr_service.h"
#include "asr/local_python_asr_service.h"
#include "settings/model_locator.h"
#include "settings/settings_manager.h"

#include <QtCore/QFileInfo>
namespace subcue {

AsrProviderFactory::AsrProviderFactory(IHttpClient *http)
    : http_(http)
{
}

QString AsrProviderFactory::providerIdFromSettings(const QJsonObject &settings)
{
    const QString provider = settings.value(QStringLiteral("asrProvider")).toString();
    if (provider == QLatin1String("qwen3") || provider == QLatin1String("funasr")) return provider;
    return QString::fromLatin1(kAsrProviderDashScope);
}

std::unique_ptr<IAsrService> AsrProviderFactory::create(
    const QJsonObject &settings,
    const QString &apiKey) const
{
    const QString provider = providerIdFromSettings(settings);
    if (provider == QLatin1String("qwen3") || provider == QLatin1String("funasr")) {
        const AsrProviderInfo info = AsrProviderCatalog::byId(provider);
        QString python = settings.value(QStringLiteral("localAsrPython")).toString();
        const QString projectPython = QString::fromUtf8(SUBCUE_PROJECT_INFERENCE_PYTHON);
        if ((python.isEmpty() || python == QLatin1String("python"))
            && QFileInfo::exists(projectPython)) {
            python = projectPython;
        }
        return std::make_unique<LocalPythonAsrService>(provider,
            ModelLocator::directoryFor(info.modelKind, settings),
            provider == QLatin1String("qwen3")
                ? ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, settings) : QString(),
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
