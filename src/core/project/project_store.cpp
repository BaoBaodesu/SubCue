#include "project/project_store.h"
#include "project/asset_file.h"
#include "tts/voice_document.h"
#include "roughcut/rough_cut_project.h"
#include "common/file_path_guard.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QCryptographicHash>
#include <QtCore/QLockFile>
#include <QtCore/QUuid>
#include <QtCore/QVariant>
#include <QtCore/QDateTime>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <algorithm>
#include <utility>
#include <limits>

namespace subcue {
namespace {
class Connection final {
public:
    explicit Connection(const QString &path)
        : name(QUuid::createUuid().toString()), db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    {
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    }
    ~Connection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    QString name;
    QSqlDatabase db;
};
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
bool validUsage(const QJsonObject &usage, QString *error)
{
    const QString state = usage.value(QStringLiteral("state")).toString();
    if (state != QStringLiteral("known") && state != QStringLiteral("unknown"))
        return fail(error, QStringLiteral("用量必须明确为已知或未知。"));
    if (state == QStringLiteral("known")) {
        for (const auto &key : {QStringLiteral("inputTokens"), QStringLiteral("outputTokens")}) {
            const auto value = usage.value(key);
            if (!value.isDouble() || value.toInteger(-1) < 0 || value.toDouble() != static_cast<double>(value.toInteger(-1)))
                return fail(error, QStringLiteral("已知用量必须提供有效输入及输出 Token。"));
        }
    }
    return true;
}
bool open(Connection &connection, QString *error)
{
    if (!connection.db.open()) return fail(error, connection.db.lastError().text());
    QSqlQuery query(connection.db);
    return query.exec(QStringLiteral("PRAGMA foreign_keys=ON")) || fail(error, query.lastError().text());
}
bool upgradeSchema(Connection &connection, QString *error)
{
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT schema_version FROM project")) || !query.next())
        return fail(error, QStringLiteral("统一工程版本缺失。"));
    const int version = query.value(0).toInt();
    if (version == 5) return true;
    if (version < 1 || version > 5) return fail(error, QStringLiteral("不支持的统一工程版本。"));
    if (version == 1 && !query.exec(QStringLiteral("CREATE TABLE assets(id TEXT PRIMARY KEY,kind TEXT NOT NULL,path TEXT NOT NULL,sha256 BLOB NOT NULL,size INTEGER NOT NULL,external INTEGER NOT NULL)")))
        return fail(error, query.lastError().text());
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS job_details(id TEXT PRIMARY KEY REFERENCES jobs(id),input_hash BLOB NOT NULL,provider TEXT NOT NULL,batch_id TEXT NOT NULL,retry_of TEXT REFERENCES jobs(id),diagnostics TEXT NOT NULL,result TEXT NOT NULL,usage TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,is_cloud INTEGER NOT NULL)")))
        return fail(error, query.lastError().text());
    if (!query.exec(QStringLiteral("SELECT jobs.id,input FROM jobs LEFT JOIN job_details ON jobs.id=job_details.id WHERE job_details.id IS NULL")))
        return fail(error, query.lastError().text());
    while (query.next()) {
        QSqlQuery detail(connection.db);
        detail.prepare(QStringLiteral("INSERT INTO job_details VALUES(?,?,'','',NULL,'{}','{}','{\"state\":\"unknown\"}',0,0,0)"));
        detail.addBindValue(query.value(0));
        detail.addBindValue(QCryptographicHash::hash(query.value(1).toByteArray(), QCryptographicHash::Sha256));
        if (!detail.exec()) return fail(error, detail.lastError().text());
    }
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS voice_versions(id TEXT PRIMARY KEY,voice_id TEXT NOT NULL,reference_asset_id TEXT REFERENCES assets(id),metadata TEXT NOT NULL)"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS voice_document(id INTEGER PRIMARY KEY CHECK(id=1),metadata TEXT NOT NULL)"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS voice_paragraphs(id TEXT PRIMARY KEY,position INTEGER NOT NULL,data TEXT NOT NULL)"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS voice_segments(id TEXT PRIMARY KEY,paragraph_id TEXT NOT NULL REFERENCES voice_paragraphs(id),position INTEGER NOT NULL,archived INTEGER NOT NULL,voice_version_id TEXT REFERENCES voice_versions(id),selected_take_id TEXT REFERENCES takes(id),data TEXT NOT NULL)"))
        || !query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS voice_segments_paragraph ON voice_segments(paragraph_id,archived,position)"))
        || !query.exec(QStringLiteral("UPDATE project SET schema_version=5"))) return fail(error, query.lastError().text());
    return true;
}

