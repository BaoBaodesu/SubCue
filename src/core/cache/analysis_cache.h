#pragma once

#include "common/app_error.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <atomic>
#include <optional>
#include <variant>

namespace subcue {

class AnalysisCache final {
public:
    explicit AnalysisCache(QString directory = {});

    [[nodiscard]] std::variant<QString, AppError> keyFor(
        const QString &mediaPath,
        const QString &modelId,
        const QString &language,
        const QJsonObject &parameters,
        const QString &algorithmVersion,
        const std::atomic<bool> *cancel = nullptr) const;
    [[nodiscard]] std::optional<QJsonObject> load(const QString &key) const;
    [[nodiscard]] bool store(const QString &key, const QJsonObject &result, AppError *error = nullptr) const;

private:
    QString directory_;
};

} // namespace subcue
