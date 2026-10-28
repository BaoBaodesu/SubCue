#pragma once

#include "project/project_storage_adapter.h"
#include <QtCore/QLockFile>
#include <memory>

namespace subcue {

// 生命周期门面只持有工程身份，不复制工作区的可编辑模型。
// 文件操作由调用方放入已有后台执行队列。
class ProjectManager final {
public:
    [[nodiscard]] std::optional<ProjectStorageAdapter::Snapshot> create(
        const QString &path, QString *error = nullptr);
    [[nodiscard]] std::optional<ProjectStorageAdapter::Snapshot> open(
        const QString &path, ProjectStorageAdapter::Format format, QString *error = nullptr);
    [[nodiscard]] std::optional<ProjectStorageAdapter::Snapshot> open(
        const QString &path, ProjectStorageAdapter::Format format, QString *error, const QByteArray &expectedVersion,
        bool recoverTasks = false, int *recoveredTasks = nullptr);
    [[nodiscard]] std::optional<ProjectStorageAdapter::Snapshot> restore(
        const QString &source, const QString &destination, QString *error = nullptr,
        ProjectStore::BackupMode mode = ProjectStore::BackupMode::ExternalMediaReferences,
        const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] std::optional<ProjectStorageAdapter::Snapshot> convert(
        const QString &source, ProjectStorageAdapter::Format format, const QString &destination,
        QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] bool save(const ProjectStorageAdapter::Snapshot &snapshot,
                            QString *error = nullptr, const std::atomic<bool> *cancel = nullptr,
                            ProjectStorageAdapter::SaveResult *result = nullptr);
    void close();
    [[nodiscard]] const QString &projectId() const { return projectId_; }
    [[nodiscard]] const QString &path() const { return path_; }
    [[nodiscard]] quint64 generation() const { return generation_; }
private:
    std::unique_ptr<QLockFile> sessionLock_;
    QString path_;
    QString projectId_;
    ProjectStorageAdapter::Format format_ = ProjectStorageAdapter::Format::SubtitleJson;
    quint64 generation_ = 0;
};

} // namespace subcue
