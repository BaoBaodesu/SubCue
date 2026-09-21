#pragma once

#include "ai/ai_review_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <optional>
#include <variant>

namespace subcue {

class OmniReviewJson final {
public:
    [[nodiscard]] static std::optional<QJsonObject> objectFromSseOrJson(const QByteArray &body);
    [[nodiscard]] static std::optional<QJsonObject> structuredObject(const QJsonObject &message);
    [[nodiscard]] static OmniUsage usageFromObject(const QJsonObject &usage);
    [[nodiscard]] static SubtitleOmniResult parseSubtitleSuggestions(const QJsonObject &root);
    [[nodiscard]] static RoughCutOmniResult parseRoughCutSuggestions(const QJsonObject &root);
    [[nodiscard]] static std::variant<QVector<QJsonObject>, AppError> parseWordMappings(const QJsonObject &root);
    [[nodiscard]] static AppError invalidResponse(QString details = {});
};

} // namespace subcue
