#include "settings/settings_manager.h"
#include "settings/model_locator.h"
#include "settings/credential_store.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QLockFile>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QSet>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

#include <utility>

namespace subcue {
namespace {
bool redirectedPath(const QString &path)
{
    QFileInfo current(path);
    for (;;) {
        if (current.isSymbolicLink() || current.isJunction()) return true;
        const auto parent = current.dir().absolutePath();
        if (parent == current.absoluteFilePath()) return false;
        current.setFile(parent);
    }
}
const QStringList legacyPrefixes = {QStringLiteral("SubCue/ASR/"), QStringLiteral("SubCue/AIReview/"),
    QStringLiteral("SubCue/AI/"), QStringLiteral("SubCue/VoiceStudio/")};
const QStringList legacyFields = {QStringLiteral("region"), QStringLiteral("asrApiHost"), QStringLiteral("asrVerification"),
    QStringLiteral("asrConfigRevision"), QStringLiteral("asrCredentialRevision"), QStringLiteral("ttsCloudWorkspace"),
    QStringLiteral("ttsCloudCredential"), QStringLiteral("ttsConnections"), QStringLiteral("ttsActiveConnection"),
    QStringLiteral("ttsConnectionReadError"), QStringLiteral("aiProviders"), QStringLiteral("aiProviderId"),
    QStringLiteral("aiAssistEnabled"), QStringLiteral("legacyAiCredentialIds"), QStringLiteral("omniReviewEnabled"),
    QStringLiteral("omniReviewProvider"), QStringLiteral("omniReviewModel"), QStringLiteral("omniReviewBaseUrl"),
    QStringLiteral("omniReviewReasoningEffort"), QStringLiteral("apiKey"), QStringLiteral("api_key"),
    QStringLiteral("asrApiKey"), QStringLiteral("aiApiKey"), QStringLiteral("voiceApiKey"), QStringLiteral("DASHSCOPE_API_KEY")};
void stripLegacyCloud(QJsonObject &settings)
{
    for (const auto &field : legacyFields) settings.remove(field);
    if (settings.value(QStringLiteral("asrProvider")) == QLatin1String("dashscope")) {
        settings.insert(QStringLiteral("asrProvider"), QStringLiteral("funasr"));
        settings.insert(QStringLiteral("asrModel"), QStringLiteral("Fun-ASR-Nano-2512"));
    }
}
struct ConnectionDatabase {
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    explicit ConnectionDatabase(const QString &path) { db.setDatabaseName(path); }
    ~ConnectionDatabase() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
QString connectionPath(const QString &path) { return QFileInfo(path).absoluteDir().filePath(QStringLiteral("connections.sqlite")); }
bool readConnections(const QString &path, QJsonObject &settings)
{
    if (!QFileInfo::exists(connectionPath(path))) return true;
    ConnectionDatabase connection(connectionPath(path));
    if (!connection.db.open()) { settings.insert(QStringLiteral("ttsConnectionReadError"), connection.db.lastError().text()); return false; }
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT id,name,workspace_id,credential_ref,active FROM cloud_connections ORDER BY rowid"))) { settings.insert(QStringLiteral("ttsConnectionReadError"), query.lastError().text()); return false; }
    QJsonArray profiles;
    settings.insert(QStringLiteral("ttsActiveConnection"), QString());
    while (query.next()) {
        profiles.append(QJsonObject{{QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("name"), query.value(1).toString()},
            {QStringLiteral("workspace"), query.value(2).toString()}, {QStringLiteral("credential"), query.value(3).toString()}});
        if (query.value(4).toBool()) {
            settings.insert(QStringLiteral("ttsActiveConnection"), query.value(0).toString());
            settings.insert(QStringLiteral("ttsCloudWorkspace"), query.value(2).toString());
            settings.insert(QStringLiteral("ttsCloudCredential"), query.value(3).toString());
        }
    }
    settings.insert(QStringLiteral("ttsConnections"), profiles);
    return true;
}
}

SettingsManager::SettingsManager(QString path)
    : path_(path.isEmpty() ? defaultPath() : std::move(path))
{
}

QJsonObject SettingsManager::defaults()
{
    return {
        {QStringLiteral("version"), 3},
        {QStringLiteral("legacyCloudRetired"), false},
        {QStringLiteral("legacyPendingCredentials"), QJsonArray{}},
        {QStringLiteral("ttsEngines"), QJsonObject{}},
        {QStringLiteral("ttsCloudWorkspace"), QString()},
        {QStringLiteral("ttsCloudCredential"), QStringLiteral("SubCue/VoiceStudio/QwenFlash")},
        {QStringLiteral("ttsConnections"), QJsonArray{}},
        {QStringLiteral("ttsActiveConnection"), QString()},
        {QStringLiteral("appearanceMode"), QStringLiteral("dark")},
        {QStringLiteral("fontFamily"), QStringLiteral("Microsoft YaHei")},
        {QStringLiteral("fontSize1080p"), 52},
        {QStringLiteral("alignment"), QStringLiteral("bottom-center")},
        {QStringLiteral("bottomMargin1080p"), 90},
        {QStringLiteral("fontColor"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("outlineColor"), QStringLiteral("#000000")},
        {QStringLiteral("outlineSize"), 2},
        {QStringLiteral("shadow"), 0},
        {QStringLiteral("outputSrt"), true},
        {QStringLiteral("outputAss"), true},
        {QStringLiteral("outputDirectory"), QString()},
        {QStringLiteral("autoReviewEnabled"), false},
        {QStringLiteral("reviewUseSameAsr"), true},
        {QStringLiteral("reviewAsrProvider"), QStringLiteral("funasr")},
        {QStringLiteral("reviewAsrModel"), QStringLiteral("Fun-ASR-Nano-2512")},
        {QStringLiteral("legacyAiCredentialIds"), QJsonArray{}},
        {QStringLiteral("omniReviewTimeoutMs"), 120000},
        {QStringLiteral("omniReviewCutConfidence"), 0.90},
        {QStringLiteral("omniReviewReviewConfidence"), 0.65},
        {QStringLiteral("omniReviewMinTailMs"), 150},
        {QStringLiteral("omniReviewPreferredTailMs"), 250},
        {QStringLiteral("omniReviewMaxTailMs"), 500},
        {QStringLiteral("omniReviewHandleMs"), 150},
        {QStringLiteral("omniReviewWindowMs"), 90000},
        {QStringLiteral("omniReviewOverlapMs"), 15000},
        {QStringLiteral("omniReviewCandidatePreMs"), 8000},
        {QStringLiteral("omniReviewCandidatePostMs"), 15000},
        {QStringLiteral("asrVerification"), QJsonObject{}},
        {QStringLiteral("asrConfigRevision"), 0},
        {QStringLiteral("asrCredentialRevision"), 0},
        {QStringLiteral("asrModel"), QStringLiteral("Fun-ASR-Nano-2512")},
        {QStringLiteral("asrProvider"), QStringLiteral("funasr")},
        {QStringLiteral("modelsRoot"), ModelLocator::compiledDefaultRoot()},
        {QStringLiteral("localAsrPython"), QString::fromUtf8(SUBCUE_PROJECT_INFERENCE_PYTHON)},
        {QStringLiteral("region"), QStringLiteral("beijing")},
        {QStringLiteral("asrApiHost"), QString()},
        {QStringLiteral("ffmpegPath"), QString()},
    };
}

QString SettingsManager::defaultPath()
{
    const QString appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    const QString base = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        : QDir(appData).filePath(QStringLiteral("SubCue"));
    const QString destination = QDir(base).filePath(QStringLiteral("settings.json"));
    if (!appData.isEmpty() && !QFileInfo::exists(destination)) {
        const QString source = QDir(appData).filePath(
            QStringLiteral("AutoSubtitleAligner/settings.json"));
        if (QFileInfo::exists(source) && QDir().mkpath(QFileInfo(destination).absolutePath())) {
            QFile::copy(source, destination);
        }
    }
    return destination;
}

QJsonObject SettingsManager::load() const
{
    QJsonObject result = defaults();
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) { if (!readConnections(path_, result)) qWarning("无法读取 SQLite 云端连接配置。"); return result; }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { if (!readConnections(path_, result)) qWarning("无法读取 SQLite 云端连接配置。"); return result; }
    const QJsonObject loaded = document.object();
    const int loadedVersion = loaded.value(QStringLiteral("version")).toInt(1);
    const QStringList droppedAiKeys = {
        QStringLiteral("aiAssistEnabled"),
        QStringLiteral("aiProviderId"),
        QStringLiteral("aiProviders"),
        QStringLiteral("omniReviewEnabled"),
        QStringLiteral("omniReviewProvider"),
        QStringLiteral("omniReviewModel"),
        QStringLiteral("omniReviewBaseUrl"),
        QStringLiteral("omniReviewReasoningEffort"),
    };
    for (auto iterator = result.begin(); iterator != result.end(); ++iterator) {
        if (droppedAiKeys.contains(iterator.key())) continue;
        if (loaded.contains(iterator.key())) iterator.value() = loaded.value(iterator.key());
    }
    if (loadedVersion < 3) {
        QJsonArray leftoverIds = result.value(QStringLiteral("legacyAiCredentialIds")).toArray();
        for (const QJsonValue &value : loaded.value(QStringLiteral("aiProviders")).toArray()) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            bool exists = false;
            for (const QJsonValue &known : leftoverIds) {
                if (known.toString() == id) {
                    exists = true;
                    break;
                }
            }
            if (!exists) leftoverIds.append(id);
        }
        result.insert(QStringLiteral("legacyAiCredentialIds"), leftoverIds);
    }
    if (!loaded.contains(QStringLiteral("asrProvider"))) {
        // 旧配置中的模型名来自云端，不将其误认为本地 Fun-ASR 模型。
        result.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope"));
    }
    if (result.value(QStringLiteral("asrProvider")).toString() == QLatin1String("whisper")) {
        result.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope"));
        result.insert(QStringLiteral("asrVerification"), QJsonObject{});
    }
    if (!loaded.contains(QStringLiteral("modelsRoot"))) {
        const QString derived = ModelLocator::derivedRootFromLegacy(loaded);
        if (!derived.isEmpty()) result.insert(QStringLiteral("modelsRoot"), derived);
    }
    const QString resolvedRoot = ModelLocator::root(result);
    if (result.value(QStringLiteral("modelsRoot")).toString().trimmed().isEmpty() && !resolvedRoot.isEmpty())
        result.insert(QStringLiteral("modelsRoot"), resolvedRoot);
    result.insert(QStringLiteral("qwen3AsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3Asr, result));
    result.insert(QStringLiteral("qwen3ForcedAlignerModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, result));
    result.insert(QStringLiteral("funAsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::FunAsrNano, result));
    result.insert(QStringLiteral("version"), 3);
    if (result.value(QStringLiteral("legacyCloudRetired")).toBool()) stripLegacyCloud(result);
    else if (!readConnections(path_, result)) qWarning("无法读取 SQLite 云端连接配置，请检查 connections.sqlite。");
    return result;
}

bool SettingsManager::save(const QJsonObject &settings, QString *errorMessage) const
{
    if (settings.contains(QStringLiteral("ttsConnectionReadError"))) {
        if (errorMessage) *errorMessage = QStringLiteral("连接配置读取失败，为保护原数据已阻止保存；请修复 connections.sqlite 后重新启动：%1").arg(settings.value(QStringLiteral("ttsConnectionReadError")).toString());
        return false;
    }
    QJsonObject safe = defaults();
    for (auto iterator = safe.begin(); iterator != safe.end(); ++iterator) {
        if (settings.contains(iterator.key())) iterator.value() = settings.value(iterator.key());
    }
    safe.insert(QStringLiteral("qwen3AsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3Asr, safe));
    safe.insert(QStringLiteral("qwen3ForcedAlignerModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, safe));
    safe.insert(QStringLiteral("funAsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::FunAsrNano, safe));
    safe.insert(QStringLiteral("version"), 3);
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建设置目录");
        return false;
    }
    ConnectionDatabase connection(connectionPath(path_));
    const auto failed = [&](const QString &message) { if (errorMessage) *errorMessage = message; return false; };
    if (!connection.db.open()) return failed(connection.db.lastError().text());
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS cloud_connections(id TEXT PRIMARY KEY,name TEXT NOT NULL,workspace_id TEXT NOT NULL,credential_ref TEXT NOT NULL,active INTEGER NOT NULL CHECK(active IN (0,1)))"))
        || !connection.db.transaction()) return failed(query.lastError().isValid() ? query.lastError().text() : connection.db.lastError().text());
    const bool retired = settings.value(QStringLiteral("legacyCloudRetired")).toBool();
    auto profiles = retired ? QJsonArray{} : settings.value(QStringLiteral("ttsConnections")).toArray();
    auto active = retired ? QString() : settings.value(QStringLiteral("ttsActiveConnection")).toString();
    if (!retired && profiles.isEmpty() && !settings.value(QStringLiteral("ttsCloudWorkspace")).toString().trimmed().isEmpty()) {
        active = QStringLiteral("legacy");
        profiles.append(QJsonObject{{QStringLiteral("id"), active}, {QStringLiteral("name"), QStringLiteral("默认连接")},
            {QStringLiteral("workspace"), settings.value(QStringLiteral("ttsCloudWorkspace"))}, {QStringLiteral("credential"), safe.value(QStringLiteral("ttsCloudCredential"))}});
    }
    QSet<QString> ids;
    if (!query.exec(QStringLiteral("DELETE FROM cloud_connections"))) return failed(query.lastError().text());
    for (const auto &value : profiles) {
        const auto profile = value.toObject();
        const auto id = profile.value(QStringLiteral("id")).toString();
        const auto name = profile.value(QStringLiteral("name")).toString().trimmed();
        const auto workspace = profile.value(QStringLiteral("workspace")).toString().trimmed();
        const auto credential = profile.value(QStringLiteral("credential")).toString().trimmed();
        if (id.isEmpty() || id.size() > 128 || ids.contains(id) || name.isEmpty() || name.size() > 128 || workspace.isEmpty() || workspace.size() > 256
            || !credential.startsWith(QStringLiteral("SubCue/VoiceStudio/")) || credential.size() > 128)
            return failed(QStringLiteral("云端连接配置无效：请检查名称、Workspace ID、凭据引用及重复标识。"));
        ids.insert(id);
        query.prepare(QStringLiteral("INSERT INTO cloud_connections VALUES(?,?,?,?,?)"));
        query.addBindValue(id); query.addBindValue(name); query.addBindValue(workspace); query.addBindValue(credential); query.addBindValue(id == active);
        if (!query.exec()) return failed(query.lastError().text());
    }
    if ((!profiles.isEmpty() && !ids.contains(active)) || (profiles.isEmpty() && !active.isEmpty())) return failed(QStringLiteral("请选择有效的当前云端连接。"));
    // 连接信息仅存 SQLite；JSON 保留其它应用设置，API Key 始终由安全凭据库管理。
    safe.remove(QStringLiteral("ttsCloudWorkspace")); safe.remove(QStringLiteral("ttsConnections")); safe.remove(QStringLiteral("ttsActiveConnection"));
    if (retired) stripLegacyCloud(safe);
    QFile previous(path_); const bool existed = previous.exists();
    QByteArray previousBytes;
    if (existed) { if (!previous.open(QIODevice::ReadOnly)) return failed(previous.errorString()); previousBytes = previous.readAll(); previous.close(); }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(safe).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (!connection.db.commit()) {
        const auto error = connection.db.lastError().text();
        if (existed) { QSaveFile restore(path_); if (!restore.open(QIODevice::WriteOnly) || restore.write(previousBytes) != previousBytes.size() || !restore.commit()) return failed(QStringLiteral("SQLite 提交失败且设置回滚失败：%1").arg(error)); }
        else QFile::remove(path_);
        return failed(error);
    }
    return true;
}

QJsonObject SettingsManager::legacyCloudPreview(const ICredentialStore &credentials, QString *errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    const auto fail = [&](const QString &message) { if (errorMessage) *errorMessage = message; return QJsonObject{}; };
    const QFileInfo info(path_);
    if (redirectedPath(path_) || redirectedPath(connectionPath(path_))) return fail(QStringLiteral("拒绝处理重定向的旧配置或数据库路径。"));
    QByteArray bytes; QJsonObject raw;
    if (info.exists()) {
        QFile file(path_); if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("旧设置不可读，未执行清理。"));
        bytes = file.readAll(); QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(QStringLiteral("旧设置格式无效，未执行清理。"));
        raw = document.object();
    }
    QStringList targets;
    for (const auto &prefix : legacyPrefixes) {
        QString error; const auto names = credentials.list(prefix, &error);
        if (!error.isEmpty()) return fail(error);
        for (const auto &name : names) if (name.startsWith(prefix) && !targets.contains(name)) targets.append(name);
    }
    for (const auto &value : raw.value(QStringLiteral("legacyPendingCredentials")).toArray()) {
        const auto name = value.toString(); bool allowed = false;
        for (const auto &prefix : legacyPrefixes) if (name.startsWith(prefix)) allowed = true;
        if (!allowed) return fail(QStringLiteral("旧凭据重试记录包含非法目标，已停止清理。"));
        if (!targets.contains(name)) targets.append(name);
    }
    targets.sort(); QStringList fields;
    for (const auto &field : legacyFields) if (raw.contains(field)) fields.append(field);
    QJsonArray connections;
    if (QFileInfo::exists(connectionPath(path_))) {
        ConnectionDatabase connection(connectionPath(path_));
        if (!connection.db.open()) return fail(QStringLiteral("连接数据库不可读，未执行清理。"));
        QSqlQuery query(connection.db);
        if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name='cloud_connections'"))) return fail(QStringLiteral("无法检查旧连接表。"));
        if (query.next()) {
            if (!query.exec(QStringLiteral("SELECT id,name,workspace_id,credential_ref,active FROM cloud_connections ORDER BY id"))) return fail(QStringLiteral("旧连接表不可读。"));
            while (query.next()) connections.append(QJsonArray{query.value(0).toString(), query.value(1).toString(), query.value(2).toString(), query.value(3).toString(), query.value(4).toBool()});
        }
    }
    const auto digest = QCryptographicHash::hash(bytes + QJsonDocument(connections).toJson(QJsonDocument::Compact)
        + QJsonDocument(QJsonArray::fromStringList(targets)).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    return {{QStringLiteral("path"), info.absoluteFilePath()}, {QStringLiteral("fields"), QJsonArray::fromStringList(fields)},
        {QStringLiteral("credentials"), QJsonArray::fromStringList(targets)}, {QStringLiteral("connections"), connections.size()},
        {QStringLiteral("metadataSnapshot"), QString::fromLatin1(QCryptographicHash::hash(bytes + QJsonDocument(connections).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("snapshot"), QString::fromLatin1(digest)}};
}
bool SettingsManager::retireLegacyCloud(const ICredentialStore &credentials, const QJsonObject &preview, QString *errorMessage) const
{
    const auto fail = [&](const QString &message) { if (errorMessage) *errorMessage = message; return false; };
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail(QStringLiteral("设置目录不可写。"));
    QLockFile lock(connectionPath(path_) + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) return fail(QStringLiteral("云端配置正在使用，未执行清理。"));
    const auto current = legacyCloudPreview(credentials, errorMessage);
    if (current.isEmpty()) return false;
    if (preview != current) return fail(QStringLiteral("清理预览已变化，请重新预览。"));
    QJsonObject raw;
    QFile file(path_);
    if (file.exists()) { if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("设置不可读。")); raw = QJsonDocument::fromJson(file.readAll()).object(); file.close(); }
    stripLegacyCloud(raw); raw.insert(QStringLiteral("legacyCloudRetired"), true);
    raw.insert(QStringLiteral("legacyPendingCredentials"), current.value(QStringLiteral("credentials")));
    const auto write = [&] {
        QSaveFile output(path_); if (!output.open(QIODevice::WriteOnly)) return false;
        const auto bytes = QJsonDocument(raw).toJson(QJsonDocument::Indented);
        return output.write(bytes) == bytes.size() && output.commit();
    };
    // 先持久记录退役与待删目标，再清表、删除凭据；崩溃后不会重新启用旧入口。
    if (!write()) return fail(QStringLiteral("无法记录旧配置退役状态，未删除凭据。"));
    if (QFileInfo::exists(connectionPath(path_))) {
        ConnectionDatabase connection(connectionPath(path_));
        if (!connection.db.open()) return fail(QStringLiteral("无法打开旧连接数据库，凭据未删除。"));
        QSqlQuery query(connection.db);
        if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name='cloud_connections'"))) return fail(QStringLiteral("无法检查旧连接表。"));
        if (query.next() && !query.exec(QStringLiteral("DELETE FROM cloud_connections"))) return fail(QStringLiteral("旧连接清理失败，凭据未删除。"));
    }
    QJsonArray pending;
    for (const auto &value : current.value(QStringLiteral("credentials")).toArray())
        if (!credentials.remove(value.toString())) pending.append(value);
    raw.insert(QStringLiteral("legacyPendingCredentials"), pending);
    if (!write()) return fail(QStringLiteral("凭据处理后未能更新重试记录，请重新预览核验。"));
    if (!pending.isEmpty()) return fail(QStringLiteral("部分旧凭据删除失败，已记录目标，需重新预览并重试。"));
    return true;
}

const QString &SettingsManager::path() const noexcept { return path_; }

} // namespace subcue
