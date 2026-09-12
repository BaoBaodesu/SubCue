#include "ai/ai_provider_factory.h"

#include "ai/openai_compatible_provider.h"
#include "settings/settings_manager.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUrl>

namespace subcue {

AiProviderFactory::AiProviderFactory(IHttpClient *http)
    : http_(http)
{
}

std::unique_ptr<IAiProvider> AiProviderFactory::create(
    const QJsonObject &settings,
    const QString &apiKey) const
{
    const QString selected = settings.value(QStringLiteral("aiProviderId")).toString();
    for (const QJsonValue &value : settings.value(QStringLiteral("aiProviders")).toArray()) {
        const QJsonObject provider = value.toObject();
        if (provider.value(QStringLiteral("id")).toString() != selected) {
            continue;
        }
        const QUrl baseUrl(provider.value(QStringLiteral("baseUrl")).toString());
        const bool qwen = provider.value(QStringLiteral("kind")).toString()
            == QLatin1String("qwen");
        return std::make_unique<OpenAICompatibleProvider>(
            apiKey,
            provider.value(QStringLiteral("selectedModel")).toString(),
            baseUrl,
            provider.value(QStringLiteral("authMode")).toString(QStringLiteral("bearer"))
                == QLatin1String("bearer"),
            qwen ? OpenAICompatibleProvider::qwenModelsEndpoint(baseUrl) : QUrl{},
            http_);
    }
    return nullptr;
}

} // namespace subcue
