#pragma once

#include "ai/ai_review_types.h"
#include "asr/http_client.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <memory>
#include <atomic>

namespace subcue {

class QwenOmniProvider final {
public:
    QwenOmniProvider(
        QString apiKey,
        OmniReviewSettings settings,
        IHttpClient *http = nullptr);

    [[nodiscard]] QString providerId() const;
    [[nodiscard]] const QString &model() const noexcept { return settings_.model; }
    [[nodiscard]] const QString &endpoint() const noexcept { return endpoint_; }

    [[nodiscard]] OmniCompleteResult complete(
        const OmniChatRequest &request,
        const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] OmniTestResult testConnection(const std::atomic<bool> *cancel = nullptr);

    [[nodiscard]] static QString normalizeBaseUrl(QString url);
    [[nodiscard]] static QUrl chatCompletionsUrl(const QString &baseUrl);
    [[nodiscard]] static QByteArray buildPayload(
        const OmniReviewSettings &settings,
        const OmniChatRequest &request);
    [[nodiscard]] static QJsonArray subtitleToolSchema();
    [[nodiscard]] static QJsonArray roughCutToolSchema();
    [[nodiscard]] static QJsonArray wordMappingToolSchema();
    [[nodiscard]] static QJsonArray connectionTestToolSchema();
    [[nodiscard]] static OmniAudioClip builtInProbeClip();
    [[nodiscard]] static AppError mapHttpError(int status, const QByteArray &body = {});

private:
    QString apiKey_;
    OmniReviewSettings settings_;
    QString endpoint_;
    std::unique_ptr<IHttpClient> ownedHttp_;
    IHttpClient *http_ = nullptr;
};

} // namespace subcue
