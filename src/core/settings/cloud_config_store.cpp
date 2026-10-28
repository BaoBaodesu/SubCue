#include "settings/cloud_config_store.h"

#include <QtCore/QDir>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <utility>

namespace subcue {
namespace {
struct Database {
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    explicit Database(const QString &path) { db.setDatabaseName(path); }
    ~Database() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
};
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
const QStringList tasks = {QStringLiteral("speechRecognition"), QStringLiteral("subtitleWordMapping"),
    QStringLiteral("subtitleAudioReview"), QStringLiteral("roughcutAudioReview"), QStringLiteral("speechSynthesis")};
QByteArray verificationIdentity(QJsonObject account, QJsonObject model)
{
    for (const auto &field : {"name", "notes", "enabled"}) account.remove(QString::fromLatin1(field));
    model.remove(QStringLiteral("name")); model.remove(QStringLiteral("enabled"));
    return QCryptographicHash::hash(QJsonDocument(QJsonObject{{QStringLiteral("account"), account}, {QStringLiteral("model"), model}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
}
bool verified(Database &connection, const QJsonObject &account, const QJsonObject &model)
{
    QSqlQuery query(connection.db); query.prepare(QStringLiteral("SELECT payload FROM cloud_verifications WHERE id=?"));
    query.addBindValue(model.value(QStringLiteral("id")).toString());
    return query.exec() && query.next() && QJsonDocument::fromJson(query.value(0).toByteArray()).object().value(QStringLiteral("identity")).toString().toLatin1() == verificationIdentity(account, model);
}
bool validate(const QJsonObject &draft, QString *error);
QJsonObject read(Database &connection, QString *error)
{
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("SELECT schema_version,store_id,revision FROM cloud_meta WHERE id=1")) || !query.next()
        || query.value(0).toInt() != 1) {
        fail(error, QStringLiteral("云端配置数据库不可读或版本不兼容")); return {};
    }
    QJsonObject result{{QStringLiteral("storeId"), query.value(1).toString()},
        {QStringLiteral("revision"), query.value(2).toLongLong()}};
    for (const auto &table : {QStringLiteral("accounts"), QStringLiteral("models"), QStringLiteral("bindings")}) {
        if (!query.exec(QStringLiteral("SELECT payload FROM cloud_%1 ORDER BY rowid").arg(table))) {
            fail(error, QStringLiteral("无法读取云端配置表")); return {};
        }
        QJsonArray rows;
        while (query.next()) {
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(query.value(0).toByteArray(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                fail(error, QStringLiteral("云端配置记录损坏")); return {};
            }
            rows.append(document.object());
        }
        result.insert(table, rows);
    }
    if (QUuid(result.value(QStringLiteral("storeId")).toString()).isNull()) { fail(error, QStringLiteral("云端配置存储身份损坏")); return {}; }
    if (!validate(result, error)) return {};
    QSet<QString> refs;
    const auto prefix = QStringLiteral("SubCue/Cloud/%1/").arg(result.value(QStringLiteral("storeId")).toString());
    for (const auto &value : result.value(QStringLiteral("accounts")).toArray()) {
        const auto ref = value.toObject().value(QStringLiteral("credentialRef")).toString();
        if (!ref.isEmpty() && (!ref.startsWith(prefix) || QUuid(ref.mid(prefix.size())).isNull() || refs.contains(ref))) {
            fail(error, QStringLiteral("云端凭据引用损坏，已停止读取及清理")); return {};
        }
        if (!ref.isEmpty()) refs.insert(ref);
    }
    return result;
}
bool validate(const QJsonObject &draft, QString *error)
{
    for (const auto &table : {QStringLiteral("accounts"), QStringLiteral("models"), QStringLiteral("bindings")})
        if (!draft.value(table).isArray()) return fail(error, QStringLiteral("云端配置集合格式无效"));
    QSet<QString> accounts, models, bindings;
    const auto check = [&](const QJsonObject &row, const QStringList &allowed) {
        for (auto it = row.begin(); it != row.end(); ++it)
            if (!allowed.contains(it.key())) return false;
        return true;
    };
    for (const auto &value : draft.value(QStringLiteral("accounts")).toArray()) {
        const auto row = value.toObject(); const auto id = row.value(QStringLiteral("id")).toString();
        const auto vendor = row.value(QStringLiteral("vendor")).toString();
        const auto region = row.value(QStringLiteral("region")).toString();
        if (!check(row, {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("vendor"), QStringLiteral("region"),
                QStringLiteral("notes"), QStringLiteral("enabled"), QStringLiteral("workspace"), QStringLiteral("credentialRef"), QStringLiteral("credentialRevision")})
            || id.isEmpty() || id.size() > 128 || accounts.contains(id) || row.value(QStringLiteral("name")).toString().trimmed().isEmpty()
            || !QStringList{QStringLiteral("bailian"), QStringLiteral("deepseek"), QStringLiteral("moonshot")}.contains(vendor)
            || (vendor == QLatin1String("bailian") ? !QStringList{QStringLiteral("beijing"), QStringLiteral("singapore")}.contains(region) : region != QLatin1String("default")))
            return fail(error, QStringLiteral("云端账号字段、厂商或地域无效"));
        if (row.contains(QStringLiteral("workspace")) && !row.value(QStringLiteral("workspace")).isString()) return fail(error, QStringLiteral("账号 Workspace 类型无效"));
        if (!row.value(QStringLiteral("workspace")).toString().isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,127}$")).match(row.value(QStringLiteral("workspace")).toString()).hasMatch()) return fail(error, QStringLiteral("账号 Workspace 无效"));
        accounts.insert(id);
    }
    for (const auto &value : draft.value(QStringLiteral("models")).toArray()) {
        const auto row = value.toObject(); const auto id = row.value(QStringLiteral("id")).toString();
        const auto adapter = row.value(QStringLiteral("adapter")).toString();
        const auto workspace = row.value(QStringLiteral("workspace")).toString();
        if (!check(row, {QStringLiteral("id"), QStringLiteral("accountId"), QStringLiteral("remoteId"), QStringLiteral("name"),
                QStringLiteral("adapter"), QStringLiteral("workspace"), QStringLiteral("enabled")})
            || id.isEmpty() || models.contains(id) || !accounts.contains(row.value(QStringLiteral("accountId")).toString())
            || row.value(QStringLiteral("remoteId")).toString().trimmed().isEmpty()
            || !QStringList{QStringLiteral("dashscope-asr"), QStringLiteral("compatible-text"), QStringLiteral("qwen-omni"), QStringLiteral("qwen-tts")}.contains(adapter)
            || (!workspace.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,127}$")).match(workspace).hasMatch()))
            return fail(error, QStringLiteral("云端模型字段、适配器或 Workspace 无效"));
        const auto accountId = row.value(QStringLiteral("accountId")).toString();
        for (const auto &account : draft.value(QStringLiteral("accounts")).toArray())
            if (account.toObject().value(QStringLiteral("id")).toString() == accountId
                && account.toObject().value(QStringLiteral("vendor")).toString() != QLatin1String("bailian")
                && adapter != QLatin1String("compatible-text")) return fail(error, QStringLiteral("该厂商尚未适配此音频协议"));
        models.insert(id);
    }
    for (const auto &value : draft.value(QStringLiteral("bindings")).toArray()) {
        const auto row = value.toObject(); const auto task = row.value(QStringLiteral("taskId")).toString();
        const auto target = row.value(QStringLiteral("target")).toString();
        if (!check(row, {QStringLiteral("taskId"), QStringLiteral("target")}) || !tasks.contains(task) || bindings.contains(task)
            || (!target.isEmpty() && !models.contains(target) && !(task == QLatin1String("speechRecognition")
                && QStringList{QStringLiteral("local:funasr"), QStringLiteral("local:qwen3")}.contains(target))))
            return fail(error, QStringLiteral("任务绑定无效"));
        bindings.insert(task);
        for (const auto &model : draft.value(QStringLiteral("models")).toArray()) {
            const auto rowModel = model.toObject();
            if (rowModel.value(QStringLiteral("id")).toString() != target) continue;
            const auto required = task == QLatin1String("speechRecognition") ? QStringLiteral("dashscope-asr")
                : task == QLatin1String("subtitleWordMapping") ? QStringLiteral("compatible-text")
                : task == QLatin1String("speechSynthesis") ? QStringLiteral("qwen-tts") : QStringLiteral("qwen-omni");
            if (rowModel.value(QStringLiteral("adapter")).toString() != required) return fail(error, QStringLiteral("模型与任务能力不兼容"));
        }
    }
    if (bindings.size() != tasks.size()) return fail(error, QStringLiteral("需要保留全部五项任务绑定，未配置时可留空"));
    return true;
}
bool write(Database &connection, const QJsonObject &draft, QString *error)
{
    QSqlQuery query(connection.db);
    for (const auto &table : {QStringLiteral("accounts"), QStringLiteral("models"), QStringLiteral("bindings")}) {
        if (!query.exec(QStringLiteral("DELETE FROM cloud_%1").arg(table))) return fail(error, QStringLiteral("云端配置写入失败"));
        for (const auto &value : draft.value(table).toArray()) {
            query.prepare(QStringLiteral("INSERT INTO cloud_%1(id,payload) VALUES(?,?)").arg(table));
            query.addBindValue(value.toObject().value(table == QLatin1String("bindings") ? QStringLiteral("taskId") : QStringLiteral("id")).toString());
            query.addBindValue(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            if (!query.exec()) return fail(error, QStringLiteral("云端配置记录写入失败"));
        }
    }
    return true;
}
}

CloudConfigStore::CloudConfigStore(QString path, ICredentialStore *credentials)
    : path_(std::move(path)), credentials_(credentials) {}

bool CloudConfigStore::initialize(const QJsonObject &legacy, QString *error)
{
    if (error) error->clear();
    if (legacy.contains(QStringLiteral("ttsConnectionReadError"))) return fail(error, QStringLiteral("旧连接配置不可读，已停止迁移"));
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail(error, QStringLiteral("无法创建云端配置目录"));
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) return fail(error, QStringLiteral("云端配置正在被其他进程使用"));
    Database connection(path_);
    if (!connection.db.open() || !connection.db.transaction()) return fail(error, QStringLiteral("无法打开云端配置数据库"));
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS cloud_connections(id TEXT PRIMARY KEY,name TEXT NOT NULL,workspace_id TEXT NOT NULL,credential_ref TEXT NOT NULL,active INTEGER NOT NULL CHECK(active IN (0,1)))"))) return fail(error, QStringLiteral("旧连接表不可用"));
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS cloud_meta(id INTEGER PRIMARY KEY CHECK(id=1),schema_version INTEGER NOT NULL,store_id TEXT NOT NULL,revision INTEGER NOT NULL)"))) return fail(error, QStringLiteral("无法初始化云端配置"));
    if (!query.exec(QStringLiteral("SELECT schema_version FROM cloud_meta WHERE id=1"))) return false;
    const bool initialized = query.next();
    if (initialized && query.value(0).toInt() != 1) return fail(error, QStringLiteral("云端配置版本不兼容"));
    if (initialized) {
        if (read(connection, error).isEmpty()) return false;
        return connection.db.commit() || fail(error, QStringLiteral("无法提交云端配置检查"));
    }
    for (const auto &table : {QStringLiteral("accounts"), QStringLiteral("models"), QStringLiteral("bindings"), QStringLiteral("verifications")})
        if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS cloud_%1(id TEXT PRIMARY KEY,payload BLOB NOT NULL)").arg(table))) return fail(error, QStringLiteral("无法初始化云端配置表"));
    if (!initialized) {
        query.prepare(QStringLiteral("INSERT INTO cloud_meta VALUES(1,1,?,0)")); query.addBindValue(uuid());
        if (!query.exec()) return fail(error, QStringLiteral("无法初始化云端配置身份"));
        QJsonArray accounts, models, bindings;
        const auto add = [&](const QString &id, const QString &name, const QString &adapter, const QString &remote, const QString &workspace, const QString &region) {
            accounts.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("vendor"), QStringLiteral("bailian")}, {QStringLiteral("region"), region}, {QStringLiteral("enabled"), true}, {QStringLiteral("credentialRef"), QStringLiteral("")}, {QStringLiteral("credentialRevision"), 0}});
            models.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("accountId"), id}, {QStringLiteral("remoteId"), remote}, {QStringLiteral("name"), remote}, {QStringLiteral("adapter"), adapter}, {QStringLiteral("workspace"), workspace}, {QStringLiteral("enabled"), true}});
        };
        if (legacy.value(QStringLiteral("asrProvider")).toString() == QLatin1String("dashscope")) {
            add(QStringLiteral("legacy-asr"), QStringLiteral("原语音识别账号"), QStringLiteral("dashscope-asr"), legacy.value(QStringLiteral("asrModel")).toString(), {}, legacy.value(QStringLiteral("region")).toString(QStringLiteral("beijing")));
            bindings.append(QJsonObject{{QStringLiteral("taskId"), QStringLiteral("speechRecognition")}, {QStringLiteral("target"), QStringLiteral("legacy-asr")}});
        } else bindings.append(QJsonObject{{QStringLiteral("taskId"), QStringLiteral("speechRecognition")}, {QStringLiteral("target"), QStringLiteral("local:") + legacy.value(QStringLiteral("asrProvider")).toString(QStringLiteral("funasr"))}});
        add(QStringLiteral("legacy-review"), QStringLiteral("原 AI 复核账号"), QStringLiteral("qwen-omni"), QStringLiteral("qwen3.8-omni-flash"), {}, QStringLiteral("beijing"));
        for (const auto &task : {QStringLiteral("subtitleAudioReview"), QStringLiteral("roughcutAudioReview")}) bindings.append(QJsonObject{{QStringLiteral("taskId"), task}, {QStringLiteral("target"), QStringLiteral("legacy-review")}});
        // 迁移只读取非敏感配置，不导入、比较或恢复旧密钥。
        for (const auto &value : legacy.value(QStringLiteral("ttsConnections")).toArray()) {
            const auto row = value.toObject(); const auto id = QStringLiteral("legacy-tts-") + row.value(QStringLiteral("id")).toString();
            add(id, row.value(QStringLiteral("name")).toString(), QStringLiteral("qwen-tts"), QStringLiteral("qwen-audio-3.1-tts-flash"), row.value(QStringLiteral("workspace")).toString(), QStringLiteral("beijing"));
            if (row.value(QStringLiteral("id")) == legacy.value(QStringLiteral("ttsActiveConnection"))) bindings.append(QJsonObject{{QStringLiteral("taskId"), QStringLiteral("speechSynthesis")}, {QStringLiteral("target"), id}});
        }
        for (const auto &task : tasks) {
            bool found = false;
            for (const auto &row : bindings) if (row.toObject().value(QStringLiteral("taskId")).toString() == task) found = true;
            if (!found) bindings.append(QJsonObject{{QStringLiteral("taskId"), task}, {QStringLiteral("target"), QStringLiteral("")}});
        }
        const QJsonObject draft{{QStringLiteral("accounts"), accounts}, {QStringLiteral("models"), models}, {QStringLiteral("bindings"), bindings}};
        if (!validate(draft, error) || !write(connection, draft, error)) return false;
    }
    return connection.db.commit() || fail(error, QStringLiteral("云端配置初始化提交失败"));
}

