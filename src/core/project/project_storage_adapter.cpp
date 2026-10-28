#include "project/project_storage_adapter.h"
#include <QtCore/QLockFile>
#include <QtCore/QUuid>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QHash>
#include <utility>

namespace subcue {
namespace {
bool adapterError(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
QString identity(const QJsonObject &metadata)
{
    const QString id = metadata.value(QStringLiteral("projectIdentity")).toObject().value(QStringLiteral("id")).toString();
    return id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
}
}

ProjectStorageAdapter::ProjectStorageAdapter(QString path, Format format)
    : path_(std::move(path)), format_(format) {}

std::optional<QJsonObject> ProjectStorageAdapter::captureModule(ProjectStore &store, Data data,
    const QString &projectId, QString *error, const std::atomic<bool> *cancel)
{
    if (projectId.isEmpty()) { adapterError(error, QStringLiteral("模块所属工程 ID 为空。")); return {}; }
    QJsonObject references;
    auto link = [&](const QString &path, const QString &role, const QByteArray &expected = QByteArray{},
                    const QString &kind = QStringLiteral("media")) {
        if (path.isEmpty()) return true;
        const auto id = store.importAsset(path, kind, error, cancel, expected);
        if (!id) return false;
        references.insert(role, *id);
        return true;
    };
    if (auto subtitle = std::get_if<Project>(&data)) {
        if (!link(subtitle->mediaPath, QStringLiteral("mediaAssetId"),
            QByteArray::fromHex(subtitle->mediaFingerprint.value(QStringLiteral("sha256")).toString().toLatin1()))) return {};
        subtitle->mediaPath.clear();
        auto identity = subtitle->metadata.value(QStringLiteral("projectIdentity")).toObject();
        identity.insert(QStringLiteral("id"), projectId);
        subtitle->metadata.insert(QStringLiteral("projectIdentity"), identity);
        QJsonArray assets;
        QJsonArray assetIds;
        for (const auto &value : subtitle->state.value(QStringLiteral("assets")).toArray()) {
            auto asset = value.toObject();
            const QString path = asset.value(QStringLiteral("path")).toString();
            QString assetId;
            if (!path.isEmpty()) {
                const auto id = store.importAsset(path, (asset.value(QStringLiteral("type")).toString() == QStringLiteral("文稿")
                    || asset.value(QStringLiteral("kind")).toString() == QStringLiteral("文稿"))
                    ? QStringLiteral("derived") : QStringLiteral("media"), error, cancel);
                if (!id) return {};
                assetId = *id;
            }
            assetIds.append(assetId);
            asset.insert(QStringLiteral("path"), QString());
            assets.append(asset);
        }
        if (subtitle->state.contains(QStringLiteral("assets"))) {
            subtitle->state.insert(QStringLiteral("assets"), assets);
            references.insert(QStringLiteral("subtitleAssetIds"), assetIds);
        }
    } else if (auto roughCut = std::get_if<RoughCutProject>(&data)) {
        if (!link(roughCut->mediaPath, QStringLiteral("mediaAssetId"), roughCut->mediaSha256)
            || !link(roughCut->scriptPath, QStringLiteral("scriptAssetId"), {}, QStringLiteral("derived"))) return {};
        roughCut->mediaPath.clear(); roughCut->scriptPath.clear();
        auto identity = roughCut->state.value(QStringLiteral("projectIdentity")).toObject();
        identity.insert(QStringLiteral("id"), projectId);
        roughCut->state.insert(QStringLiteral("projectIdentity"), identity);
    } else {
        adapterError(error, QStringLiteral("仅支持字幕或粗剪编辑模型的模块捕获。")); return {};
    }
    const auto module = packModule(data, error, cancel);
    if (!module || !unpackModule(*module, error)) return {};
    references.insert(QStringLiteral("snapshot"), *module);
    return references;
}

std::optional<ProjectStorageAdapter::Data> ProjectStorageAdapter::loadModule(const QString &module, QString *error) const
{
    if (format_ != Format::UnifiedSqlite || (module != QStringLiteral("subtitle") && module != QStringLiteral("roughcut"))) {
        adapterError(error, QStringLiteral("仅统一工程支持旧模块快照读取。")); return {};
    }
    ProjectStore store(path_);
    const auto project = store.load(error);
    if (!project) return {};
    const auto wrapper = project->modules.value(module).toObject();
    auto data = unpackModule(wrapper.value(QStringLiteral("snapshot")).toObject(), error);
    if (!data) return {};
    QHash<QString, QString> paths;
    QString assetError;
    const auto manifest = store.assets(&assetError);
    if (!assetError.isEmpty()) { adapterError(error, assetError); return {}; }
    for (const auto &value : manifest) {
        const auto asset = value.toObject();
        paths.insert(asset.value(QStringLiteral("id")).toString(), store.resolveAudioPath(asset.value(QStringLiteral("path")).toString()));
    }
    auto resolve = [&](const QString &id, QString &path) {
        if (id.isEmpty()) { path.clear(); return true; }
        if (!paths.contains(id)) return adapterError(error, QStringLiteral("模块引用的项目资产不存在：%1").arg(id));
        path = paths.value(id);
        return true;
    };
    if (auto subtitle = std::get_if<Project>(&*data)) {
        if (module != QStringLiteral("subtitle")) { adapterError(error, QStringLiteral("模块类型不匹配。")); return {}; }
        if (!resolve(wrapper.value(QStringLiteral("mediaAssetId")).toString(), subtitle->mediaPath)) return {};
        QJsonArray assets;
        const auto sourceAssets = subtitle->state.value(QStringLiteral("assets")).toArray();
        const auto assetIds = wrapper.value(QStringLiteral("subtitleAssetIds")).toArray();
        const bool separateReferences = wrapper.contains(QStringLiteral("subtitleAssetIds"));
        if (separateReferences && (!wrapper.value(QStringLiteral("subtitleAssetIds")).isArray() || assetIds.size() != sourceAssets.size())) {
            adapterError(error, QStringLiteral("字幕资源映射与模块快照不匹配。")); return {};
        }
        for (qsizetype index = 0; index < sourceAssets.size(); ++index) {
            const auto &value = sourceAssets.at(index);
            auto asset = value.toObject();
            QString path;
            if (separateReferences && !assetIds.at(index).isString()) {
                adapterError(error, QStringLiteral("字幕资源 ID 类型无效。")); return {};
            }
            if (!resolve(separateReferences ? assetIds.at(index).toString() : asset.value(QStringLiteral("assetId")).toString(), path)) return {};
            if (!separateReferences) asset.remove(QStringLiteral("assetId"));
            asset.insert(QStringLiteral("path"), path);
            assets.append(asset);
        }
        if (subtitle->state.contains(QStringLiteral("assets"))) subtitle->state.insert(QStringLiteral("assets"), assets);
    } else if (auto roughCut = std::get_if<RoughCutProject>(&*data)) {
        if (module != QStringLiteral("roughcut")) { adapterError(error, QStringLiteral("模块类型不匹配。")); return {}; }
        if (!resolve(wrapper.value(QStringLiteral("mediaAssetId")).toString(), roughCut->mediaPath)
            || !resolve(wrapper.value(QStringLiteral("scriptAssetId")).toString(), roughCut->scriptPath)) return {};
    }
    return data;
}

std::optional<QJsonObject> ProjectStorageAdapter::packModule(const Data &data,
    QString *error, const std::atomic<bool> *cancel)
{
    QTemporaryDir directory;
    if (!directory.isValid()) { adapterError(error, QStringLiteral("无法创建模块快照临时目录。")); return {}; }
    const QString path = directory.filePath(QStringLiteral("module"));
    QString format;
    if (const auto project = std::get_if<Project>(&data)) {
        if (!project->mediaPath.isEmpty()) {
            adapterError(error, QStringLiteral("模块媒体路径必须先转换为资产引用。")); return {};
        }
        for (const auto &asset : project->state.value(QStringLiteral("assets")).toArray()) {
            if (!asset.toObject().value(QStringLiteral("path")).toString().isEmpty()) {
                adapterError(error, QStringLiteral("模块素材路径必须先转换为资产引用。")); return {};
            }
        }
        if (!ProjectSerializer::save(path, *project, error, cancel)) return {};
        format = QStringLiteral("subtitle-json");
    } else if (const auto roughCut = std::get_if<RoughCutProject>(&data)) {
        if (!roughCut->mediaPath.isEmpty() || !roughCut->scriptPath.isEmpty()) {
            adapterError(error, QStringLiteral("粗剪素材路径必须先转换为资产引用。")); return {};
        }
        if (!RoughCutProjectSerializer::save(path, *roughCut, error, cancel)) return {};
        format = QStringLiteral("roughcut-sqlite");
    } else {
        adapterError(error, QStringLiteral("统一工程不能作为旧模块快照打包。")); return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        adapterError(error, QStringLiteral("模块快照无法读取或超过 64MiB 限制。")); return {};
    }
    return QJsonObject{{QStringLiteral("snapshotVersion"), 1}, {QStringLiteral("format"), format},
        {QStringLiteral("payloadBase64"), QString::fromLatin1(file.readAll().toBase64())}};
}

std::optional<ProjectStorageAdapter::Data> ProjectStorageAdapter::unpackModule(const QJsonObject &module,
    QString *error)
{
    const QString format = module.value(QStringLiteral("format")).toString();
    const QString payload = module.value(QStringLiteral("payloadBase64")).toString();
    if (module.value(QStringLiteral("snapshotVersion")).toInt() != 1
        || (format != QStringLiteral("subtitle-json") && format != QStringLiteral("roughcut-sqlite"))
        || payload.isEmpty() || payload.size() > 90 * 1024 * 1024) {
        adapterError(error, QStringLiteral("模块快照格式或大小无效。")); return {};
    }
    const auto decoded = QByteArray::fromBase64Encoding(payload.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > 64 * 1024 * 1024) {
        adapterError(error, QStringLiteral("模块快照编码无效。")); return {};
    }
    QTemporaryDir directory;
    if (!directory.isValid()) { adapterError(error, QStringLiteral("无法创建模块读取临时目录。")); return {}; }
    const QString path = directory.filePath(QStringLiteral("module"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(decoded.decoded) != decoded.decoded.size()) {
        adapterError(error, QStringLiteral("模块快照临时写入失败。")); return {};
    }
    file.close();
    if (format == QStringLiteral("subtitle-json")) {
        const auto project = ProjectSerializer::load(path, error);
        if (project) return Data{*project};
    } else {
        const auto project = RoughCutProjectSerializer::load(path, error);
        if (project) return Data{*project};
    }
    return {};
}

std::optional<ProjectStorageAdapter::Snapshot> ProjectStorageAdapter::load(QString *error) const
{
    if (format_ == Format::UnifiedSqlite) {
        const auto project = ProjectStore(path_).load(error);
        if (!project) return {};
        return Snapshot{project->projectId, QByteArray::number(project->revision), *project};
    }
    // 锁保护同应用内的读取/保存；哈希额外检测其他程序造成的变化。
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { adapterError(error, QStringLiteral("工程正在被其他操作使用。")); return {}; }
    const auto before = RoughCutProjectSerializer::mediaSha256(path_, error);
    if (before.isEmpty()) return {};
    std::optional<Snapshot> result;
    if (format_ == Format::SubtitleJson) {
        const auto project = ProjectSerializer::load(path_, error);
        if (project) result = Snapshot{identity(project->metadata), before, *project};
    } else {
        const auto project = RoughCutProjectSerializer::load(path_, error);
        if (project) result = Snapshot{identity(project->state), before, *project};
    }
    if (result && before != RoughCutProjectSerializer::mediaSha256(path_, error)) {
        adapterError(error, QStringLiteral("读取期间工程被外部修改，请重新打开。")); return {};
    }
    return result;
}

bool ProjectStorageAdapter::save(const Snapshot &snapshot, QString *error, const std::atomic<bool> *cancel,
                                SaveResult *result) const
{
    if (cancel && cancel->load()) return adapterError(error, QStringLiteral("工程保存已取消。"));
    if (snapshot.projectId.isEmpty()) return adapterError(error, QStringLiteral("工程 ID 为空。"));
    if (format_ == Format::UnifiedSqlite) {
        const auto project = std::get_if<ProjectStore::Snapshot>(&snapshot.data);
        if (!project || project->projectId != snapshot.projectId
            || QByteArray::number(project->revision) != snapshot.storageVersion)
            return adapterError(error, QStringLiteral("工程格式或版本不匹配。"));
        qint64 revision = project->revision;
        if (!ProjectStore(path_).save(*project, error, &revision, cancel)) return false;
        if (result) {
            result->revision = revision;
            result->storageVersion = QByteArray::number(revision);
        }
        return true;
    }
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return adapterError(error, QStringLiteral("工程正在被其他操作使用。"));
    if (snapshot.storageVersion.isEmpty()
        || snapshot.storageVersion != RoughCutProjectSerializer::mediaSha256(path_, error, cancel))
        return adapterError(error, QStringLiteral("工程已被外部修改，保存未覆盖现有数据。"));
    if (format_ == Format::SubtitleJson) {
        const auto project = std::get_if<Project>(&snapshot.data);
        if (!project) return adapterError(error, QStringLiteral("工程格式不匹配。"));
        Project copy = *project;
        auto metadata = copy.metadata.value(QStringLiteral("projectIdentity")).toObject();
        metadata.insert(QStringLiteral("id"), snapshot.projectId);
        copy.metadata.insert(QStringLiteral("projectIdentity"), metadata);
        QByteArray version;
        if (!ProjectSerializer::save(path_, copy, error, cancel, result ? &version : nullptr)) return false;
        if (result) { result->revision.reset(); result->storageVersion = version; }
        return true;
    }
    const auto project = std::get_if<RoughCutProject>(&snapshot.data);
    if (!project) return adapterError(error, QStringLiteral("工程格式不匹配。"));
    RoughCutProject copy = *project;
    auto metadata = copy.state.value(QStringLiteral("projectIdentity")).toObject();
    metadata.insert(QStringLiteral("id"), snapshot.projectId);
    copy.state.insert(QStringLiteral("projectIdentity"), metadata);
    QByteArray version;
    if (!RoughCutProjectSerializer::save(path_, copy, error, cancel, result ? &version : nullptr)) return false;
    if (result) { result->revision.reset(); result->storageVersion = version; }
    return true;
}

bool ProjectStorageAdapter::supports(const QString &module) const
{
    return (format_ == Format::SubtitleJson && module == QStringLiteral("subtitle"))
        || (format_ == Format::RoughCutSqlite && module == QStringLiteral("roughcut"))
        || (format_ == Format::UnifiedSqlite && (module == QStringLiteral("subtitle")
            || module == QStringLiteral("roughcut") || module == QStringLiteral("voice")));
}
} // namespace subcue
