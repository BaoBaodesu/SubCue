#pragma once

#include "ai/ai_types.h"
#include "alignment/transcript.h"
#include "common/provider_types.h"
#include "subtitle/subtitle.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <atomic>

namespace subcue {

class IAiProvider {
public:
    virtual ~IAiProvider() = default;

    [[nodiscard]] virtual QString providerId() const = 0;
    [[nodiscard]] virtual ModelListResult listModels(
        const std::atomic<bool> *cancel = nullptr)
    {
        Q_UNUSED(cancel);
        return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidRequest),
            QStringLiteral("Provider 不支持模型发现"));
    }
    [[nodiscard]] virtual ProviderTestResult testConnection(
        const std::atomic<bool> *cancel = nullptr)
    {
        ModelListResult result = listModels(cancel);
        if (std::holds_alternative<AppError>(result)) {
            return std::get<AppError>(std::move(result));
        }
        return ConnectionTestResult{std::get<QVector<ModelDescriptor>>(std::move(result)),
                                    QDateTime::currentDateTimeUtc()};
    }

    // 只复核 needsAiReview 的字幕，就地改写 QVector。成功返回 API 调用次数。
    [[nodiscard]] virtual AiReviewResult review(
        QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words,
        const std::atomic<bool> *cancel = nullptr,
        const AiProgress &progress = {}) = 0;

    [[nodiscard]] virtual QVector<AppError> errors() const = 0;
};

} // namespace subcue
