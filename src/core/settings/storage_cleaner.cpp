#include "settings/storage_cleaner.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QStandardPaths>

#include <utility>

namespace subcue {
namespace {

QString joinRoot(const QString &root, const QString &relative)
{
    return QDir::cleanPath(QDir(root).filePath(relative));
}

} // namespace

StorageCleaner::StorageCleaner(QString projectRoot, QString modelsRoot, QString keepBuildDirectory)
    : projectRoot_(normalize(projectRoot))
    , modelsRoot_(normalize(modelsRoot))
    , keepBuildDirectory_(normalize(keepBuildDirectory))
{
}

QString StorageCleaner::normalize(const QString &path) const
{
    if (path.isEmpty()) return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool StorageCleaner::under(const QString &path, const QString &root) const
{
    if (path.isEmpty() || root.isEmpty()) return false;
    return path == root || path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

QString StorageCleaner::keepBuildName() const
{
    return QFileInfo(keepBuildDirectory_).fileName();
}

bool StorageCleaner::developmentTreeVisible() const
{
    if (projectRoot_.isEmpty() || keepBuildDirectory_.isEmpty()) return false;
    return under(keepBuildDirectory_, joinRoot(projectRoot_, QStringLiteral("out/build")));
}

qint64 StorageCleaner::directoryBytes(const QString &path) const
{
    const QFileInfo info(path);
    if (!info.exists()) return 0;
    if (info.isFile()) return info.size();
    qint64 total = 0;
    QDirIterator iterator(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

bool StorageCleaner::isPathAllowed(const QString &path) const
{
    const QString canonical = normalize(path);
    if (canonical.isEmpty() || QFileInfo(canonical).isRelative()) return false;
    if (!modelsRoot_.isEmpty() && under(canonical, modelsRoot_)) return false;
    if (!projectRoot_.isEmpty()) {
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral("src")))) return false;
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral("tests/media")))) return false;
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral("tests/golden")))) return false;
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral(".git")))) return false;
    }
    if (!keepBuildDirectory_.isEmpty() && under(canonical, keepBuildDirectory_)) {
        const QString package = joinRoot(keepBuildDirectory_, QStringLiteral("package"));
        if (!under(canonical, package)) return false;
    }

    if (!projectRoot_.isEmpty() && under(canonical, projectRoot_)) {
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral("out")))) return true;
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral(".test_tmp")))) return true;
        if (under(canonical, joinRoot(projectRoot_, QStringLiteral(".test_appdata")))) return true;
        if (normalize(QFileInfo(canonical).absolutePath()) == projectRoot_) {
            const QString name = QFileInfo(canonical).fileName();
            if (name.endsWith(QLatin1String(".log"), Qt::CaseInsensitive)
                || name.endsWith(QLatin1String(".err"), Qt::CaseInsensitive)
                || name.compare(QLatin1String("$log"), Qt::CaseInsensitive) == 0
                || name.compare(QLatin1String("main.obj"), Qt::CaseInsensitive) == 0
                || name.compare(QLatin1String("dryrun.txt"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    const QString appData = normalize(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    const QString cache = normalize(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const QString genericCache = normalize(
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/SubCue"));
    const QString roaming = normalize(
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/SubCue"));
    const QString tempRoot = normalize(QDir::tempPath());

    if (under(canonical, cache) || under(canonical, appData) || under(canonical, genericCache)
        || under(canonical, roaming)) {
        return true;
    }
    if (under(canonical, tempRoot)) {
        const QString name = QFileInfo(canonical).fileName();
        return name.startsWith(QLatin1String("SubCue-"), Qt::CaseInsensitive)
            || under(canonical, joinRoot(tempRoot, QStringLiteral("SubCue")));
    }
    return false;
}

void StorageCleaner::appendIfExists(QVector<StorageTarget> *targets, StorageTarget target) const
{
    target.path = normalize(target.path);
    if (target.path.isEmpty() || !QFileInfo::exists(target.path)) return;
    if (!isPathAllowed(target.path)) return;
    target.bytes = directoryBytes(target.path);
    targets->append(std::move(target));
}

QVector<StorageTarget> StorageCleaner::scan() const
{
    QVector<StorageTarget> targets;
    appendIfExists(&targets, {QStringLiteral("cache-analysis"), QStringLiteral("cache"),
        QStringLiteral("分析缓存"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("analysis"))});
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    appendIfExists(&targets, {QStringLiteral("cache-logs"), QStringLiteral("cache"),
        QStringLiteral("应用日志"), QDir(appData).filePath(QStringLiteral("SubCue/logs"))});

    QDir temp(QDir::tempPath());
    const QFileInfoList tempEntries = temp.entryInfoList(
        {QStringLiteral("SubCue-*")}, QDir::Dirs | QDir::Files | QDir::Hidden, QDir::Name);
    int tempIndex = 0;
    for (const QFileInfo &entry : tempEntries) {
        appendIfExists(&targets, {QStringLiteral("cache-temp-%1").arg(tempIndex++), QStringLiteral("cache"),
            QStringLiteral("临时目录 %1").arg(entry.fileName()), entry.absoluteFilePath()});
    }

    if (!developmentTreeVisible()) return targets;

    appendIfExists(&targets, {QStringLiteral("test-tmp"), QStringLiteral("test"),
        QStringLiteral("测试临时目录 .test_tmp"), joinRoot(projectRoot_, QStringLiteral(".test_tmp"))});
    appendIfExists(&targets, {QStringLiteral("test-appdata"), QStringLiteral("test"),
        QStringLiteral("测试配置 .test_appdata"), joinRoot(projectRoot_, QStringLiteral(".test_appdata"))});

    const QDir project(projectRoot_);
    const QFileInfoList rootJunk = project.entryInfoList(
        {QStringLiteral("*.log"), QStringLiteral("*.err"), QStringLiteral("$log"),
         QStringLiteral("main.obj"), QStringLiteral("dryrun.txt")},
        QDir::Files | QDir::Hidden);
    int junkIndex = 0;
    for (const QFileInfo &entry : rootJunk) {
        appendIfExists(&targets, {QStringLiteral("test-root-%1").arg(junkIndex++), QStringLiteral("test"),
            QStringLiteral("根目录残留 %1").arg(entry.fileName()), entry.absoluteFilePath()});
    }

    const QDir outDir(joinRoot(projectRoot_, QStringLiteral("out")));
    if (outDir.exists()) {
        const QFileInfoList outFiles = outDir.entryInfoList(
            {QStringLiteral("*.txt"), QStringLiteral("*.log"), QStringLiteral("*.json"),
             QStringLiteral("*.csv"), QStringLiteral("*.docx"), QStringLiteral("*.zip"),
             QStringLiteral("*.html")},
            QDir::Files | QDir::Hidden);
        int outFileIndex = 0;
        for (const QFileInfo &entry : outFiles) {
            appendIfExists(&targets, {QStringLiteral("test-out-file-%1").arg(outFileIndex++),
                QStringLiteral("test"), QStringLiteral("探测产物 %1").arg(entry.fileName()),
                entry.absoluteFilePath()});
        }
        const QFileInfoList outDirs = outDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        int outDirIndex = 0;
        for (const QFileInfo &entry : outDirs) {
            const QString name = entry.fileName();
            if (name == QLatin1String("build") || name == QLatin1String("install")) continue;
            if (name.startsWith(QLatin1String("tmp")) || name.startsWith(QLatin1String("phase"))
                || name.startsWith(QLatin1String("inference-packages"))) {
                appendIfExists(&targets, {QStringLiteral("test-out-dir-%1").arg(outDirIndex++),
                    QStringLiteral("test"), QStringLiteral("探测目录 %1").arg(name),
                    entry.absoluteFilePath()});
            }
        }
    }

    const QDir buildRoot(joinRoot(projectRoot_, QStringLiteral("out/build")));
    if (buildRoot.exists()) {
        const QString keepName = keepBuildName();
        const QFileInfoList builds = buildRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : builds) {
            if (entry.fileName() == keepName) {
                appendIfExists(&targets, {QStringLiteral("build-package-%1").arg(keepName),
                    QStringLiteral("build"),
                    QStringLiteral("当前构建的 package 暂存（保留 %1）").arg(keepName),
                    joinRoot(entry.absoluteFilePath(), QStringLiteral("package"))});
                continue;
            }
            appendIfExists(&targets, {QStringLiteral("build-%1").arg(entry.fileName()),
                QStringLiteral("build"),
                QStringLiteral("过时构建 %1（保留当前构建：%2）").arg(entry.fileName(), keepName),
                entry.absoluteFilePath()});
        }
    }
    return targets;
}

StorageCleanupResult StorageCleaner::remove(
    const QStringList &ids,
    const std::atomic<bool> *cancel) const
{
    StorageCleanupResult result;
    const QVector<StorageTarget> targets = scan();
    QHash<QString, StorageTarget> byId;
    for (const StorageTarget &target : targets) byId.insert(target.id, target);
    for (const QString &id : ids) {
        if (cancel && cancel->load(std::memory_order_acquire)) {
            result.ok = false;
            result.error = QStringLiteral("任务已取消");
            return result;
        }
        if (!byId.contains(id)) {
            result.ok = false;
            result.error = QStringLiteral("未知清理项：%1").arg(id);
            return result;
        }
        const StorageTarget target = byId.value(id);
        if (!isPathAllowed(target.path)) {
            result.ok = false;
            result.error = QStringLiteral("拒绝清理受保护路径：%1").arg(target.path);
            return result;
        }
        const qint64 bytes = directoryBytes(target.path);
        if (!QFile::exists(target.path) && !QDir(target.path).exists()) continue;
        const QFileInfo info(target.path);
        const bool removed = info.isDir()
            ? QDir(target.path).removeRecursively()
            : QFile::remove(target.path);
        if (!removed) {
            result.ok = false;
            result.error = QStringLiteral("无法删除：%1").arg(target.path);
            return result;
        }
        result.bytesRemoved += bytes;
        result.removed.append(target.path);
    }
    return result;
}

} // namespace subcue
