#include "tts/voice_library.h"
#include "project/project_store.h"
#include "project/asset_file.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QUuid>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <algorithm>
#include <cmath>

namespace subcue {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
class Connection final {
public:
    explicit Connection(const QString &path)
        : name(QUuid::createUuid().toString()), db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    { db.setDatabaseName(path); db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000")); }
    ~Connection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    bool open(QString *error)
    {
        if (!db.open()) return fail(error, db.lastError().text());
        QSqlQuery query(db);
        return query.exec(QStringLiteral("PRAGMA foreign_keys=ON")) || fail(error, query.lastError().text());
    }
    QString name;
    QSqlDatabase db;
};
bool containsSecret(const QJsonValue &value, int depth = 0)
{
    if (depth > 16) return true;
    if (value.isArray()) {
        for (const auto &item : value.toArray()) if (containsSecret(item, depth + 1)) return true;
    } else if (value.isObject()) {
        const auto object = value.toObject();
        for (auto item = object.begin(); item != object.end(); ++item) {
            const QString key = item.key().toLower();
            if (key == QStringLiteral("apikey") || key == QStringLiteral("api_key") || key == QStringLiteral("secret")
                || key == QStringLiteral("accesstoken") || key == QStringLiteral("password") || key == QStringLiteral("authorization")) return true;
            if (containsSecret(item.value(), depth + 1)) return true;
        }
    }
    return false;
}
bool validMetadata(const QJsonObject &metadata)
{
    // 凭据只允许引用，拒绝把密钥放入声音版本或项目副本。
    const auto json = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (metadata.isEmpty() || json.size() > 1024 * 1024) return false;
    if (metadata.contains(QStringLiteral("manualEmotion"))
        && (!metadata.value(QStringLiteral("manualEmotion")).isString()
            || !VoiceLibrary::validEmotionLabel(metadata.value(QStringLiteral("manualEmotion")).toString()))) return false;
    if (metadata.contains(QStringLiteral("emotionPrediction"))
        && !VoiceLibrary::validEmotionPrediction(metadata.value(QStringLiteral("emotionPrediction")).toObject())) return false;
    return !containsSecret(metadata);
}
}

VoiceLibrary::VoiceLibrary(QString path) : path_(std::move(path)) {}

bool VoiceLibrary::validEmotionLabel(const QString &label)
{
    return QStringList{QStringLiteral("unclassified"), QStringLiteral("angry"), QStringLiteral("disgusted"),
        QStringLiteral("fearful"), QStringLiteral("happy"), QStringLiteral("neutral"), QStringLiteral("other"),
        QStringLiteral("sad"), QStringLiteral("surprised"), QStringLiteral("unknown")}.contains(label);
}

bool VoiceLibrary::validEmotionPrediction(const QJsonObject &prediction)
{
    const QStringList labels{QStringLiteral("angry"), QStringLiteral("disgusted"), QStringLiteral("fearful"), QStringLiteral("happy"),
        QStringLiteral("neutral"), QStringLiteral("other"), QStringLiteral("sad"), QStringLiteral("surprised"), QStringLiteral("unknown")};
    const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    for (const auto &key : {QStringLiteral("assetSha256"), QStringLiteral("modelVersion"), QStringLiteral("weightSha256")})
        if (!hash.match(prediction.value(key).toString()).hasMatch()) return false;
    if (prediction.value(QStringLiteral("model")).toString() != QStringLiteral("emotion2vec_plus_base")
        || prediction.value(QStringLiteral("policy")).toString() != QStringLiteral("review-v1-score0.6-margin0.15")
        || !prediction.value(QStringLiteral("requiresReview")).isBool() || !prediction.value(QStringLiteral("reviewed")).isBool()) return false;
    const auto scores = prediction.value(QStringLiteral("scores")).toArray();
    if (scores.size() != 9) return false;
    double sum = 0, first = -1, second = -1; int top = 0;
    for (int i = 0; i < scores.size(); ++i) {
        const double score = scores[i].toDouble(-1);
        if (!std::isfinite(score) || score < 0 || score > 1) return false;
        sum += score;
        if (score > first) { second = first; first = score; top = i; }
        else if (score > second) second = score;
    }
    const bool review = first < 0.6 || first - second < 0.15 || labels[top] == QLatin1String("other") || labels[top] == QLatin1String("unknown");
    return std::abs(sum - 1) < 1e-4 && prediction.value(QStringLiteral("label")).toString() == labels[top]
        && std::abs(prediction.value(QStringLiteral("score")).toDouble(-1) - first) < 1e-6
        && std::abs(prediction.value(QStringLiteral("margin")).toDouble(-1) - (first - second)) < 1e-6
        && prediction.value(QStringLiteral("requiresReview")).toBool() == review;
}

bool VoiceLibrary::initialize(QString *error)
{
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail(error, QStringLiteral("无法创建声音库目录。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("声音库正在被其他操作使用。"));
    Connection connection(path_);
    if (!connection.open(error) || !connection.db.transaction()) return false;
    QSqlQuery query(connection.db);
    for (const auto &sql : {
        "CREATE TABLE IF NOT EXISTS library(version INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS profiles(id TEXT PRIMARY KEY,name TEXT NOT NULL,archived INTEGER NOT NULL DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS voice_versions(id TEXT PRIMARY KEY,profile_id TEXT NOT NULL REFERENCES profiles(id),metadata TEXT NOT NULL,reference_path TEXT NOT NULL,reference_sha256 BLOB,created_at INTEGER NOT NULL)"}) {
        if (!query.exec(QString::fromLatin1(sql))) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    }
    if (!query.exec(QStringLiteral("SELECT version FROM library"))) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    if (query.next()) {
        const int version = query.value(0).toInt();
        if ((version != 1 && version != 2) || query.next()) { connection.db.rollback(); return fail(error, QStringLiteral("不支持的声音库版本。")); }
    } else if (!query.exec(QStringLiteral("INSERT INTO library VALUES(1)"))) {
        connection.db.rollback(); return fail(error, query.lastError().text());
    }
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS profile_defaults(profile_id TEXT PRIMARY KEY REFERENCES profiles(id),version_id TEXT NOT NULL REFERENCES voice_versions(id))"))
        || !query.exec(QStringLiteral("UPDATE library SET version=2"))) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

std::optional<QString> VoiceLibrary::createProfile(const QString &name, QString *error)
{
    if (name.trimmed().isEmpty() || name.size() > 256) { fail(error, QStringLiteral("声音名称为空或过长。")); return {}; }
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { fail(error, QStringLiteral("声音库正在被其他操作使用。")); return {}; }
    Connection connection(path_);
    if (!connection.open(error)) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("INSERT INTO profiles(id,name) VALUES(?,?)"));
    query.addBindValue(id); query.addBindValue(name.trimmed());
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    return id;
}

bool VoiceLibrary::renameProfile(const QString &id, const QString &name, QString *error)
{
    if (QUuid(id).isNull() || name.trimmed().isEmpty() || name.size() > 256) return fail(error, QStringLiteral("声音身份或名称无效。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("声音库正在被其他操作使用。"));
    Connection connection(path_); if (!connection.open(error)) return false;
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("UPDATE profiles SET name=? WHERE id=?"));
    query.addBindValue(name.trimmed()); query.addBindValue(id);
    return (query.exec() && query.numRowsAffected() == 1) || fail(error, QStringLiteral("声音重命名失败或档案不存在。"));
}

bool VoiceLibrary::setProfileArchived(const QString &id, bool archived, QString *error)
{
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("声音库正在被其他操作使用。"));
    Connection connection(path_); if (!connection.open(error)) return false;
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("UPDATE profiles SET archived=? WHERE id=?"));
    query.addBindValue(archived ? 1 : 0); query.addBindValue(id);
    return (query.exec() && query.numRowsAffected() == 1) || fail(error, QStringLiteral("声音停用失败或档案不存在。"));
}

bool VoiceLibrary::setDefaultVersion(const QString &profileId, const QString &versionId, QString *error)
{
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("声音库正在被其他操作使用。"));
    Connection connection(path_); if (!connection.open(error)) return false;
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT reference_path,metadata FROM voice_versions JOIN profiles ON profiles.id=profile_id WHERE voice_versions.id=? AND profiles.id=? AND archived=0"));
    query.addBindValue(versionId); query.addBindValue(profileId);
    if (!query.exec() || !query.next()) return fail(error, QStringLiteral("默认版本不属于该声音档案，或档案已停用。"));
    const auto metadata = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
    const QString reference = query.value(0).toString();
    if ((!reference.isEmpty() && (metadata.value(QStringLiteral("qualityState")).toString() != QStringLiteral("approved")
            || metadata.value(QStringLiteral("referenceText")).toString().trimmed().isEmpty()
            || !QFileInfo(QDir(path_ + QStringLiteral(".assets")).filePath(reference)).isFile()))
        || (reference.isEmpty() && (metadata.value(QStringLiteral("provider")).toString() != QStringLiteral("qwen-cloud")
            || metadata.value(QStringLiteral("providerVoiceId")).toString().isEmpty())))
        return fail(error, QStringLiteral("默认参考必须已审核且文件可用；云音色需有明确绑定。"));
    query.prepare(QStringLiteral("INSERT INTO profile_defaults VALUES(?,?) ON CONFLICT(profile_id) DO UPDATE SET version_id=excluded.version_id"));
    query.addBindValue(profileId); query.addBindValue(versionId);
    return query.exec() || fail(error, query.lastError().text());
}

