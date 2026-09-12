#pragma once

#include "ai/ai_provider.h"
#include "asr/http_client.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>

#include <memory>
#include <optional>
#include <variant>

namespace subcue {

class OpenAICompatibleProvider final : public IAiProvider {
public:
    OpenAICompatibleProvider(
        QString apiKey,
        QString model,
        QString region = QStringLiteral("beijing"),
        IHttpClient *http = nullptr);
    OpenAICompatibleProvider(
        QString apiKey,
        QString model,
        QUrl baseUrl,
        bool useBearer,
        IHttpClient *http = nullptr);
    OpenAICompatibleProvider(
        QString apiKey,
        QString model,
        QUrl baseUrl,
        bool useBearer,
        QUrl modelsUrl,
        IHttpClient *http);

    [[nodiscard]] QString providerId() const override;
    [[nodiscard]] ModelListResult listModels(
        const std::atomic<bool> *cancel = nullptr) override;
    [[nodiscard]] ProviderTestResult testConnection(
        const std::atomic<bool> *cancel = nullptr) override;
    [[nodiscard]] AiReviewResult review(
        QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words,
        const std::atomic<bool> *cancel = nullptr,
        const AiProgress &progress = {}) override;
    [[nodiscard]] QVector<AppError> errors() const override;

    [[nodiscard]] const QString &endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] const QString &modelsEndpoint() const noexcept { return modelsEndpoint_; }
    [[nodiscard]] const QString &model() const noexcept { return model_; }

    [[nodiscard]] static QString systemPrompt();
    [[nodiscard]] static QUrl qwenModelsEndpoint(QUrl baseUrl);
    [[nodiscard]] static QJsonObject responseFormatSchema();
    [[nodiscard]] static QString buildUserPrompt(
        const QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words,
        int targetIndex);
    [[nodiscard]] static QByteArray buildPayload(
        const QString &model,
        const QString &userPrompt);
    [[nodiscard]] static std::variant<std::optional<QJsonObject>, AppError> mappingFromResponse(
        const QByteArray &body,
        int pythonIndex);
    [[nodiscard]] static bool applyIfValid(
        Subtitle &target,
        int targetIndex,
        const QJsonObject &mapping,
        const QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words);

private:
    [[nodiscard]] std::variant<std::optional<QJsonObject>, AppError> reviewOne(
        int targetIndex,
        const QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words,
        const std::atomic<bool> *cancel);

    QString apiKey_;
    QString model_;
    QString endpoint_;
    QString modelsEndpoint_;
    bool useBearer_ = true;
    std::unique_ptr<IHttpClient> ownedHttp_;
    IHttpClient *http_ = nullptr;
    QVector<AppError> errors_;
};

} // namespace subcue
