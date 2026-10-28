#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace subcue {

class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;
    [[nodiscard]] virtual QString load(
        const QString &credentialId,
        QString *errorMessage = nullptr) const = 0;
    [[nodiscard]] virtual bool save(
        const QString &credentialId,
        const QString &secret,
        QString *errorMessage = nullptr) const = 0;
    [[nodiscard]] virtual bool remove(
        const QString &credentialId,
        QString *errorMessage = nullptr) const = 0;
    [[nodiscard]] virtual bool exists(const QString &credentialId) const = 0;
    [[nodiscard]] virtual QStringList list(const QString &prefix, QString *errorMessage = nullptr) const
    {
        Q_UNUSED(prefix);
        if (errorMessage) *errorMessage = QStringLiteral("凭据库不支持枚举");
        return {};
    }
};

} // namespace subcue