bool saveVoiceDocument(Connection &connection, const QJsonObject &document, QString *error)
{
    if (!VoiceDocument::validate(document, error)) return false;
    QSqlQuery query(connection.db);
    for (const auto &sql : {"DELETE FROM voice_segments", "DELETE FROM voice_paragraphs", "DELETE FROM voice_document"})
        if (!query.exec(QString::fromLatin1(sql))) return fail(error, query.lastError().text());
    auto metadata = document;
    metadata.remove(QStringLiteral("paragraphs")); metadata.remove(QStringLiteral("segments")); metadata.remove(QStringLiteral("archivedSegments"));
    query.prepare(QStringLiteral("INSERT INTO voice_document VALUES(1,?)")); query.addBindValue(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    if (!query.exec()) return fail(error, query.lastError().text());
    const auto paragraphs = document.value(QStringLiteral("paragraphs")).toArray();
    for (int i = 0; i < paragraphs.size(); ++i) {
        query.prepare(QStringLiteral("INSERT INTO voice_paragraphs VALUES(?,?,?)")); query.addBindValue(paragraphs[i].toObject().value(QStringLiteral("id")).toString());
        query.addBindValue(i); query.addBindValue(QJsonDocument(paragraphs[i].toObject()).toJson(QJsonDocument::Compact));
        if (!query.exec()) return fail(error, query.lastError().text());
    }
    for (const auto &key : {QStringLiteral("segments"), QStringLiteral("archivedSegments")}) {
        const auto items = document.value(key).toArray();
        for (int i = 0; i < items.size(); ++i) {
            const auto item = items[i].toObject();
            query.prepare(QStringLiteral("INSERT INTO voice_segments VALUES(?,?,?,?,NULLIF(?,''),NULLIF(?,''),?)"));
            query.addBindValue(item.value(QStringLiteral("id")).toString()); query.addBindValue(item.value(QStringLiteral("paragraphId")).toString());
            query.addBindValue(i); query.addBindValue(key == QStringLiteral("archivedSegments") ? 1 : 0);
            query.addBindValue(item.value(QStringLiteral("voiceVersionId")).toString()); query.addBindValue(item.value(QStringLiteral("selectedTake")).toObject().value(QStringLiteral("id")).toString());
            query.addBindValue(QJsonDocument(item).toJson(QJsonDocument::Compact));
            if (!query.exec()) return fail(error, query.lastError().text());
        }
    }
    return true;
}

std::optional<QJsonObject> loadVoiceDocument(Connection &connection, QString *error)
{
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT metadata FROM voice_document WHERE id=1"))) { fail(error, query.lastError().text()); return {}; }
    if (!query.next()) {
        if (!query.exec(QStringLiteral("SELECT (SELECT count(*) FROM voice_paragraphs)+(SELECT count(*) FROM voice_segments)")) || !query.next()) { fail(error, query.lastError().text()); return {}; }
        if (query.value(0).toInt() != 0) { fail(error, QStringLiteral("配音实体缺少文稿元数据，工程不完整。")); return {}; }
        return QJsonObject{};
    }
    QJsonParseError parseError;
    auto metadata = QJsonDocument::fromJson(query.value(0).toByteArray(), &parseError);
    if (!metadata.isObject() || parseError.error != QJsonParseError::NoError) { fail(error, QStringLiteral("配音文稿元数据损坏。")); return {}; }
    auto document = metadata.object();
    for (const auto &key : {QStringLiteral("paragraphs"), QStringLiteral("segments"), QStringLiteral("archivedSegments")}) {
        if (!query.exec(key == QStringLiteral("paragraphs") ? QStringLiteral("SELECT data,id FROM voice_paragraphs ORDER BY position")
            : QStringLiteral("SELECT data,id,paragraph_id,COALESCE(voice_version_id,''),COALESCE(selected_take_id,'') FROM voice_segments WHERE archived=%1 ORDER BY position").arg(key == QStringLiteral("archivedSegments") ? 1 : 0))) {
            fail(error, query.lastError().text()); return {};
        }
        QJsonArray items;
        while (query.next()) {
            const auto item = QJsonDocument::fromJson(query.value(0).toByteArray(), &parseError);
            if (!item.isObject() || parseError.error != QJsonParseError::NoError) { fail(error, QStringLiteral("配音文稿实体损坏。")); return {}; }
            if (item.object().value(QStringLiteral("id")).toString() != query.value(1).toString()
                || (key != QStringLiteral("paragraphs") && (item.object().value(QStringLiteral("paragraphId")).toString() != query.value(2).toString()
                    || item.object().value(QStringLiteral("voiceVersionId")).toString() != query.value(3).toString()
                    || item.object().value(QStringLiteral("selectedTake")).toObject().value(QStringLiteral("id")).toString() != query.value(4).toString()))) {
                fail(error, QStringLiteral("配音实体与关系引用不一致，未加载损坏工程。")); return {};
            }
            items.append(item.object());
        }
        document.insert(key, items);
    }
    return VoiceDocument::validate(document, error) ? std::optional<QJsonObject>(document) : std::nullopt;
}


}

ProjectStore::ProjectStore(QString path) : path_(std::move(path)) {}

bool ProjectStore::create(QString *error)
{
    return create(QUuid::createUuid().toString(QUuid::WithoutBraces), error);
}

bool ProjectStore::create(const QString &projectId, QString *error)
{
    if (QUuid(projectId).isNull()) return fail(error, QStringLiteral("统一工程 ID 必须是有效 UUID。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (QFileInfo::exists(path_)) return fail(error, QStringLiteral("目标工程已存在。"));
    QTemporaryFile candidate(QFileInfo(path_).absoluteDir().filePath(QStringLiteral(".subcue-project-XXXXXX")));
    if (!candidate.open()) return fail(error, candidate.errorString());
    candidate.close();
    {
        Connection connection(candidate.fileName());
        if (!open(connection, error) || !connection.db.transaction()) return false;
        QSqlQuery query(connection.db);
        for (const auto &sql : {
            "CREATE TABLE project(id TEXT PRIMARY KEY,schema_version INTEGER NOT NULL,edit_revision INTEGER NOT NULL,modules TEXT NOT NULL)",
            "CREATE TABLE jobs(id TEXT PRIMARY KEY,input TEXT NOT NULL,state TEXT NOT NULL)",
            "CREATE TABLE takes(id TEXT PRIMARY KEY,job_id TEXT NOT NULL REFERENCES jobs(id),audio_path TEXT NOT NULL)"}) {
            if (!query.exec(QString::fromLatin1(sql))) {
                connection.db.rollback();
                return fail(error, query.lastError().text());
            }
        }
        query.prepare(QStringLiteral("INSERT INTO project VALUES(?,1,0,'{}')"));
        query.addBindValue(projectId);
        if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
        if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
        if (!connection.db.commit()) return fail(error, connection.db.lastError().text());
    }
    if (!ProjectStore(candidate.fileName()).load(error)) return false;
    QFile source(candidate.fileName());
    QSaveFile destination(path_);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("无法发布新工程。"));
    while (!source.atEnd()) {
        const auto bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || destination.write(bytes) != bytes.size())
            return fail(error, QStringLiteral("新工程写入失败。"));
    }
    if (QFileInfo::exists(path_)) return fail(error, QStringLiteral("目标工程已存在。"));
    return destination.commit() || fail(error, destination.errorString());
}

