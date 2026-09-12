#pragma once

#include "ai/ai_provider.h"
#include "asr/http_client.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>

namespace subcue {

class AiProviderFactory final {
public:
    // 注入的 HTTP 客户端必须在返回的 provider 使用期间保持有效。
    explicit AiProviderFactory(IHttpClient *http = nullptr);

    [[nodiscard]] std::unique_ptr<IAiProvider> create(
        const QJsonObject &settings,
        const QString &apiKey) const;

private:
    IHttpClient *http_ = nullptr;
};

} // namespace subcue
