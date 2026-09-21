#pragma once

#include "ai/ai_review_types.h"
#include "ai/qwen_omni_provider.h"
#include "asr/http_client.h"
#include "subtitle/subtitle.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace subcue {

using OmniReviewProgress = std::function<void(int completed, int total, const QString &message)>;

class AiReviewService final {
public:
    AiReviewService(
        OmniReviewSettings settings,
        QString apiKey,
        IHttpClient *http = nullptr);

    [[nodiscard]] const OmniReviewSettings &settings() const noexcept { return settings_; }
    [[nodiscard]] QwenOmniProvider &provider() noexcept { return provider_; }
    [[nodiscard]] const OmniUsage &accumulatedUsage() const noexcept { return usage_; }
    [[nodiscard]] QString lastModel() const;

    [[nodiscard]] SubtitleOmniResult reviewSubtitles(
        const SubtitleOmniRequest &request,
        const std::atomic<bool> *cancel = nullptr,
        OmniReviewProgress progress = {});
    [[nodiscard]] RoughCutOmniResult reviewCandidates(
        const RoughCutOmniRequest &request,
        const std::atomic<bool> *cancel = nullptr,
        OmniReviewProgress progress = {});
    [[nodiscard]] WordMappingOmniResult reviewWordMapping(
        const WordMappingOmniRequest &request,
        const std::atomic<bool> *cancel = nullptr,
        OmniReviewProgress progress = {});

    [[nodiscard]] static QVector<QPair<qint64, qint64>> subtitleWindows(
        qint64 durationMs,
        const OmniReviewSettings &settings);
    [[nodiscard]] static void applySubtitleSuggestion(Subtitle *subtitle, const SubtitleOmniSuggestion &suggestion);
    [[nodiscard]] static RoughCutDecision gatedCutDecision(
        const RoughCutOmniSuggestion &suggestion,
        const OmniReviewSettings &settings);
    [[nodiscard]] static QString subtitleSystemPrompt();
    [[nodiscard]] static QString roughCutSystemPrompt();
    [[nodiscard]] static QString wordMappingSystemPrompt();

private:
    [[nodiscard]] OmniCompleteResult completeWithRetry(
        OmniChatRequest request,
        const std::atomic<bool> *cancel);
    void recordChatResponse(const OmniChatResponse &response);

    OmniReviewSettings settings_;
    QwenOmniProvider provider_;
    OmniUsage usage_;
    QString lastModel_;
};

} // namespace subcue