std::optional<ProjectStore::Snapshot> ProjectStore::load(QString *error) const
{
    if (!QFileInfo::exists(path_)) { fail(error, QStringLiteral("工程文件不存在。")); return {}; }
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return {};
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT id,edit_revision,modules,schema_version FROM project")) || !query.next()) {
        fail(error, QStringLiteral("统一工程数据不完整。")); return {};
    }
    QJsonParseError parseError;
    const auto modules = QJsonDocument::fromJson(query.value(2).toByteArray(), &parseError);
    const int version = query.value(3).toInt();
    if ((version < 1 || version > 5) || query.value(0).toString().isEmpty()
        || parseError.error != QJsonParseError::NoError || !modules.isObject()) {
        fail(error, QStringLiteral("工程版本或模块数据无效。")); return {};
    }
    Snapshot snapshot{query.value(0).toString(), query.value(1).toLongLong(), modules.object()};
    if (version >= 2
        && !query.exec(QStringLiteral("SELECT id,kind,path,sha256,size,external FROM assets LIMIT 0"))) {
        fail(error, QStringLiteral("工程资产清单缺失或无效。")); return {};
    }
    if (version >= 3 && !query.exec(QStringLiteral("SELECT id,input_hash,provider,batch_id,retry_of,diagnostics,result,usage,created_at,updated_at,is_cloud FROM job_details LIMIT 0"))) {
        fail(error, QStringLiteral("工程任务详情缺失或无效。")); return {};
    }
    if (version >= 4 && !query.exec(QStringLiteral("SELECT id,voice_id,reference_asset_id,metadata FROM voice_versions LIMIT 0"))) {
        fail(error, QStringLiteral("项目声音版本表缺失或无效。")); return {};
    }
    if (version == 5) {
        const auto voice = loadVoiceDocument(connection, error); if (!voice) return {};
        if (!voice->isEmpty()) snapshot.modules.insert(QStringLiteral("voiceStudio"), *voice);
    }
    if (!connection.db.commit()) { fail(error, connection.db.lastError().text()); return {}; }
    return snapshot;
}

