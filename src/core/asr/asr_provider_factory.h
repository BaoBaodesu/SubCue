#pragma once

#include "asr/asr_service.h"
#include "asr/http_client.h"
#include "asr/whisper_engine.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <memory>

namespace subcue {

class AsrProviderFactory final {
public:
    // 注入的 HTTP / Whisper 引擎必须在返回的 service 使用期间保持有效。
    // 未注入时各 Provider 自行创建 QtNetworkHttpClient / 已链接的 WhisperEngine。
    AsrProviderFactory(IHttpClient *http = nullptr, IWhisperEngine *whisperEngine = nullptr);

    // asrProvider 缺省或未知时使用 dashscope，不改写旧 asrModel。
    [[nodiscard]] std::unique_ptr<IAsrService> create(
        const QJsonObject &settings,
        const QString &apiKey) const;

    [[nodiscard]] static QString providerIdFromSettings(const QJsonObject &settings);
    [[nodiscard]] static QString defaultWhisperModelsDirectory();

private:
    IHttpClient *http_ = nullptr;
    IWhisperEngine *whisperEngine_ = nullptr;
};

} // namespace subcue