QJsonObject CloudConfigStore::load(QString *error) const
{
    if (error) error->clear();
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) { fail(error, QStringLiteral("云端配置忙，请稍后重试")); return {}; }
    Database connection(path_);
    if (!connection.db.open()) { fail(error, QStringLiteral("无法读取云端配置数据库")); return {}; }
    return read(connection, error);
}

bool CloudConfigStore::save(QJsonObject draft, qint64 expectedRevision,
    const QMap<QString, CloudSecretChange> &secrets, QString *error)
{
    if (error) error->clear();
    if (!validate(draft, error)) return false;
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) return fail(error, QStringLiteral("云端配置忙，请稍后重试"));
    Database connection(path_);
    if (!connection.db.open()) return fail(error, QStringLiteral("无法打开云端配置数据库"));
    const auto original = read(connection, error);
    if (original.isEmpty()) return false;
    if (original.value(QStringLiteral("revision")).toInteger() != expectedRevision) return fail(error, QStringLiteral("云端配置已更新，请重新载入后保存"));
    QMap<QString, QJsonObject> old;
    for (const auto &row : original.value(QStringLiteral("accounts")).toArray()) old.insert(row.toObject().value(QStringLiteral("id")).toString(), row.toObject());
    QSet<QString> ids;
    for (const auto &value : draft.value(QStringLiteral("accounts")).toArray()) ids.insert(value.toObject().value(QStringLiteral("id")).toString());
    for (auto it = secrets.begin(); it != secrets.end(); ++it)
        if (!ids.contains(it.key())) return fail(error, QStringLiteral("密钥动作引用了不存在的账号"));
    QJsonArray accounts; QStringList created, retired;
    const auto rollbackKeys = [&] { for (const auto &id : created) { QString ignored; (void)credentials_->remove(id, &ignored); } };
    for (const auto &value : draft.value(QStringLiteral("accounts")).toArray()) {
        auto row = value.toObject(); const auto id = row.value(QStringLiteral("id")).toString();
        const auto previous = old.take(id); const auto change = secrets.value(id);
        if (!previous.isEmpty() && (previous.value(QStringLiteral("vendor")) != row.value(QStringLiteral("vendor")) || previous.value(QStringLiteral("region")) != row.value(QStringLiteral("region")))) {
            rollbackKeys(); return fail(error, QStringLiteral("变更厂商或地域请新建账号，不能继承原密钥"));
        }
        row.insert(QStringLiteral("credentialRef"), previous.value(QStringLiteral("credentialRef")).toString());
        row.insert(QStringLiteral("credentialRevision"), previous.value(QStringLiteral("credentialRevision")).toInteger());
        if (change.action != CloudSecretAction::Keep) {
            if (!row.value(QStringLiteral("credentialRef")).toString().isEmpty()) retired.append(row.value(QStringLiteral("credentialRef")).toString());
            row.insert(QStringLiteral("credentialRef"), QString());
            row.insert(QStringLiteral("credentialRevision"), row.value(QStringLiteral("credentialRevision")).toInteger() + 1);
            if (change.action == CloudSecretAction::Replace) {
                if (!credentials_ || change.value.trimmed().isEmpty() || change.value.contains(QLatin1Char('\r')) || change.value.contains(QLatin1Char('\n'))) { rollbackKeys(); return fail(error, QStringLiteral("新密钥为空或包含非法换行")); }
                const auto ref = QStringLiteral("SubCue/Cloud/%1/%2").arg(original.value(QStringLiteral("storeId")).toString(), uuid());
                if (!credentials_->save(ref, change.value.trimmed(), error)) { rollbackKeys(); return false; }
                created.append(ref); row.insert(QStringLiteral("credentialRef"), ref);
            }
        }
        accounts.append(row);
    }
    for (const auto &row : old) if (!row.value(QStringLiteral("credentialRef")).toString().isEmpty()) retired.append(row.value(QStringLiteral("credentialRef")).toString());
    draft.insert(QStringLiteral("accounts"), accounts);
    if (!connection.db.transaction()) { rollbackKeys(); return fail(error, QStringLiteral("无法开始云端配置事务")); }
    if (!write(connection, draft, error)) { rollbackKeys(); return false; }
    QSqlQuery query(connection.db);
    if (!query.exec(QStringLiteral("UPDATE cloud_meta SET revision=revision+1 WHERE id=1"))) { rollbackKeys(); return fail(error, QStringLiteral("云端配置写入失败")); }
    if (!connection.db.commit()) {
        // 提交结果不确定时保留新旧凭据，之后只依据已提交的实际引用清理。
        return fail(error, QStringLiteral("云端配置提交未确认，已保留凭据；请重新载入"));
    }
    bool cleaned = true;
    for (const auto &ref : retired) if (credentials_ && !credentials_->remove(ref)) cleaned = false;
    if (!cleaned && error) *error = QStringLiteral("配置已保存，旧密钥清理待重试");
    return true;
}

