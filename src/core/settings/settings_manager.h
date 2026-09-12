#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace subcue {

class SettingsManager final {
public:
    explicit SettingsManager(QString path = {});

    [[nodiscard]] static QJsonObject defaults();
    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] QJsonObject load() const;
    [[nodiscard]] bool save(const QJsonObject &settings, QString *errorMessage = nullptr) const;
    [[nodiscard]] const QString &path() const noexcept;

private:
    QString path_;
};

} // namespace subcue
