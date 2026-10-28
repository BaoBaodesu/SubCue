#pragma once
#include "settings/cloud_config_store.h"
#include <QtCore/QObject>
#include <QtCore/QThreadPool>
#include <QtCore/QVariant>
#include <atomic>
#include <memory>

namespace subcue {
class ApplicationContext;
class CloudSettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    Q_PROPERTY(QVariantList bindings READ bindings NOTIFY changed)
    Q_PROPERTY(QVariantList discovered READ discovered NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantMap tests READ tests NOTIFY changed)
public:
    explicit CloudSettingsController(ApplicationContext *context, QObject *parent = nullptr);
    ~CloudSettingsController() override;
    QVariantList accounts() const;
    QVariantList models() const;
    QVariantList bindings() const;
    QVariantList discovered() const { return discovered_; }
    bool dirty() const;
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    QVariantMap tests() const { return tests_; }
    Q_INVOKABLE void begin();
    Q_INVOKABLE void discard();
    Q_INVOKABLE QString addAccount(const QString &vendor);
    Q_INVOKABLE void editAccount(const QString &id, const QVariantMap &fields);
    Q_INVOKABLE void moveAccount(const QString &id, int direction);
    Q_INVOKABLE void removeAccount(const QString &id);
    Q_INVOKABLE bool stageKey(const QString &id, const QString &key);
    Q_INVOKABLE void removeKey(const QString &id);
    Q_INVOKABLE void keepKey(const QString &id);
    Q_INVOKABLE QString addModel(const QString &accountId, const QString &remoteId, const QString &adapter);
    Q_INVOKABLE void editModel(const QString &id, const QVariantMap &fields);
    Q_INVOKABLE void removeModel(const QString &id);
    Q_INVOKABLE void bindModel(const QString &task, const QString &model);
    Q_INVOKABLE bool save();
    Q_INVOKABLE void discoverModels(const QString &accountId);
    Q_INVOKABLE void testConnection(const QString &accountId);
    Q_INVOKABLE void testCapability(const QString &modelId, const QString &voiceId = {});
    Q_INVOKABLE QVariantList taskChoices(const QString &task) const;
    Q_INVOKABLE QString taskStatus(const QString &task) const;
    Q_INVOKABLE bool switchTask(const QString &task, const QString &model);
    Q_INVOKABLE void cancelDiscovery();
signals:
    void changed();
private:
    void requestModels(const QString &accountId, bool discovery);
    void updateRow(const QString &table, const QString &id, const QVariantMap &fields, const QStringList &allowed);
    ApplicationContext *context_;
    QJsonObject draft_, original_;
    QMap<QString, CloudSecretChange> secrets_;
    QVariantList discovered_;
    QString status_;
    QVariantMap tests_;
    bool busy_ = false;
    quint64 generation_ = 0;
    QThreadPool requests_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};
} // namespace subcue
