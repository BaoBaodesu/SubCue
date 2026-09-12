#pragma once

#include "asr/asr_service.h"
#include "asr/http_client.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <functional>
#include <memory>
#include <variant>

namespace subcue {

class DashScopeAsrService final : public IAsrService {
public:
    DashScopeAsrService(
        QString apiKey,
        QString model,
        QString region = QStringLiteral("beijing"),
        IHttpClient *http = nullptr);
    DashScopeAsrService(
        QString apiKey,
        QString model,
        QString region,
        QString apiHost,
        IHttpClient *http);

    [[nodiscard]] QString providerId() const override;
    [[nodiscard]] ProviderTestResult testConnection(
        const std::atomic<bool> *cancel = nullptr) override;
    [[nodiscard]] AsrResult transcribe(const AsrRequest &request) override;

    [[nodiscard]] AsrResult transcribePreparedChunks(
        const QVector<PreparedAudioChunk> &chunks,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(int, int)> &progress = {});

    [[nodiscard]] static QString endpointForRegion(QStringView region);
    [[nodiscard]] static QString endpointForApiHost(QString apiHost, QStringView region);
    [[nodiscard]] static QVector<QJsonObject> parseEventPayloads(
        const QByteArray &body,
        QByteArrayView contentType);
    [[nodiscard]] static std::variant<QVector<TranscriptWord>, AppError> wordsFromEvents(
        const QVector<QJsonObject> &events);
    [[nodiscard]] static bool isOverlapDuplicate(
        const TranscriptWord &incoming,
        const QVector<TranscriptWord> &existing);

    [[nodiscard]] const QString &endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] const QString &model() const noexcept { return model_; }

private:
    [[nodiscard]] AsrResult transcribeFlac(
        const QByteArray &flac,
        const std::atomic<bool> *cancel);
    [[nodiscard]] QByteArray buildPayload(const QByteArray &flac) const;

    QString apiKey_;
    QString model_;
    QString endpoint_;
    std::unique_ptr<IHttpClient> ownedHttp_;
    IHttpClient *http_ = nullptr;
};

} // namespace subcue