QString CloudConfigStore::resolveKey(const QString &accountId, QString *error) const
{
    if (error) error->clear();
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) { fail(error, QStringLiteral("云端配置忙")); return {}; }
    Database connection(path_);
    if (!connection.db.open()) { fail(error, QStringLiteral("无法读取云端配置")); return {}; }
    const auto state = read(connection, error);
    if (state.isEmpty()) return {};
    for (const auto &value : state.value(QStringLiteral("accounts")).toArray()) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("id")).toString() != accountId) continue;
        const auto ref = row.value(QStringLiteral("credentialRef")).toString();
        if (!row.value(QStringLiteral("enabled")).toBool(true) || ref.isEmpty()) break;
        if (!ref.startsWith(QStringLiteral("SubCue/Cloud/%1/").arg(state.value(QStringLiteral("storeId")).toString()))) break;
        return credentials_ ? credentials_->load(ref, error) : QString();
    }
    fail(error, QStringLiteral("账号已停用或需要添加密钥")); return {};
}

bool CloudConfigStore::cleanup(QString *error) const
{
    if (error) error->clear();
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) return fail(error, QStringLiteral("云端配置忙"));
    Database connection(path_);
    if (!connection.db.open()) return fail(error, QStringLiteral("数据库不可读，已停止凭据清理"));
    const auto state = read(connection, error);
    if (state.isEmpty() || !credentials_) return false;
    QSqlQuery verification(connection.db);
    if (!verification.exec(QStringLiteral("SELECT id FROM cloud_verifications LIMIT 1"))) return fail(error, QStringLiteral("验证表损坏，已停止清理"));
    const auto prefix = QStringLiteral("SubCue/Cloud/%1/").arg(state.value(QStringLiteral("storeId")).toString());
    QSet<QString> active;
    for (const auto &row : state.value(QStringLiteral("accounts")).toArray()) active.insert(row.toObject().value(QStringLiteral("credentialRef")).toString());
    QString listError;
    const auto refs = credentials_->list(prefix, &listError);
    if (!listError.isEmpty()) return fail(error, listError);
    for (const auto &ref : refs) if (ref.startsWith(prefix) && !active.contains(ref) && !credentials_->remove(ref, error)) return false;
    return true;
}

