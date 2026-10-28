#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <atomic>
#include <optional>

namespace subcue {
class ProjectStore;

// 全局档案可重命名、停用，声音版本只新增；项目复制版本，不持有跨库外键。
class VoiceLibrary final {
public:
    explicit VoiceLibrary(QString path);
    bool initialize(QString *error = nullptr);
    static bool validEmotionLabel(const QString &label);
    static bool validEmotionPrediction(const QJsonObject &prediction);
    std::optional<QString> createProfile(const QString &name, QString *error = nullptr);
    std::optional<QString> createWithVersions(const QString &name, const QJsonArray &entries,
        QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
    bool renameProfile(const QString &id, const QString &name, QString *error = nullptr);
    bool setProfileArchived(const QString &id, bool archived, QString *error = nullptr);
    bool setDefaultVersion(const QString &profileId, const QString &versionId, QString *error = nullptr);
    QString defaultVersion(const QString &profileId, QString *error = nullptr) const;
    QJsonArray profiles(bool includeArchived = false, int offset = 0, int limit = 50, QString *error = nullptr) const;
    std::optional<QString> addVersion(const QString &profileId, const QJsonObject &metadata,
        const QString &referencePath = {}, QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
    std::optional<QJsonObject> version(const QString &id, QString *error = nullptr) const;
    QJsonArray versions(const QString &profileId, int offset = 0, int limit = 50, QString *error = nullptr) const;
    bool copyVersionToProject(const QString &id, ProjectStore &project, QString *error = nullptr,
        const std::atomic<bool> *cancel = nullptr) const;
private:
    QString path_;
};
}