QString VoiceLibrary::defaultVersion(const QString &profileId, QString *error) const
{
    if (!QFileInfo::exists(path_)) return {};
    Connection connection(path_); if (!connection.open(error)) return {};
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT version FROM library")) || !query.next()) { fail(error, QStringLiteral("声音库版本缺失。")); return {}; }
    if (query.value(0).toInt() == 1) return {};
    if (query.value(0).toInt() != 2) { fail(error, QStringLiteral("不支持的声音库版本。")); return {}; }
    query.prepare(QStringLiteral("SELECT version_id FROM profile_defaults WHERE profile_id=?")); query.addBindValue(profileId);
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    return query.next() ? query.value(0).toString() : QString();
}

QJsonArray VoiceLibrary::profiles(bool includeArchived, int offset, int limit, QString *error) const
{
    QJsonArray result;
    if (!QFileInfo::exists(path_)) return result;
    Connection connection(path_); if (!connection.open(error)) return result;
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT id,name,archived FROM profiles WHERE archived=0 OR ?=1 ORDER BY name,id LIMIT ? OFFSET ?"));
    query.addBindValue(includeArchived ? 1 : 0); query.addBindValue(std::clamp(limit, 1, 200)); query.addBindValue(std::max(offset, 0));
    if (!query.exec()) { fail(error, query.lastError().text()); return result; }
    while (query.next()) result.append(QJsonObject{{QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("name"), query.value(1).toString()}, {QStringLiteral("archived"), query.value(2).toBool()}});
    return result;
}

