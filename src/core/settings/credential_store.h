#pragma once

#include <QtCore/QString>

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
};

} // namespace subcue
