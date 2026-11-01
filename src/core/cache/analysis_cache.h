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
    // 缓存按内容哈希累积，必须给上限；超过上限时按最近修改时间淘汰旧结果。
    static constexpr qint64 kDefaultLimitBytes = 512LL * 1024 * 1024;
    explicit AnalysisCache(QString directory = {}, qint64 limitBytes = kDefaultLimitBytes);

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
    qint64 limitBytes_ = kDefaultLimitBytes;
};

} // namespace subcue