namespace {
std::optional<QString> storeVersion(QSqlDatabase &database, const QString &path, const QString &profileId,
    const QJsonObject &metadata, const QString &referencePath, QString *error, const std::atomic<bool> *cancel,
    QStringList *createdAssets = nullptr)
{
    if (QUuid(profileId).isNull() || !validMetadata(metadata)) { fail(error, QStringLiteral("声音版本身份或元数据无效，请使用凭据引用。")); return {}; }
    QSqlQuery query(database); query.prepare(QStringLiteral("SELECT archived FROM profiles WHERE id=?")); query.addBindValue(profileId);
    if (!query.exec() || !query.next() || query.value(0).toBool()) { fail(error, QStringLiteral("声音档案不存在或已停用。")); return {}; }
    QString storedPath; QByteArray hash;
    if (!referencePath.isEmpty()) {
        bool created = false;
        const auto copied = copyAssetFile(referencePath, path + QStringLiteral(".assets"), error, cancel, &created);
        if (copied && created && createdAssets) createdAssets->append(QDir(path + QStringLiteral(".assets")).filePath(copied->first));
        if (!copied) return {};
        storedPath = copied->first; hash = copied->second;
    }
    if (cancel && cancel->load()) { fail(error, QStringLiteral("声音版本创建已取消。")); return {}; }
    if (metadata.contains(QStringLiteral("emotionPrediction"))
        && metadata.value(QStringLiteral("emotionPrediction")).toObject().value(QStringLiteral("assetSha256")).toString() != QString::fromLatin1(hash.toHex())) {
        fail(error, QStringLiteral("情绪预测与参考音频指纹不一致，请重新识别。")); return {};
    }
    if (metadata.contains(QStringLiteral("originalSha256")) && QString::fromLatin1(hash.toHex()) != metadata.value(QStringLiteral("originalSha256")).toString()) {
        fail(error, QStringLiteral("原件在复制期间发生变化，请重新整理。")); return {};
    }
    if (metadata.value(QStringLiteral("qualityState")).toString() == QStringLiteral("approved")
        || !metadata.value(QStringLiteral("quality")).toObject().isEmpty()) {
        const auto checked = QByteArray::fromHex(metadata.value(QStringLiteral("quality")).toObject().value(QStringLiteral("sha256")).toString().toLatin1());
        if (checked.size() != 32 || hash != checked) { fail(error, QStringLiteral("参考与审核指纹不一致，请重新检查。")); return {}; }
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    query.prepare(QStringLiteral("INSERT INTO voice_versions VALUES(?,?,?,?,?,?)"));
    query.addBindValue(id); query.addBindValue(profileId); query.addBindValue(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    query.addBindValue(storedPath.isNull() ? QStringLiteral("") : storedPath); query.addBindValue(hash); query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    return id;
}
}
std::optional<QString> VoiceLibrary::addVersion(const QString &profileId, const QJsonObject &metadata,
    const QString &referencePath, QString *error, const std::atomic<bool> *cancel)
{
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { fail(error, QStringLiteral("声音库正在被其他操作使用。")); return {}; }
    Connection connection(path_); if (!connection.open(error)) return {};
    return storeVersion(connection.db, path_, profileId, metadata, referencePath, error, cancel);
}
std::optional<QString> VoiceLibrary::createWithVersions(const QString &name, const QJsonArray &entries,
    QString *error, const std::atomic<bool> *cancel)
{
    if (name.trimmed().isEmpty() || name.size() > 256 || entries.isEmpty() || entries.size() > 2000) {
        fail(error, QStringLiteral("请填写音色名称并至少确认一个参考样本。")); return {};
    }
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { fail(error, QStringLiteral("声音库正在被其他操作使用。")); return {}; }
    Connection connection(path_);
    if (!connection.open(error) || !connection.db.transaction()) return {};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList createdAssets; QString defaultVersion;
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("INSERT INTO profiles(id,name) VALUES(?,?)")); query.addBindValue(id); query.addBindValue(name.trimmed());
    bool ok = query.exec();
    for (const auto &value : entries) {
        if (!ok || (cancel && cancel->load())) { ok = false; break; }
        const auto entry = value.toObject();
        const auto version = storeVersion(connection.db, path_, id, entry.value(QStringLiteral("metadata")).toObject(),
            entry.value(QStringLiteral("referencePath")).toString(), error, cancel, &createdAssets);
        if (!version) { ok = false; break; }
        if (defaultVersion.isEmpty()) defaultVersion = *version;
    }
    if (ok && !(cancel && cancel->load())) {
        query.prepare(QStringLiteral("INSERT INTO profile_defaults(profile_id,version_id) VALUES(?,?)"));
        query.addBindValue(id); query.addBindValue(defaultVersion); ok = query.exec() && connection.db.commit();
        if (ok) return id;
    }
    connection.db.rollback();
    // 只清理本次发布的未提交资产，去重复用的旧资产不在列表中。
    for (const auto &file : createdAssets) QFile::remove(file);
    if (error && error->isEmpty()) *error = cancel && cancel->load() ? QStringLiteral("创建已取消。") : query.lastError().text();
    return {};
}

std::optional<QJsonObject> VoiceLibrary::version(const QString &id, QString *error) const
{
    if (!QFileInfo::exists(path_)) return {};
    Connection connection(path_); if (!connection.open(error)) return {};
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("SELECT profile_id,metadata,reference_path,reference_sha256 FROM voice_versions WHERE id=?")); query.addBindValue(id);
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    if (!query.next()) return {};
    QJsonParseError parseError; const auto metadata = QJsonDocument::fromJson(query.value(1).toByteArray(), &parseError);
    const QString file = query.value(2).toString();
    if (!metadata.isObject() || parseError.error != QJsonParseError::NoError || !validMetadata(metadata.object())
        || (metadata.object().contains(QStringLiteral("emotionPrediction"))
            && metadata.object().value(QStringLiteral("emotionPrediction")).toObject().value(QStringLiteral("assetSha256")).toString() != QString::fromLatin1(query.value(3).toByteArray().toHex()))
        || (!file.isEmpty() && (QFileInfo(file).fileName() != file || query.value(3).toByteArray().size() != 32))) {
        fail(error, QStringLiteral("声音版本数据损坏。")); return {};
    }
    return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("voiceId"), query.value(0).toString()}, {QStringLiteral("metadata"), metadata.object()},
        {QStringLiteral("referencePath"), file.isEmpty() ? QString() : QDir(path_ + QStringLiteral(".assets")).filePath(file)},
        {QStringLiteral("referenceSha256"), QString::fromLatin1(query.value(3).toByteArray().toHex())}};
}

