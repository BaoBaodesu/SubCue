#pragma once

#include "settings/credential_store.h"

namespace subcue {

class DpapiCredentialStore final : public ICredentialStore {
public:
    explicit DpapiCredentialStore(QString path = {});

    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] QString load(const QString &credentialId, QString *errorMessage = nullptr) const override;
    [[nodiscard]] bool save(const QString &credentialId, const QString &secret,
                            QString *errorMessage = nullptr) const override;
    [[nodiscard]] bool remove(const QString &credentialId, QString *errorMessage = nullptr) const override;
    [[nodiscard]] bool exists(const QString &credentialId) const override;

    [[nodiscard]] QString load(QString *errorMessage = nullptr) const;
    [[nodiscard]] bool save(const QString &apiKey, QString *errorMessage = nullptr) const;
    [[nodiscard]] bool hasSavedKey() const;
    [[nodiscard]] QString getKey(const QString &userInput = {}) const;
    [[nodiscard]] bool migrateLegacyDashScope(QString *errorMessage = nullptr) const;

private:
    [[nodiscard]] QString targetName(const QString &credentialId) const;
    [[nodiscard]] QString loadLegacy(QString *errorMessage = nullptr) const;

    QString path_;
    bool productionTargets_ = false;
};

} // namespace subcue