QJsonObject CloudConfigStore::resolveSynthesis(QString *key, QString *error) const
{
    const auto snapshot = resolveTask(QStringLiteral("speechSynthesis"), key, error);
    if (snapshot.isEmpty()) return {};
    const auto account = snapshot.value(QStringLiteral("account")).toObject();
    auto model = snapshot.value(QStringLiteral("model")).toObject();
    if (model.value(QStringLiteral("workspace")).toString().isEmpty()) model.insert(QStringLiteral("workspace"), account.value(QStringLiteral("workspace")));
    // 仅开放已有价格、协议及结果保护的型号，不能按相似名称猜测兼容性。
    if (account.value(QStringLiteral("vendor")) != QLatin1String("bailian")
        || account.value(QStringLiteral("region")) != QLatin1String("beijing")
        || model.value(QStringLiteral("adapter")) != QLatin1String("qwen-tts")
        || model.value(QStringLiteral("remoteId")) != QLatin1String("qwen-audio-3.1-tts-flash")
        || model.value(QStringLiteral("workspace")).toString().isEmpty()) {
        if (key) key->clear();
        fail(error, QStringLiteral("当前仅适配北京 qwen-audio-3.1-tts-flash，请检查 AI 服务中的型号、地域与 Workspace。"));
        return {};
    }
    return {{QStringLiteral("cloudAccountId"), account.value(QStringLiteral("id"))},
        {QStringLiteral("cloudModelId"), model.value(QStringLiteral("id"))},
        {QStringLiteral("model"), model.value(QStringLiteral("remoteId"))},
        {QStringLiteral("region"), account.value(QStringLiteral("region"))},
        {QStringLiteral("businessSpace"), model.value(QStringLiteral("workspace"))}};
}

