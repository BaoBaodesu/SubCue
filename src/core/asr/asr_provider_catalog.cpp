#include "asr/asr_provider_catalog.h"

#include "asr/asr_types.h"

namespace subcue {

QVector<AsrProviderInfo> AsrProviderCatalog::providers()
{
    return {
        {QString::fromLatin1(kAsrProviderDashScope), QStringLiteral("云端语音识别"), false,
            ModelKind::FunAsrNano, QStringLiteral("fun-asr-flash-2026-06-15"),
            QStringLiteral("Fun-ASR Flash 2026-06-15")},
        {QStringLiteral("qwen3"), QStringLiteral("本地 Qwen3-ASR 0.6B"), true,
            ModelKind::Qwen3Asr, QStringLiteral("Qwen3-ASR-0.6B"),
            QStringLiteral("Qwen3-ASR-0.6B")},
        {QStringLiteral("funasr"), QStringLiteral("本地 Fun-ASR Nano"), true,
            ModelKind::FunAsrNano, QStringLiteral("Fun-ASR-Nano-2512"),
            QStringLiteral("Fun-ASR-Nano-2512")},
    };
}

AsrProviderInfo AsrProviderCatalog::byId(const QString &providerId)
{
    for (const AsrProviderInfo &info : providers()) {
        if (info.id == providerId) return info;
    }
    return providers().constFirst();
}

QString AsrProviderCatalog::displayName(const QString &providerId)
{
    if (providerId == QLatin1String("qwen3")) return QStringLiteral("Qwen3-ASR");
    if (providerId == QLatin1String("funasr")) return QStringLiteral("Fun-ASR");
    return byId(providerId).name;
}

} // namespace subcue
