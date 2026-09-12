#include "asr/http_client.h"

#include "asr/asr_types.h"

#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace subcue {
namespace {

void applyHeaders(QNetworkRequest &networkRequest, const HttpRequest &request, qint64 resumeFrom)
{
    for (const HttpHeader &header : request.headers) {
        networkRequest.setRawHeader(header.name, header.value);
    }
    if (resumeFrom > 0) {
        networkRequest.setRawHeader(
            QByteArrayLiteral("Range"),
            QByteArrayLiteral("bytes=") + QByteArray::number(resumeFrom) + QByteArrayLiteral("-"));
    }
    networkRequest.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    if (request.transferTimeoutMs > 0) {
        networkRequest.setTransferTimeout(request.transferTimeoutMs);
    }
}

AppError httpFailure(QString message, const QString &details = {})
{
    return AppError(ErrorDomain::Network, static_cast<int>(AsrErrorCode::HttpFailure),
        std::move(message), details);
}

bool waitForReply(
    QNetworkReply *reply,
    const std::atomic<bool> *cancel,
    int connectTimeoutMs)
{
    QEventLoop loop;
    QTimer connectTimer;
    connectTimer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &connectTimer, &QTimer::stop);
    if (connectTimeoutMs > 0) {
        QObject::connect(&connectTimer, &QTimer::timeout, reply, &QNetworkReply::abort);
        connectTimer.start(connectTimeoutMs);
    }

    QTimer cancelTimer;
    if (cancel) {
        cancelTimer.setInterval(50);
        QObject::connect(&cancelTimer, &QTimer::timeout, reply, [reply, cancel, &loop]() {
            if (asrCancelled(cancel)) {
                reply->abort();
                loop.quit();
            }
        });
        cancelTimer.start();
    }
    loop.exec();
    return !asrCancelled(cancel);
}

QNetworkReply *startReply(QNetworkAccessManager &manager, const HttpRequest &request, qint64 resumeFrom)
{
    QNetworkRequest networkRequest(request.url);
    applyHeaders(networkRequest, request, resumeFrom);
    const QByteArray method = request.method.toUpper();
    if (method == QByteArrayLiteral("POST")) {
        return manager.post(networkRequest, request.body);
    }
    if (method == QByteArrayLiteral("HEAD")) {
        return manager.head(networkRequest);
    }
    return manager.get(networkRequest);
}

} // namespace

std::variant<HttpResponse, AppError> QtNetworkHttpClient::send(
    const HttpRequest &request,
    const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    QNetworkAccessManager manager;
    QNetworkReply *reply = startReply(manager, request, 0);
    if (!waitForReply(reply, cancel, request.connectTimeoutMs)) {
        reply->deleteLater();
        return asrCancelledError();
    }
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
        return asrCancelledError();
    }

    HttpResponse response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    response.body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    reply->deleteLater();
    if (error != QNetworkReply::NoError && response.status < 400) {
        return httpFailure(QStringLiteral("网络请求失败"), errorString);
    }
    if (response.status <= 0) {
        return httpFailure(QStringLiteral("网络请求失败"), errorString);
    }
    return response;
}

std::variant<qint64, AppError> QtNetworkHttpClient::download(
    const HttpRequest &request,
    QFile *output,
    qint64 resumeFrom,
    const std::atomic<bool> *cancel,
    const std::function<void(qint64, qint64)> &progress)
{
    if (!output || !output->isWritable()) {
        return AppError(ErrorDomain::Network, static_cast<int>(AsrErrorCode::DownloadFailure),
            QStringLiteral("无法写入下载文件"));
    }
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }

    QNetworkAccessManager manager;
    QNetworkReply *reply = startReply(manager, request, resumeFrom);
    qint64 received = resumeFrom;
    qint64 total = -1;
    bool restarted = false;

    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, resumeFrom, &restarted, &received]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (resumeFrom > 0 && status == 200) {
            restarted = true;
            received = 0;
        }
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
        [&received, &total, resumeFrom, &restarted, &progress](qint64 current, qint64 expected) {
            const qint64 base = restarted ? 0 : resumeFrom;
            received = base + current;
            total = expected < 0 ? -1 : base + expected;
            if (progress) {
                progress(received, total);
            }
        });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, output, &restarted, resumeFrom]() {
        if (restarted && resumeFrom > 0 && output->pos() != 0) {
            output->seek(0);
            output->resize(0);
            restarted = false;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            output->write(chunk);
        }
    });

    if (!waitForReply(reply, cancel, request.connectTimeoutMs)) {
        reply->deleteLater();
        return asrCancelledError();
    }
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        const QByteArray trailing = reply->readAll();
        if (!trailing.isEmpty()) {
            output->write(trailing);
        }
        reply->deleteLater();
        return asrCancelledError();
    }

    const QByteArray trailing = reply->readAll();
    if (!trailing.isEmpty()) {
        output->write(trailing);
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    reply->deleteLater();
    if (error != QNetworkReply::NoError && status < 400) {
        return httpFailure(QStringLiteral("模型下载失败"), errorString);
    }
    if (status >= 400) {
        return AppError(ErrorDomain::Network, status,
            QStringLiteral("模型下载失败（HTTP %1）").arg(status));
    }
    output->flush();
    return output->size();
}

} // namespace subcue