bool CloudConfigStore::resolveAsr(QJsonObject *settings, QString *key, QString *error) const
{
    if (!settings || !key) return fail(error, QStringLiteral("识别配置目标无效"));
    const auto snapshot = resolveTask(QStringLiteral("speechRecognition"), key, error);
    if (snapshot.isEmpty()) return false;
    const auto target = snapshot.value(QStringLiteral("target")).toString();
    if (target.startsWith(QStringLiteral("local:"))) {
        settings->insert(QStringLiteral("asrProvider"), target.mid(6));
    } else {
        const auto account = snapshot.value(QStringLiteral("account")).toObject();
        const auto model = snapshot.value(QStringLiteral("model")).toObject();
        if (model.value(QStringLiteral("adapter")).toString() != QLatin1String("dashscope-asr"))
            return fail(error, QStringLiteral("当前模型不支持语音识别"));
        settings->insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope"));
        settings->insert(QStringLiteral("cloudAccountId"), account.value(QStringLiteral("id")));
        settings->insert(QStringLiteral("asrModel"), model.value(QStringLiteral("remoteId")));
        settings->insert(QStringLiteral("region"), account.value(QStringLiteral("region")));
        const auto workspace = model.value(QStringLiteral("workspace")).toString();
        settings->insert(QStringLiteral("asrApiHost"), workspace.isEmpty() ? QString()
            : QStringLiteral("https://%1.%2.maas.aliyuncs.com").arg(workspace,
                account.value(QStringLiteral("region")).toString() == QLatin1String("singapore")
                    ? QStringLiteral("ap-southeast-1") : QStringLiteral("cn-beijing")));
    }
    // 这里只表示统一配置已解析，不冒充已通过远端连接测试。
    settings->insert(QStringLiteral("unifiedAsrConfigured"), true);
    return true;
}

