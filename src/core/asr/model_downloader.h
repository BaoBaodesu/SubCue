#pragma once

#include "asr/http_client.h"
#include "asr/whisper_model_catalog.h"
#include "common/app_error.h"

#include <QtCore/QString>

#include <atomic>
#include <functional>
#include <variant>

namespace subcue {

class ModelDownloader final {
public:
    explicit ModelDownloader(IHttpClient *http);

    // 已存在且大小/SHA-256 匹配则直接返回路径。
    // 否则写入 destination.part，支持 Range 断点续传，校验后原子改名为正式文件。
    [[nodiscard]] std::variant<QString, AppError> ensure(
        const WhisperModelSpec &spec,
        const QString &directory,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(qint64 received, qint64 total)> &progress = {}) const;

    [[nodiscard]] static QByteArray sha256HexOfFile(const QString &path, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] static bool matchesSpec(const QString &path, const WhisperModelSpec &spec, const std::atomic<bool> *cancel = nullptr);

private:
    IHttpClient *http_ = nullptr;
};

} // namespace subcue
