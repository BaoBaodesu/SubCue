#pragma once

#include "project/project_serializer.h"
#include "project/project_store.h"
#include "roughcut/rough_cut_project.h"
#include <variant>

namespace subcue {

// 固定格式适配边界：旧工程只保存自己的业务数据。
class ProjectStorageAdapter final {
public:
    enum class Format { SubtitleJson, RoughCutSqlite, UnifiedSqlite };
    using Data = std::variant<Project, RoughCutProject, ProjectStore::Snapshot>;
    struct Snapshot {
        QString projectId;
        QByteArray storageVersion;
        Data data;
        quint64 sessionGeneration = 0;
    };
    struct SaveResult {
        // 旧格式没有数据库 revision，仍使用文件指纹检测外部修改。
        std::optional<qint64> revision;
        QByteArray storageVersion;
    };
    ProjectStorageAdapter(QString path, Format format);
    [[nodiscard]] std::optional<Snapshot> load(QString *error = nullptr) const;
    [[nodiscard]] bool save(const Snapshot &snapshot, QString *error = nullptr,
                            const std::atomic<bool> *cancel = nullptr,
                            SaveResult *result = nullptr) const;
    [[nodiscard]] bool supports(const QString &module) const;
    [[nodiscard]] static std::optional<QJsonObject> packModule(const Data &data,
        QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] static std::optional<Data> unpackModule(const QJsonObject &module,
        QString *error = nullptr);
    [[nodiscard]] std::optional<Data> loadModule(const QString &module, QString *error = nullptr) const;
    [[nodiscard]] static std::optional<QJsonObject> captureModule(ProjectStore &store, Data data,
        const QString &projectId, QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
private:
    QString path_;
    Format format_;
};

} // namespace subcue
