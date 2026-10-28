#include "project/project_manager.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>

namespace subcue {

std::optional<ProjectStorageAdapter::Snapshot> ProjectManager::convert(
    const QString &source, ProjectStorageAdapter::Format format, const QString &destination,
    QString *error, const std::atomic<bool> *cancel)
{
    if (cancel && cancel->load()) {
        if (error) *error = QStringLiteral("工程转换已取消。");
        return {};
    }
    if (QFileInfo::exists(destination) || QFileInfo::exists(destination + QStringLiteral(".assets"))) {
        if (error) *error = QStringLiteral("转换目标已存在，未覆盖。");
        return {};
    }
    if (format == ProjectStorageAdapter::Format::UnifiedSqlite) {
        if (error) *error = QStringLiteral("统一工程请使用另存或恢复，不重复转换。");
        return {};
    }
    const auto legacy = ProjectStorageAdapter(source, format).load(error);
    if (!legacy) return {};
    QTemporaryDir staging(QFileInfo(destination).absoluteDir().filePath(QStringLiteral(".subcue-convert-XXXXXX")));
    if (!staging.isValid()) { if (error) *error = QStringLiteral("无法创建转换临时目录。"); return {}; }
    ProjectStore store(staging.filePath(QStringLiteral("project.sqlite")));
    if (!store.create(error)) return {};
    auto project = store.load(error);
    if (!project) return {};
    const auto module = ProjectStorageAdapter::captureModule(store, legacy->data, project->projectId, error, cancel);
    if (!module) return {};
    project->modules.insert(format == ProjectStorageAdapter::Format::SubtitleJson
        ? QStringLiteral("subtitle") : QStringLiteral("roughcut"), *module);
    project->modules.insert(QStringLiteral("provenance"), QJsonObject{
        {QStringLiteral("sourceProjectId"), legacy->projectId},
        {QStringLiteral("sourceFormat"), format == ProjectStorageAdapter::Format::SubtitleJson
            ? QStringLiteral("subtitle-json") : QStringLiteral("roughcut-sqlite")},
        {QStringLiteral("sourceFingerprint"), QString::fromLatin1(legacy->storageVersion.toHex())}});
    if (!store.save(*project, error, nullptr, cancel)) return {};
    if (RoughCutProjectSerializer::mediaSha256(source, error, cancel) != legacy->storageVersion) {
        if (error) *error = QStringLiteral("转换期间原工程被修改或操作被取消，未发布转换工程。");
        return {};
    }
    if (!store.backup(destination, error, ProjectStore::BackupMode::ExternalMediaReferences, cancel)) return {};
    return open(destination, ProjectStorageAdapter::Format::UnifiedSqlite, error);
}

std::optional<ProjectStorageAdapter::Snapshot> ProjectManager::restore(
    const QString &source, const QString &destination, QString *error,
    ProjectStore::BackupMode mode, const std::atomic<bool> *cancel)
{
    if (!ProjectStore(source).backup(destination, error, mode, cancel)) return {};
    return open(destination, ProjectStorageAdapter::Format::UnifiedSqlite, error);
}

std::optional<ProjectStorageAdapter::Snapshot> ProjectManager::create(const QString &path, QString *error)
{
    if (!ProjectStore(path).create(error)) return {};
    return open(path, ProjectStorageAdapter::Format::UnifiedSqlite, error);
}

std::optional<ProjectStorageAdapter::Snapshot> ProjectManager::open(
    const QString &path, ProjectStorageAdapter::Format format, QString *error)
{
    return open(path, format, error, {});
}

std::optional<ProjectStorageAdapter::Snapshot> ProjectManager::open(
    const QString &path, ProjectStorageAdapter::Format format, QString *error, const QByteArray &expectedVersion,
    bool recoverTasks, int *recoveredTasks)
{
    auto snapshot = ProjectStorageAdapter(path, format).load(error);
    if (!snapshot) return {};
    const QString resolved = QFileInfo(path).canonicalFilePath();
    if (resolved.isEmpty()) {
        if (error) *error = QStringLiteral("工程文件在打开期间被移除，当前会话未切换。");
        return {};
    }
    std::unique_ptr<QLockFile> lock;
    if (resolved != path_ || !sessionLock_) {
        lock = std::make_unique<QLockFile>(resolved + QStringLiteral(".session.lock"));
        // 工程会话可能持续数小时，不能仅因锁文件较旧就夺取活跃会话。
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            if (error) *error = QStringLiteral("工程已在另一个会话中打开，请先关闭该会话。");
            return {};
        }
        snapshot = ProjectStorageAdapter(resolved, format).load(error);
        if (!snapshot) return {};
    }
    if (!expectedVersion.isEmpty() && snapshot->storageVersion != expectedVersion) {
        if (error) *error = QStringLiteral("工程在准备期间发生变化，当前会话未切换。");
        return {};
    }
    int recovered = 0;
    if (recoverTasks && format == ProjectStorageAdapter::Format::UnifiedSqlite) {
        const auto count = ProjectStore(resolved).recoverJobs(error);
        if (!count) return {};
        recovered = *count;
    }
    if (lock) sessionLock_ = std::move(lock);
    path_ = resolved;
    format_ = format;
    projectId_ = snapshot->projectId;
    ++generation_;
    snapshot->sessionGeneration = generation_;
    if (recoveredTasks) *recoveredTasks = recovered;
    return snapshot;
}

bool ProjectManager::save(const ProjectStorageAdapter::Snapshot &snapshot,
                          QString *error, const std::atomic<bool> *cancel,
                          ProjectStorageAdapter::SaveResult *result)
{
    if (path_.isEmpty() || snapshot.projectId != projectId_
        || snapshot.sessionGeneration != generation_) {
        if (error) *error = QStringLiteral("保存请求不属于当前工程。");
        return false;
    }
    return ProjectStorageAdapter(path_, format_).save(snapshot, error, cancel, result);
}

void ProjectManager::close()
{
    sessionLock_.reset();
    path_.clear();
    projectId_.clear();
    ++generation_;
}

} // namespace subcue
