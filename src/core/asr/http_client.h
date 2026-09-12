#pragma once

#include "common/app_error.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QUrl>

#include <atomic>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

namespace subcue {

struct HttpHeader final {
    QByteArray name;
    QByteArray value;
};

struct HttpRequest final {
    QUrl url;
    QByteArray method = QByteArrayLiteral("GET");
    std::vector<HttpHeader> headers;
    QByteArray body;
    int connectTimeoutMs = 15'000;
    int transferTimeoutMs = 360'000;
};

struct HttpResponse final {
    int status = 0;
    QByteArray contentType;
    QByteArray body;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    [[nodiscard]] virtual std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel = nullptr) = 0;

    // 将响应体写入 output。resumeFrom > 0 时发送 Range: bytes={resumeFrom}-。
    // 成功时返回写入后的文件大小。
    [[nodiscard]] virtual std::variant<qint64, AppError> download(
        const HttpRequest &request,
        QFile *output,
        qint64 resumeFrom = 0,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(qint64 received, qint64 total)> &progress = {}) = 0;
};

class QtNetworkHttpClient final : public IHttpClient {
public:
    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel = nullptr) override;

    std::variant<qint64, AppError> download(
        const HttpRequest &request,
        QFile *output,
        qint64 resumeFrom = 0,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(qint64 received, qint64 total)> &progress = {}) override;
};

} // namespace subcue