bool ProjectStore::save(const Snapshot &snapshot, QString *error, qint64 *committedRevision,
                        const std::atomic<bool> *cancel)
{
    if (snapshot.revision < 0 || snapshot.revision == std::numeric_limits<qint64>::max())
        return fail(error, QStringLiteral("工程保存版本超出有效范围。"));
    if (cancel && cancel->load()) return fail(error, QStringLiteral("工程保存已取消。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    auto modules = snapshot.modules;
    // voice 是既有未知模块可使用的名称，正式文稿使用独立键，不能覆盖旧元数据。
    if (modules.contains(QStringLiteral("voiceStudio"))) {
        if (!modules.value(QStringLiteral("voiceStudio")).isObject() || !saveVoiceDocument(connection, modules.value(QStringLiteral("voiceStudio")).toObject(), error)) { connection.db.rollback(); return false; }
        modules.remove(QStringLiteral("voiceStudio"));
    }
    query.prepare(QStringLiteral("UPDATE project SET modules=?,edit_revision=edit_revision+1 WHERE id=? AND edit_revision=? AND schema_version=5"));
    query.addBindValue(QJsonDocument(modules).toJson(QJsonDocument::Compact));
    query.addBindValue(snapshot.projectId);
    query.addBindValue(snapshot.revision);
    if (!query.exec() || query.numRowsAffected() != 1) {
        connection.db.rollback();
        return fail(error, QStringLiteral("工程已被修改，保存未覆盖现有数据。"));
    }
    // 提交前取消回滚；事务一旦成功提交，必须按成功回传，不能伪报取消。
    if (cancel && cancel->load()) {
        connection.db.rollback();
        return fail(error, QStringLiteral("工程保存已取消。"));
    }
    if (!connection.db.commit()) return fail(error, connection.db.lastError().text());
    if (committedRevision) *committedRevision = snapshot.revision + 1;
    return true;
}

bool ProjectStore::enqueue(const QString &jobId, const QJsonObject &input, QString *error)
{
    return enqueue(jobId, input, JobOptions{}, error);
}

bool ProjectStore::enqueue(const QString &jobId, const QJsonObject &input, const JobOptions &options, QString *error)
{
    if (jobId.isEmpty() || input.isEmpty()) return fail(error, QStringLiteral("生成任务缺少输入。"));
    if (!options.inputHash.isEmpty() && options.inputHash.size() != 32) return fail(error, QStringLiteral("生成输入指纹无效。"));
    if (options.cloud && options.provider.isEmpty()) return fail(error, QStringLiteral("云端任务缺少 Provider 标识。"));
    if (!options.retryOf.isEmpty() && !options.manualRetry) return fail(error, QStringLiteral("恢复任务必须由用户手动确认，不自动重发。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    QString retryOf = options.retryOf;
    if (options.cloud && options.inputHash.size() == 32) {
        query.prepare(QStringLiteral("SELECT jobs.id FROM jobs JOIN job_details ON job_details.id=jobs.id WHERE provider=? AND input_hash=? AND (state='outcome_unknown' OR (state IN ('failed','canceled','interrupted') AND json_extract(usage,'$.state')='known' AND (json_extract(usage,'$.inputTokens')>0 OR json_extract(usage,'$.outputTokens')>0))) ORDER BY jobs.rowid DESC LIMIT 1"));
        query.addBindValue(options.provider); query.addBindValue(options.inputHash);
        if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
        if (query.next()) {
            if (!options.manualRetry) { connection.db.rollback(); return fail(error, QStringLiteral("相同输入存在不确定结果或已计费但未取得音频；请核对记录，并明确确认重新付费请求。")); }
            if (retryOf.isEmpty()) retryOf = query.value(0).toString();
        }
    }
    if (!retryOf.isEmpty()) {
        query.prepare(QStringLiteral("SELECT state FROM jobs WHERE id=?")); query.addBindValue(retryOf);
        if (!query.exec() || !query.next() || query.value(0).toString() == QStringLiteral("queued")
            || query.value(0).toString() == QStringLiteral("running") || query.value(0).toString() == QStringLiteral("validating")) {
            connection.db.rollback(); return fail(error, QStringLiteral("重试来源不存在或仍在执行。"));
        }
    }
    const QByteArray encoded = QJsonDocument(input).toJson(QJsonDocument::Compact);
    query.prepare(QStringLiteral("INSERT INTO jobs VALUES(?,?,'queued')"));
    query.addBindValue(jobId);
    query.addBindValue(encoded);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO job_details VALUES(?,?,?,?,?,'{}','{}','{\"state\":\"unknown\"}',?,?,?)"));
    query.addBindValue(jobId);
    query.addBindValue(options.inputHash.isEmpty() ? QCryptographicHash::hash(encoded, QCryptographicHash::Sha256) : options.inputHash);
    query.addBindValue(options.provider.isEmpty() ? QStringLiteral("") : options.provider);
    query.addBindValue(options.batchId.isEmpty() ? QStringLiteral("") : options.batchId);
    query.addBindValue(retryOf.isEmpty() ? QVariant() : QVariant(retryOf));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.addBindValue(now); query.addBindValue(now); query.addBindValue(options.cloud ? 1 : 0);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

QString ProjectStore::jobStateName(JobState state)
{
    switch (state) {
    case JobState::Queued: return QStringLiteral("queued");
    case JobState::Running: return QStringLiteral("running");
    case JobState::Validating: return QStringLiteral("validating");
    case JobState::Completed: return QStringLiteral("completed");
    case JobState::Failed: return QStringLiteral("failed");
    case JobState::Canceled: return QStringLiteral("canceled");
    case JobState::Interrupted: return QStringLiteral("interrupted");
    case JobState::OutcomeUnknown: return QStringLiteral("outcome_unknown");
    }
    return {};
}

bool ProjectStore::transitionJob(const QString &jobId, JobState expected, JobState next,
    const QJsonObject &diagnostics, QString *error)
{
    const bool allowed = (expected == JobState::Queued && (next == JobState::Running || next == JobState::Canceled
        || next == JobState::Failed || next == JobState::Interrupted))
        || (expected == JobState::Running && (next == JobState::Canceled || next == JobState::Failed
            || next == JobState::Interrupted || next == JobState::OutcomeUnknown))
        || (expected == JobState::Validating && (next == JobState::Canceled || next == JobState::Failed || next == JobState::Interrupted));
    if (!allowed) return fail(error, QStringLiteral("任务状态转换无效，成功结果必须通过音频提交入口。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("UPDATE jobs SET state=? WHERE id=? AND state=?"));
    query.addBindValue(jobStateName(next)); query.addBindValue(jobId); query.addBindValue(jobStateName(expected));
    if (!query.exec() || query.numRowsAffected() != 1) {
        connection.db.rollback(); return fail(error, QStringLiteral("任务状态已变化，未覆盖其它操作的结果。"));
    }
    query.prepare(QStringLiteral("UPDATE job_details SET diagnostics=?,updated_at=? WHERE id=?"));
    query.addBindValue(QJsonDocument(diagnostics).toJson(QJsonDocument::Compact));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

bool ProjectStore::recordJobResult(const QString &jobId, const QJsonObject &result, const QJsonObject &usage, QString *error)
{
    if (!validUsage(usage, error)) return false;
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT usage FROM job_details WHERE id=?")); query.addBindValue(jobId);
    if (!query.exec() || !query.next()) { connection.db.rollback(); return fail(error, QStringLiteral("任务不存在。")); }
    const auto existingUsage = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    if (existingUsage.value(QStringLiteral("state")).toString() == QStringLiteral("known") && existingUsage != usage) {
        connection.db.rollback(); return fail(error, QStringLiteral("生成结果的用量与已记录用量不一致。"));
    }
    query.prepare(QStringLiteral("UPDATE jobs SET state='validating' WHERE id=? AND state='running'")); query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) { connection.db.rollback(); return fail(error, QStringLiteral("任务已取消、完成或尚未运行，结果未覆盖当前状态。")); }
    query.prepare(QStringLiteral("UPDATE job_details SET result=?,usage=?,updated_at=? WHERE id=?"));
    query.addBindValue(QJsonDocument(result).toJson(QJsonDocument::Compact));
    query.addBindValue(QJsonDocument(usage).toJson(QJsonDocument::Compact));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

bool ProjectStore::recordJobUsage(const QString &jobId, const QJsonObject &usage, QString *error)
{
    if (!validUsage(usage, error)) return false;
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT usage FROM job_details WHERE id=?")); query.addBindValue(jobId);
    if (!query.exec() || !query.next()) { connection.db.rollback(); return fail(error, QStringLiteral("任务不存在，无法补记用量。")); }
    const auto existing = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    if (existing.value(QStringLiteral("state")).toString() == QStringLiteral("known")) {
        connection.db.rollback();
        return existing == usage || fail(error, QStringLiteral("已知用量不可被后来的未知或不同值覆盖。"));
    }
    query.prepare(QStringLiteral("UPDATE job_details SET usage=?,updated_at=? WHERE id=?"));
    query.addBindValue(QJsonDocument(usage).toJson(QJsonDocument::Compact));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

bool ProjectStore::finish(const QString &jobId, const QString &takeId, const QString &audioPath, QString *error)
{
    return finish(jobId, takeId, audioPath, error, {}, nullptr);
}

bool ProjectStore::finish(const QString &jobId, const QString &takeId, const QString &audioPath,
    QString *error, const QByteArray &expectedSha256, const std::atomic<bool> *cancel)
{
    if ((!expectedSha256.isEmpty() && expectedSha256.size() != 32) || (cancel && cancel->load()))
        return fail(error, QStringLiteral("音频提交指纹无效或已取消。"));
    if (takeId.isEmpty() || !QFileInfo(audioPath).isFile() || QFileInfo(audioPath).size() <= 0)
        return fail(error, QStringLiteral("生成音频不存在或为空。"));
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("UPDATE jobs SET state='completed' WHERE id=? AND (state='validating' OR (state='queued' AND EXISTS(SELECT 1 FROM job_details WHERE job_details.id=jobs.id AND provider='')))"));
    query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        connection.db.rollback(); return fail(error, QStringLiteral("任务已取消、完成或不存在。"));
    }
    const auto copied = copyAssetFile(audioPath, path_ + QStringLiteral(".assets"), error, cancel);
    if (!copied) { connection.db.rollback(); return false; }
    if ((!expectedSha256.isEmpty() && copied->second != expectedSha256) || (cancel && cancel->load())) {
        connection.db.rollback(); return fail(error, QStringLiteral("音频与质检指纹不一致或提交已取消。"));
    }
    query.prepare(QStringLiteral("INSERT INTO takes VALUES(?,?,?)"));
    query.addBindValue(takeId); query.addBindValue(jobId);
    query.addBindValue(QFileInfo(path_).fileName() + QStringLiteral(".assets/") + copied->first);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    query.prepare(QStringLiteral("UPDATE job_details SET updated_at=? WHERE id=?"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(jobId);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

QJsonArray ProjectStore::voiceBindings(const QStringList &versionIds, QString *error) const
{
    if (versionIds.isEmpty() || !load(error)) return {};
    const QSet<QString> wanted(versionIds.begin(), versionIds.end());
    Connection connection(path_); if (!open(connection, error)) return {};
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT id,metadata FROM voice_versions"))) { fail(error, query.lastError().text()); return {}; }
    QJsonArray result;
    while (query.next()) {
        if (!wanted.contains(query.value(0).toString())) continue;
        const auto metadata = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
        if (metadata.value(QStringLiteral("provider")) != QStringLiteral("qwen-cloud")) continue;
        QJsonObject row{{QStringLiteral("versionId"), query.value(0).toString()}};
        for (const auto &field : {QStringLiteral("providerVoiceId"), QStringLiteral("cloudAccountId"), QStringLiteral("model"), QStringLiteral("region"), QStringLiteral("businessSpace")}) row.insert(field, metadata.value(field));
        result.append(row);
    }
    return result;
}
QJsonObject ProjectStore::voiceGenerationStates(const QJsonObject &document, QString *error, const std::atomic<bool> *cancel) const
{
    if (!load(error)) return {};
    QHash<QString, QJsonObject> segments;
    for (const auto &value : document.value(QStringLiteral("segments")).toArray()) segments.insert(value.toObject().value(QStringLiteral("id")).toString(), value.toObject());
    Connection connection(path_); if (!open(connection, error)) return {};
    QSqlQuery query(connection.db); query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT jobs.input,jobs.state,takes.audio_path FROM jobs LEFT JOIN takes ON takes.job_id=jobs.id ORDER BY jobs.rowid DESC"))) { fail(error, query.lastError().text()); return {}; }
    QJsonObject result; QSet<QString> matched;
    // 流式读取历史，只保留每个当前句的一个状态；不加载音频或完整任务历史。
    while ((!cancel || !cancel->load()) && matched.size() < segments.size() && query.next()) {
        const auto input = QJsonDocument::fromJson(query.value(0).toByteArray()).object(); const auto id = input.value(QStringLiteral("segmentId")).toString();
        if (!segments.contains(id) || matched.contains(id)) continue;
        QJsonObject frozen{{QStringLiteral("spoken"), input.value(QStringLiteral("spokenText"))}, {QStringLiteral("manualSpoken"), true}};
        for (const auto &field : {QStringLiteral("voiceVersionId"), QStringLiteral("parameters"), QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")}) frozen.insert(field, input.value(field));
        const auto key = VoiceDocument::inputKey(VoiceDocument::empty(), frozen);
        const bool current = key == VoiceDocument::inputKey(document, segments.value(id)) && input.value(QStringLiteral("inputVersion")) == segments.value(id).value(QStringLiteral("inputVersion"));
        const auto state = query.value(1).toString();
        if (!current && (result.contains(id) || state != QStringLiteral("completed"))) continue;
        const bool available = state == QStringLiteral("completed") && !query.value(2).toString().isEmpty() && QFileInfo::exists(resolveAudioPath(query.value(2).toString()));
        result.insert(id, QJsonObject{{QStringLiteral("inputKey"), QString::fromLatin1(key.toHex())}, {QStringLiteral("inputVersion"), input.value(QStringLiteral("inputVersion"))},
            {QStringLiteral("state"), !current ? QStringLiteral("stale") : available ? QStringLiteral("candidate") : state == QStringLiteral("failed") || state == QStringLiteral("outcome_unknown") || state == QStringLiteral("completed") ? QStringLiteral("failed") : QStringLiteral("pending")}});
        if (current) matched.insert(id);
    }
    return result;
}
QJsonArray ProjectStore::segmentTakes(const QString &segmentId, int offset, int limit, QString *error) const
{
    QJsonArray result;
    if (QUuid(segmentId).isNull() || !load(error)) return result;
    Connection connection(path_); if (!open(connection, error)) return result;
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT takes.id,takes.audio_path,jobs.input,job_details.result FROM takes JOIN jobs ON jobs.id=takes.job_id JOIN job_details ON jobs.id=job_details.id WHERE json_extract(jobs.input,'$.segmentId')=? ORDER BY takes.rowid DESC LIMIT ? OFFSET ?"));
    query.addBindValue(segmentId); query.addBindValue(std::clamp(limit, 1, 50)); query.addBindValue(std::max(offset, 0));
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    while (query.next()) result.append(QJsonObject{{QStringLiteral("id"), query.value(0).toString()},
        {QStringLiteral("audioPath"), resolveAudioPath(query.value(1).toString())}, {QStringLiteral("input"), QJsonDocument::fromJson(query.value(2).toByteArray()).object()},
        {QStringLiteral("result"), QJsonDocument::fromJson(query.value(3).toByteArray()).object()}});
    return result;
}

bool ProjectStore::cancel(const QString &jobId, QString *error)
{
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    if (!load(error)) return false;
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("UPDATE jobs SET state='canceled' WHERE id=? AND state IN ('queued','running','validating')"));
    query.addBindValue(jobId);
    if (!query.exec() || query.numRowsAffected() != 1) { connection.db.rollback(); return fail(error, QStringLiteral("任务已完成、取消或不存在。")); }
    query.prepare(QStringLiteral("UPDATE job_details SET updated_at=? WHERE id=?"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch()); query.addBindValue(jobId);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

QJsonArray ProjectStore::jobs(int offset, int limit, QString *error, const QString &batchId) const
{
    QJsonArray result;
    if (!load(error)) return result;
    Connection connection(path_);
    if (!open(connection, error)) return result;
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT schema_version FROM project")) || !query.next()) {
        fail(error, query.lastError().text()); return {};
    }
    const bool details = query.value(0).toInt() >= 3;
    if (!details && !batchId.isEmpty()) return {};
    query.prepare(details ? QStringLiteral("SELECT jobs.id,input,state,input_hash,provider,batch_id,retry_of,diagnostics,result,usage,created_at,updated_at,is_cloud FROM jobs LEFT JOIN job_details ON jobs.id=job_details.id WHERE (?='' OR batch_id=?) ORDER BY jobs.rowid DESC LIMIT ? OFFSET ?")
        : QStringLiteral("SELECT id,input,state,NULL,'','',NULL,'{}','{}','{\"state\":\"unknown\"}',0,0,0 FROM jobs ORDER BY rowid DESC LIMIT ? OFFSET ?"));
    if (details) {
        const auto filter = batchId.isEmpty() ? QStringLiteral("") : batchId;
        query.addBindValue(filter); query.addBindValue(filter);
    }
    query.addBindValue(std::clamp(limit, 1, 200)); query.addBindValue(std::max(offset, 0));
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    while (query.next()) {
        QJsonParseError parseError;
        const auto input = QJsonDocument::fromJson(query.value(1).toByteArray(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !input.isObject()) {
            fail(error, QStringLiteral("任务输入损坏：%1").arg(query.value(0).toString())); return {};
        }
        if (details && query.value(3).toByteArray().size() != 32) {
            fail(error, QStringLiteral("任务详情或输入指纹损坏：%1").arg(query.value(0).toString())); return {};
        }
        for (int column = 7; column <= 9; ++column) {
            const auto metadata = QJsonDocument::fromJson(query.value(column).toByteArray(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !metadata.isObject()) {
                fail(error, QStringLiteral("任务诊断、结果或用量损坏：%1").arg(query.value(0).toString())); return {};
            }
        }
        result.append(QJsonObject{{QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("input"), input.object()},
            {QStringLiteral("state"), query.value(2).toString()},
            {QStringLiteral("inputHash"), QString::fromLatin1((details ? query.value(3).toByteArray()
                : QCryptographicHash::hash(query.value(1).toByteArray(), QCryptographicHash::Sha256)).toHex())},
            {QStringLiteral("provider"), query.value(4).toString()}, {QStringLiteral("batchId"), query.value(5).toString()},
            {QStringLiteral("retryOf"), query.value(6).toString()},
            {QStringLiteral("diagnostics"), QJsonDocument::fromJson(query.value(7).toByteArray()).object()},
            {QStringLiteral("result"), QJsonDocument::fromJson(query.value(8).toByteArray()).object()},
            {QStringLiteral("usage"), QJsonDocument::fromJson(query.value(9).toByteArray()).object()},
            {QStringLiteral("createdAt"), query.value(10).toLongLong()}, {QStringLiteral("updatedAt"), query.value(11).toLongLong()},
            {QStringLiteral("cloud"), query.value(12).toBool()}});
    }
    return result;
}

std::optional<int> ProjectStore::recoverJobs(QString *error)
{
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { fail(error, QStringLiteral("工程正在被其他操作使用。")); return {}; }
    if (!load(error)) return {};
    Connection connection(path_);
    if (!open(connection, error)) return {};
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM jobs WHERE state IN ('queued','running','validating')")) || !query.next()) {
        fail(error, query.lastError().text()); return {};
    }
    const int count = query.value(0).toInt();
    if (!count) return 0;
    if (!connection.db.transaction()) { fail(error, connection.db.lastError().text()); return {}; }
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return {}; }
    if (!query.exec(QStringLiteral("SELECT jobs.id,state,is_cloud,diagnostics FROM jobs JOIN job_details ON jobs.id=job_details.id WHERE state IN ('queued','running','validating')"))) {
        connection.db.rollback(); fail(error, query.lastError().text()); return {};
    }
    while (query.next()) {
        const QString previous = query.value(1).toString();
        auto diagnostics = QJsonDocument::fromJson(query.value(3).toByteArray()).object();
        diagnostics.insert(QStringLiteral("recovery"), QJsonObject{{QStringLiteral("previousState"), previous},
            {QStringLiteral("message"), QStringLiteral("执行中断，等待用户决定继续原输入或重新生成。")}});
        QSqlQuery update(connection.db);
        update.prepare(QStringLiteral("UPDATE jobs SET state=? WHERE id=? AND state=?"));
        update.addBindValue(previous == QStringLiteral("running") && query.value(2).toBool()
            ? QStringLiteral("outcome_unknown") : QStringLiteral("interrupted"));
        update.addBindValue(query.value(0)); update.addBindValue(previous);
        if (!update.exec() || update.numRowsAffected() != 1) { connection.db.rollback(); fail(error, update.lastError().text()); return {}; }
        update.prepare(QStringLiteral("UPDATE job_details SET diagnostics=?,updated_at=? WHERE id=?"));
        update.addBindValue(QJsonDocument(diagnostics).toJson(QJsonDocument::Compact));
        update.addBindValue(QDateTime::currentMSecsSinceEpoch()); update.addBindValue(query.value(0));
        if (!update.exec()) { connection.db.rollback(); fail(error, update.lastError().text()); return {}; }
    }
    if (!connection.db.commit()) { fail(error, connection.db.lastError().text()); return {}; }
    return count;
}

QJsonArray ProjectStore::history(int offset, int limit, QString *error) const
{
    QJsonArray result;
    if (!load(error)) return result;
    Connection connection(path_);
    if (!open(connection, error)) return result;
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT takes.id,job_id,audio_path,input FROM takes JOIN jobs ON jobs.id=takes.job_id ORDER BY takes.rowid DESC LIMIT ? OFFSET ?"));
    query.addBindValue(std::clamp(limit, 1, 200)); query.addBindValue(std::max(offset, 0));
    if (!query.exec()) { fail(error, query.lastError().text()); return result; }
    while (query.next()) result.append(QJsonObject{
        {QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("jobId"), query.value(1).toString()},
        {QStringLiteral("audioPath"), query.value(2).toString()},
        {QStringLiteral("input"), QJsonDocument::fromJson(query.value(3).toByteArray()).object()}});
    return result;
}

std::optional<QString> ProjectStore::importAsset(const QString &sourcePath, const QString &kind,
    QString *error, const std::atomic<bool> *cancel, const QByteArray &expectedSha256)
{
    if (!expectedSha256.isEmpty() && expectedSha256.size() != 32) {
        fail(error, QStringLiteral("资产内容指纹无效。")); return {};
    }
    if (kind != QStringLiteral("media") && kind != QStringLiteral("reference")
        && kind != QStringLiteral("derived")) {
        fail(error, QStringLiteral("不支持的项目资产类型。")); return {};
    }
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) { fail(error, QStringLiteral("工程正在被其他操作使用。")); return {}; }
    if (!load(error)) return {};
    QString storedPath = QFileInfo(sourcePath).absoluteFilePath();
    QByteArray hash;
    if (kind == QStringLiteral("media")) {
        if (QFileInfo::exists(storedPath)) {
            hash = RoughCutProjectSerializer::mediaSha256(storedPath, error, cancel);
            if (hash.size() != 32 || QFileInfo(storedPath).size() <= 0) return {};
        } else {
            // 外部媒体允许离线，未知指纹保持空值，完整备份仍会要求文件可读。
            hash = expectedSha256.isEmpty() ? QByteArray("", 0) : expectedSha256;
        }
    } else {
        const auto copied = copyAssetFile(storedPath, path_ + QStringLiteral(".assets"), error, cancel);
        if (!copied) return {};
        storedPath = QFileInfo(path_).fileName() + QStringLiteral(".assets/") + copied->first;
        hash = copied->second;
    }
    if (!expectedSha256.isEmpty() && hash != expectedSha256) {
        fail(error, QStringLiteral("资产内容与原工程记录不一致，未提交引用。")); return {};
    }
    if (cancel && cancel->load()) { fail(error, QStringLiteral("资产处理已取消。")); return {}; }
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return {};
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return {}; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT id,path,external FROM assets WHERE kind=? AND sha256=COALESCE(?,X'')"));
    query.addBindValue(kind); query.addBindValue(hash);
    if (!query.exec()) { connection.db.rollback(); fail(error, query.lastError().text()); return {}; }
    while (query.next()) {
        const QString existingPath = resolveAudioPath(query.value(1).toString());
        const bool samePath = sameFilePath(existingPath, resolveAudioPath(storedPath));
        if (!samePath && (query.value(2).toBool() || !QFileInfo::exists(existingPath))) continue;
        if (!samePath && RoughCutProjectSerializer::mediaSha256(existingPath, error, cancel) != hash) {
            connection.db.rollback(); return {};
        }
        const QString id = query.value(0).toString();
        query.finish();
        if (!connection.db.commit()) { fail(error, connection.db.lastError().text()); return {}; }
        return id;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    query.prepare(QStringLiteral("INSERT INTO assets VALUES(?,?,?,COALESCE(?,X''),?,?)"));
    query.addBindValue(id); query.addBindValue(kind); query.addBindValue(storedPath);
    query.addBindValue(hash); query.addBindValue(QFileInfo(resolveAudioPath(storedPath)).size());
    query.addBindValue(kind == QStringLiteral("media") ? 1 : 0);
    if (!query.exec()) { connection.db.rollback(); fail(error, query.lastError().text()); return {}; }
    if (cancel && cancel->load()) {
        connection.db.rollback(); fail(error, QStringLiteral("资产处理已取消。")); return {};
    }
    if (!connection.db.commit()) { fail(error, connection.db.lastError().text()); return {}; }
    return id;
}

bool ProjectStore::importVoiceVersion(const QString &voiceId, const QString &versionId,
    const QString &referencePath, const QByteArray &referenceHash, const QJsonObject &metadata,
    QString *error, const std::atomic<bool> *cancel)
{
    if (QUuid(voiceId).isNull() || QUuid(versionId).isNull()
        || (referencePath.isEmpty() ? !referenceHash.isEmpty() : referenceHash.size() != 32)
        || metadata.isEmpty()) return fail(error, QStringLiteral("声音版本身份、参考指纹或元数据无效。"));
    if (!referencePath.isEmpty() && metadata.value(QStringLiteral("qualityState")).toString() == QStringLiteral("approved")
        && QByteArray::fromHex(metadata.value(QStringLiteral("quality")).toObject().value(QStringLiteral("sha256")).toString().toLatin1()) != referenceHash)
        return fail(error, QStringLiteral("审核结果与声音参考指纹不一致。"));
    if (!load(error)) return false;
    const auto assetId = referencePath.isEmpty() ? std::optional<QString>(QString())
        : importAsset(referencePath, QStringLiteral("reference"), error, cancel, referenceHash);
    if (!assetId) return false;
    QLockFile lock(path_ + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return fail(error, QStringLiteral("工程正在被其他操作使用。"));
    Connection connection(path_);
    if (!open(connection, error) || !connection.db.transaction()) return false;
    if (!upgradeSchema(connection, error)) { connection.db.rollback(); return false; }
    QSqlQuery query(connection.db);
    query.prepare(QStringLiteral("SELECT voice_id,reference_asset_id,metadata FROM voice_versions WHERE id=?")); query.addBindValue(versionId);
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    if (query.next()) {
        const bool same = query.value(0).toString() == voiceId && query.value(1).toString() == *assetId
            && QJsonDocument::fromJson(query.value(2).toByteArray()).object() == metadata;
        connection.db.rollback();
        return same || fail(error, QStringLiteral("声音版本不可覆盖，请创建新的版本 UUID。"));
    }
    if (cancel && cancel->load()) { connection.db.rollback(); return fail(error, QStringLiteral("声音复制已取消。")); }
    query.prepare(QStringLiteral("INSERT INTO voice_versions VALUES(?,?,?,?)"));
    query.addBindValue(versionId); query.addBindValue(voiceId); query.addBindValue(assetId->isEmpty() ? QVariant() : QVariant(*assetId));
    query.addBindValue(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    if (!query.exec()) { connection.db.rollback(); return fail(error, query.lastError().text()); }
    return connection.db.commit() || fail(error, connection.db.lastError().text());
}

std::optional<QJsonObject> ProjectStore::voiceVersion(const QString &versionId, QString *error) const
{
    if (!load(error)) return {};
    Connection connection(path_);
    if (!open(connection, error)) return {};
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT schema_version FROM project")) || !query.next()) { fail(error, query.lastError().text()); return {}; }
    if (query.value(0).toInt() < 4) return {};
    query.prepare(QStringLiteral("SELECT voice_id,reference_asset_id,metadata,sha256,path FROM voice_versions LEFT JOIN assets ON assets.id=reference_asset_id WHERE voice_versions.id=?"));
    query.addBindValue(versionId);
    if (!query.exec()) { fail(error, query.lastError().text()); return {}; }
    if (!query.next()) return {};
    QJsonParseError parseError;
    const auto metadata = QJsonDocument::fromJson(query.value(2).toByteArray(), &parseError);
    if (!metadata.isObject() || parseError.error != QJsonParseError::NoError
        || (!query.value(1).isNull() && query.value(3).toByteArray().size() != 32)) {
        fail(error, QStringLiteral("项目声音版本损坏。")); return {};
    }
    return QJsonObject{{QStringLiteral("id"), versionId}, {QStringLiteral("voiceId"), query.value(0).toString()},
        {QStringLiteral("referenceAssetId"), query.value(1).toString()}, {QStringLiteral("metadata"), metadata.object()},
        {QStringLiteral("referenceSha256"), QString::fromLatin1(query.value(3).toByteArray().toHex())},
        {QStringLiteral("referencePath"), query.value(1).isNull() ? QString() : resolveAudioPath(query.value(4).toString())}};
}

QJsonArray ProjectStore::assets(QString *error) const
{
    QJsonArray result;
    if (!load(error)) return result;
    Connection connection(path_);
    if (!open(connection, error)) return result;
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT schema_version FROM project")) || !query.next()) {
        fail(error, query.lastError().text()); return {};
    }
    if (query.value(0).toInt() >= 2) {
        if (!query.exec(QStringLiteral("SELECT id,kind,path,sha256,size,external FROM assets ORDER BY rowid"))) {
            fail(error, query.lastError().text()); return {};
        }
        while (query.next()) result.append(QJsonObject{
            {QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("kind"), query.value(1).toString()},
            {QStringLiteral("path"), query.value(2).toString()},
            {QStringLiteral("sha256"), QString::fromLatin1(query.value(3).toByteArray().toHex())},
            {QStringLiteral("size"), query.value(4).toLongLong()}, {QStringLiteral("external"), query.value(5).toBool()}});
    }
    // 既有 Take 路径保持唯一权威来源，清单只投影引用，不重复存储路径。
    if (!query.exec(QStringLiteral("SELECT id,audio_path FROM takes ORDER BY rowid"))) {
        fail(error, query.lastError().text()); return {};
    }
    while (query.next()) result.append(QJsonObject{
        {QStringLiteral("id"), query.value(0).toString()}, {QStringLiteral("kind"), QStringLiteral("generated")},
        {QStringLiteral("path"), query.value(1).toString()}});
    return result;
}

bool ProjectStore::backup(const QString &destination, QString *error, BackupMode mode,
    const std::atomic<bool> *cancel) const
{
    if (cancel && cancel->load()) return fail(error, QStringLiteral("备份已取消。"));
    if (QFileInfo::exists(destination) || QFileInfo::exists(destination + QStringLiteral(".assets")))
        return fail(error, QStringLiteral("备份目标已存在。"));
    if (!load(error)) return false;
    QTemporaryDir staging(QFileInfo(destination).absoluteDir().filePath(QStringLiteral(".subcue-backup-XXXXXX")));
    if (!staging.isValid()) return fail(error, QStringLiteral("无法创建备份临时目录。"));
    const QString databasePath = staging.filePath(QStringLiteral("project.sqlite"));
    {
        Connection connection(path_);
        if (!open(connection, error)) return false;
        QSqlQuery query(connection.db);
        query.prepare(QStringLiteral("VACUUM INTO ?")); query.addBindValue(databasePath);
        if (!query.exec()) return fail(error, query.lastError().text());
    }
    if (!QDir().mkdir(staging.filePath(QStringLiteral("assets"))))
        return fail(error, QStringLiteral("无法创建备份资产目录。"));
    {
        Connection connection(databasePath);
        if (!open(connection, error) || !connection.db.transaction()) return false;
        QSqlQuery query(connection.db);
        if (!query.exec(QStringLiteral("SELECT id,audio_path FROM takes")))
            return fail(error, query.lastError().text());
        while (query.next()) {
            const auto copied = copyAssetFile(resolveAudioPath(query.value(1).toString()),
                staging.filePath(QStringLiteral("assets")), error, cancel);
            if (!copied) return false;
            const QString storedPath = query.value(1).toString();
            const QString digest = QFileInfo(storedPath).completeBaseName();
            if (storedPath.startsWith(QFileInfo(path_).fileName() + QStringLiteral(".assets/"))
                && digest.size() == 64 && QByteArray::fromHex(digest.toLatin1()) != copied->second)
                return fail(error, QStringLiteral("生成音频内容变化，备份未发布。"));
            QSqlQuery update(connection.db);
            update.prepare(QStringLiteral("UPDATE takes SET audio_path=? WHERE id=?"));
            update.addBindValue(QFileInfo(destination).fileName() + QStringLiteral(".assets/") + copied->first);
            update.addBindValue(query.value(0));
            if (!update.exec()) return fail(error, update.lastError().text());
        }
        if (!query.exec(QStringLiteral("SELECT schema_version FROM project")) || !query.next())
            return fail(error, query.lastError().text());
        if (query.value(0).toInt() >= 2) {
            if (!query.exec(QStringLiteral("SELECT id,kind,path,sha256,size,external FROM assets")))
                return fail(error, query.lastError().text());
            while (query.next()) {
                QString storedPath = resolveAudioPath(query.value(2).toString());
                QByteArray digest = query.value(3).toByteArray();
                qint64 size = query.value(4).toLongLong();
                const bool external = mode == BackupMode::ExternalMediaReferences
                    && query.value(1).toString() == QStringLiteral("media") && query.value(5).toBool();
                if (!external) {
                    const auto copied = copyAssetFile(storedPath, staging.filePath(QStringLiteral("assets")), error, cancel);
                    if (!copied) return false;
                    if (!query.value(3).toByteArray().isEmpty() && copied->second != query.value(3).toByteArray())
                        return fail(error, QStringLiteral("项目资产内容变化，备份未发布：%1").arg(storedPath));
                    storedPath = QFileInfo(destination).fileName() + QStringLiteral(".assets/") + copied->first;
                    digest = copied->second;
                    size = QFileInfo(staging.filePath(QStringLiteral("assets/") + copied->first)).size();
                }
                QSqlQuery update(connection.db);
                update.prepare(QStringLiteral("UPDATE assets SET path=?,external=?,sha256=COALESCE(?,X''),size=? WHERE id=?"));
                update.addBindValue(storedPath); update.addBindValue(external ? 1 : 0);
                update.addBindValue(digest); update.addBindValue(size);
                update.addBindValue(query.value(0));
                if (!update.exec()) return fail(error, update.lastError().text());
            }
        }
        if (!connection.db.commit()) return fail(error, connection.db.lastError().text());
    }
    if (!ProjectStore(databasePath).load(error)) return false;
    if (cancel && cancel->load()) return fail(error, QStringLiteral("备份已取消。"));
    // 先发布不可变资产，再发布数据库；失败时把资产收回临时目录。
    if (!QDir().rename(staging.filePath(QStringLiteral("assets")), destination + QStringLiteral(".assets")))
        return fail(error, QStringLiteral("无法发布备份资产。"));
    if (!QFile::rename(databasePath, destination)) {
        if (!QDir().rename(destination + QStringLiteral(".assets"), staging.filePath(QStringLiteral("assets"))))
            return fail(error, QStringLiteral("数据库发布失败，资产留在备份目标旁，请保留以便恢复。"));
        return fail(error, QStringLiteral("无法发布备份数据库。"));
    }
    return true;
}

QString ProjectStore::resolveAudioPath(const QString &storedPath) const
{
    return QFileInfo(storedPath).isAbsolute() ? storedPath
        : QFileInfo(path_).absoluteDir().absoluteFilePath(storedPath);
}
} // namespace subcue
