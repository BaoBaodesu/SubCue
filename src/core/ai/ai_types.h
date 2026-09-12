#pragma once

#include "common/app_error.h"

#include <QtCore/QString>
#include <QtCore/QStringView>

#include <atomic>
#include <functional>
#include <variant>

namespace subcue {

inline constexpr auto kAiProviderOpenAiCompatible = "openai-compatible";

inline constexpr int kAiConnectTimeoutMs = 15'000;
inline constexpr int kAiTransferTimeoutMs = 120'000;
inline constexpr int kAiNearbyLineBefore = 3;
inline constexpr int kAiNearbyLineAfter = 2;
inline constexpr int kAiWordWindowRadius = 25;
inline constexpr double kAiApplyConfidenceFloor = 0.70;

enum class AiErrorCode {
    Cancelled = 1,
    HttpFailure = 2,
    InvalidResponse = 3,
    InvalidRequest = 4
};

[[nodiscard]] inline bool aiCancelled(const std::atomic<bool> *cancel) noexcept
{
    return cancel && cancel->load(std::memory_order_acquire);
}

[[nodiscard]] inline AppError aiCancelledError()
{
    return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::Cancelled),
        QStringLiteral("任务已取消"));
}

[[nodiscard]] inline bool isAiCancelError(const AppError &error) noexcept
{
    return error.userMessage() == QStringLiteral("任务已取消")
        || (error.code() == static_cast<int>(AiErrorCode::Cancelled)
            && (error.domain() == ErrorDomain::Ai || error.domain() == ErrorDomain::Asr));
}

[[nodiscard]] inline QString aiEndpointForRegion(QStringView region)
{
    if (region == QLatin1String("singapore")) {
        return QStringLiteral(
            "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions");
    }
    return QStringLiteral(
        "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
}

using AiReviewResult = std::variant<int, AppError>;
using AiProgress = std::function<void(int current, int total)>;

} // namespace subcue
