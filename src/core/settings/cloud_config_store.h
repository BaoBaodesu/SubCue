#pragma once

#include "settings/credential_store.h"
#include <QtCore/QJsonObject>
#include <QtCore/QMap>

namespace subcue {

enum class CloudSecretAction { Keep, Replace, Remove };
struct CloudSecretChange final {
    CloudSecretAction action = CloudSecretAction::Keep;
    QString value;
};

// 应用级元数据与安全凭据的协调器；不向配置快照返回密钥。
class CloudConfigStore final {
public:
    CloudConfigStore(QString databasePath, ICredentialStore *credentials);
    [[nodiscard]] bool initialize(const QJsonObject &legacy, QString *error = nullptr);
    [[nodiscard]] QJsonObject load(QString *error = nullptr) const;
    [[nodiscard]] bool save(QJsonObject draft, qint64 expectedRevision,
        const QMap<QString, CloudSecretChange> &secrets, QString *error = nullptr);
    [[nodiscard]] QString resolveKey(const QString &accountId, QString *error = nullptr) const;
    [[nodiscard]] QJsonObject resolveTask(const QString &taskId, QString *key, QString *error = nullptr) const;
    [[nodiscard]] bool resolveAsr(QJsonObject *settings, QString *key, QString *error = nullptr) const;
    [[nodiscard]] QJsonObject resolveSynthesis(QString *key, QString *error = nullptr) const;
    [[nodiscard]] static bool adaptedModel(const QJsonObject &account, const QJsonObject &model);
    [[nodiscard]] bool modelVerified(const QString &modelId) const;
    [[nodiscard]] bool markModelVerified(const QString &modelId, qint64 expectedRevision, QString *error = nullptr);
    [[nodiscard]] bool cleanup(QString *error = nullptr) const;
private:
    QString path_;
    ICredentialStore *credentials_;
};

} // namespace subcue