QJsonArray VoiceLibrary::versions(const QString &profileId, int offset, int limit, QString *error) const
{
    QJsonArray result; if (!QFileInfo::exists(path_)) return result;
    Connection connection(path_); if (!connection.open(error)) return result;
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("SELECT id,created_at FROM voice_versions WHERE profile_id=? ORDER BY created_at DESC,id LIMIT ? OFFSET ?"));
    query.addBindValue(profileId); query.addBindValue(std::clamp(limit, 1, 200)); query.addBindValue(std::max(offset, 0));
    if (!query.exec()) { fail(error, query.lastError().text()); return result; }
    while (query.next()) result.append(QJsonObject{{QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("createdAt"), QString::number(query.value(1).toLongLong())}});
    return result;
}

bool VoiceLibrary::copyVersionToProject(const QString &id, ProjectStore &project, QString *error, const std::atomic<bool> *cancel) const
{
    const auto snapshot = version(id, error);
    if (!snapshot) return fail(error, QStringLiteral("声音版本不存在或无法读取。"));
    return project.importVoiceVersion(snapshot->value(QStringLiteral("voiceId")).toString(), id,
        snapshot->value(QStringLiteral("referencePath")).toString(), QByteArray::fromHex(snapshot->value(QStringLiteral("referenceSha256")).toString().toLatin1()),
        snapshot->value(QStringLiteral("metadata")).toObject(), error, cancel);
}
}