bool CloudConfigStore::adaptedModel(const QJsonObject &account, const QJsonObject &model)
{
    const auto vendor = account.value(QStringLiteral("vendor")).toString(), adapter = model.value(QStringLiteral("adapter")).toString(), remote = model.value(QStringLiteral("remoteId")).toString();
    if (adapter == QLatin1String("compatible-text")) {
        if (vendor == QLatin1String("deepseek")) return QStringList{QStringLiteral("deepseek-chat"), QStringLiteral("deepseek-reasoner")}.contains(remote);
        if (vendor == QLatin1String("moonshot")) return QStringList{QStringLiteral("moonshot-v1-8k"), QStringLiteral("moonshot-v1-32k"), QStringLiteral("moonshot-v1-128k")}.contains(remote);
        return QStringList{QStringLiteral("qwen-plus"), QStringLiteral("qwen-turbo"), QStringLiteral("qwen-max")}.contains(remote);
    }
    if (vendor != QLatin1String("bailian")) return false;
    if (adapter == QLatin1String("dashscope-asr")) return remote == QLatin1String("fun-asr-flash");
    if (adapter == QLatin1String("qwen-omni")) return remote == QLatin1String("qwen3.8-omni-flash");
    return adapter == QLatin1String("qwen-tts") && remote == QLatin1String("qwen-audio-3.1-tts-flash") && account.value(QStringLiteral("region")) == QLatin1String("beijing");
}
bool CloudConfigStore::modelVerified(const QString &modelId) const
{
    Database connection(path_); if (!connection.db.open()) return false;
    const auto state = read(connection, nullptr);
    for (const auto &value : state.value(QStringLiteral("models")).toArray()) {
        const auto model = value.toObject(); if (model.value(QStringLiteral("id")).toString() != modelId) continue;
        for (const auto &owner : state.value(QStringLiteral("accounts")).toArray())
            if (owner.toObject().value(QStringLiteral("id")) == model.value(QStringLiteral("accountId"))) return verified(connection, owner.toObject(), model);
    }
    return false;
}
bool CloudConfigStore::markModelVerified(const QString &modelId, qint64 expectedRevision, QString *error)
{
    QLockFile lock(path_ + QStringLiteral(".cloud.lock")); if (!lock.tryLock(2000)) return fail(error, QStringLiteral("云端配置忙"));
    Database connection(path_); if (!connection.db.open()) return fail(error, QStringLiteral("无法读取云端配置"));
    const auto state = read(connection, error);
    if (state.isEmpty() || state.value(QStringLiteral("revision")).toInteger() != expectedRevision) return fail(error, QStringLiteral("配置已变更，请重新测试任务能力"));
    for (const auto &value : state.value(QStringLiteral("models")).toArray()) {
        const auto model = value.toObject(); if (model.value(QStringLiteral("id")).toString() != modelId) continue;
        for (const auto &owner : state.value(QStringLiteral("accounts")).toArray()) {
            if (owner.toObject().value(QStringLiteral("id")) != model.value(QStringLiteral("accountId"))) continue;
            QSqlQuery query(connection.db); query.prepare(QStringLiteral("INSERT OR REPLACE INTO cloud_verifications(id,payload) VALUES(?,?)")); query.addBindValue(modelId);
            query.addBindValue(QJsonDocument(QJsonObject{{QStringLiteral("identity"), QString::fromLatin1(verificationIdentity(owner.toObject(), model))}, {QStringLiteral("verifiedAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson(QJsonDocument::Compact));
            return query.exec() || fail(error, QStringLiteral("无法保存能力验证结果"));
        }
    }
    return fail(error, QStringLiteral("模型已移除"));
}

QJsonObject CloudConfigStore::resolveTask(const QString &taskId, QString *key, QString *error) const
{
    if (error) error->clear();
    if (key) key->clear();
    QLockFile lock(path_ + QStringLiteral(".cloud.lock"));
    if (!lock.tryLock(2000)) { fail(error, QStringLiteral("云端配置忙")); return {}; }
    Database connection(path_);
    if (!connection.db.open()) { fail(error, QStringLiteral("无法读取云端配置")); return {}; }
    const auto state = read(connection, error);
    if (state.isEmpty()) return {};
    QString target;
    for (const auto &row : state.value(QStringLiteral("bindings")).toArray())
        if (row.toObject().value(QStringLiteral("taskId")).toString() == taskId) target = row.toObject().value(QStringLiteral("target")).toString();
    if (target.startsWith(QStringLiteral("local:"))) return QJsonObject{{QStringLiteral("target"), target}, {QStringLiteral("revision"), state.value(QStringLiteral("revision"))}};
    for (const auto &value : state.value(QStringLiteral("models")).toArray()) {
        const auto model = value.toObject();
        if (model.value(QStringLiteral("id")).toString() != target || !model.value(QStringLiteral("enabled")).toBool(true)) continue;
        for (const auto &accountValue : state.value(QStringLiteral("accounts")).toArray()) {
            auto account = accountValue.toObject();
            if (account.value(QStringLiteral("id")) != model.value(QStringLiteral("accountId")) || !account.value(QStringLiteral("enabled")).toBool(true)) continue;
            if (!adaptedModel(account, model) && !verified(connection, account, model)) {
                fail(error, QStringLiteral("此模型尚未验证任务能力，请在 AI 服务的模型详情中测试。")); return {};
            }
            const auto ref = account.value(QStringLiteral("credentialRef")).toString();
            if (ref.isEmpty() || !credentials_ || !key) break;
            *key = credentials_->load(ref, error);
            if (key->isEmpty()) return {};
            account.remove(QStringLiteral("credentialRef"));
            return QJsonObject{{QStringLiteral("account"), account}, {QStringLiteral("model"), model}, {QStringLiteral("revision"), state.value(QStringLiteral("revision"))}};
        }
    }
    fail(error, QStringLiteral("任务未配置可用模型或需要添加密钥")); return {};
}
} // namespace subcue
