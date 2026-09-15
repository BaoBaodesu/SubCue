#pragma once

#include "asr/asr_service.h"
#include "asr/http_client.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>

namespace subcue {

class AsrProviderFactory final {
public:
    // 注入的 HTTP 客户端必须在返回的 service 使用期间保持有效。
    explicit AsrProviderFactory(IHttpClient *http = nullptr);

    // asrProvider 缺省或未知时使用 dashscope，不改写旧 asrModel。
    [[nodiscard]] std::unique_ptr<IAsrService> create(
        const QJsonObject &settings,
        const QString &apiKey) const;

    [[nodiscard]] static QString providerIdFromSettings(const QJsonObject &settings);

private:
    IHttpClient *http_ = nullptr;
};

} // namespace subcue
