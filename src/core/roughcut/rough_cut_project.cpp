#include "roughcut/rough_cut_project.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

namespace subcue {
namespace {

QString name(RoughCutDecision value)
{
    if (value == RoughCutDecision::Keep) return QStringLiteral("KEEP");
    if (value == RoughCutDecision::Cut) return QStringLiteral("CUT");
    return QStringLiteral("REVIEW");
}

std::optional<RoughCutDecision> decision(const QString &value)
{
    if (value == QLatin1String("KEEP")) return RoughCutDecision::Keep;
    if (value == QLatin1String("CUT")) return RoughCutDecision::Cut;
    if (value == QLatin1String("REVIEW")) return RoughCutDecision::Review;
    return std::nullopt;
}

QString relative(const QString &project, const QString &path)
{
    return path.isEmpty() ? QString() : QDir::fromNativeSeparators(
        QFileInfo(project).absoluteDir().relativeFilePath(path));
}

QString absolute(const QString &project, const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(
        QFileInfo(project).absoluteDir().absoluteFilePath(path));
}

class Database final {
public:
    Database(const QString &path, bool writable)
        : id(QStringLiteral("roughcut-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), id);
        db.setDatabaseName(path);
        Q_UNUSED(writable)
    }
    ~Database() { db.close(); db = {}; QSqlDatabase::removeDatabase(id); }
    QString id;
    QSqlDatabase db;
};

bool setError(QString *output, const QString &message)
{
    if (output) *output = message;
    return false;
}

} // namespace

QByteArray RoughCutProjectSerializer::mediaSha256(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        setError(errorMessage, file.errorString());
        return {};
    }
    return hash.result();
}

bool RoughCutProjectSerializer::save(const QString &path, const RoughCutProject &project,
                                     QString *errorMessage)
{
    if (project.recording.size() != project.decisions.size() || project.sampleRate <= 0
        || project.channels <= 0 || project.sourceSampleCount <= 0 || project.mediaSha256.size() != 32)
        return setError(errorMessage, QStringLiteral("粗剪工程数据不完整"));
    Database holder(path, true);
    if (!holder.db.open()) return setError(errorMessage, holder.db.lastError().text());
    QSqlQuery query(holder.db);
    if (!holder.db.transaction()
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value BLOB)"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_versions(version INTEGER PRIMARY KEY,created_at TEXT NOT NULL)"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_segments(analysis_version INTEGER NOT NULL,position INTEGER NOT NULL,id TEXT NOT NULL,text TEXT NOT NULL,start_sample INTEGER NOT NULL,end_sample INTEGER NOT NULL,auto_decision TEXT NOT NULL,user_decision TEXT,rule_score REAL NOT NULL,reason TEXT NOT NULL,evidence_json TEXT NOT NULL,PRIMARY KEY(analysis_version,position))"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS auxiliary_results(analysis_version INTEGER NOT NULL,recording_index INTEGER NOT NULL,primary_text TEXT NOT NULL,funasr_text TEXT NOT NULL,whisper_text TEXT NOT NULL,funasr_failed INTEGER NOT NULL,whisper_failed INTEGER NOT NULL,conflict INTEGER NOT NULL,PRIMARY KEY(analysis_version,recording_index))"))
        || !query.exec(QStringLiteral("DELETE FROM metadata"))) {
        holder.db.rollback(); return setError(errorMessage, query.lastError().text());
    }
    query.prepare(QStringLiteral("INSERT INTO metadata VALUES(?,?)"));
    const QList<QPair<QString, QVariant>> values{
        {QStringLiteral("schemaVersion"), 2}, {QStringLiteral("analysisVersion"), project.analysisVersion},
        {QStringLiteral("mediaPath"), relative(path, project.mediaPath)},
        {QStringLiteral("mediaSha256"), project.mediaSha256},
        {QStringLiteral("scriptPath"), relative(path, project.scriptPath)},
        {QStringLiteral("scriptText"), project.scriptText},
        {QStringLiteral("sampleRate"), project.sampleRate}, {QStringLiteral("channels"), project.channels},
        {QStringLiteral("sourceSampleCount"), project.sourceSampleCount}};
    for (const auto &[key, value] : values) {
        query.bindValue(0, key); query.bindValue(1, value);
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO analysis_versions VALUES(?,datetime('now'))"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("DELETE FROM analysis_segments WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO analysis_segments VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
    for (int index = 0; index < project.recording.size(); ++index) {
        const auto &passage = project.recording.at(index);
        const auto &result = project.decisions.at(index);
        QJsonArray evidence;
        for (const QString &item : result.evidence) evidence.append(item);
        query.bindValue(0, project.analysisVersion); query.bindValue(1, index);
        query.bindValue(2, passage.id); query.bindValue(3, passage.text);
        query.bindValue(4, passage.startSample); query.bindValue(5, passage.endSample);
        query.bindValue(6, name(result.autoDecision));
        query.bindValue(7, result.userDecision ? QVariant(name(*result.userDecision)) : QVariant());
        query.bindValue(8, result.ruleScore); query.bindValue(9, result.reason);
        query.bindValue(10, QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact)));
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    query.prepare(QStringLiteral("DELETE FROM auxiliary_results WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO auxiliary_results VALUES(?,?,?,?,?,?,?,?)"));
    for (const auto &result : project.auxiliaryResults) {
        query.bindValue(0, project.analysisVersion); query.bindValue(1, result.recordingIndex);
        query.bindValue(2, result.primaryText); query.bindValue(3, result.funAsrText);
        query.bindValue(4, result.whisperText); query.bindValue(5, result.funAsrFailed);
        query.bindValue(6, result.whisperFailed); query.bindValue(7, result.conflict);
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    return holder.db.commit() || setError(errorMessage, holder.db.lastError().text());
}

std::optional<RoughCutProject> RoughCutProjectSerializer::load(const QString &path,
                                                               QString *errorMessage)
{
    Database holder(path, false);
    if (!holder.db.open()) { setError(errorMessage, holder.db.lastError().text()); return std::nullopt; }
    QSqlQuery query(holder.db);
    if (!query.exec(QStringLiteral("SELECT key,value FROM metadata"))) {
        setError(errorMessage, query.lastError().text()); return std::nullopt;
    }
    QHash<QString, QVariant> values;
    while (query.next()) values.insert(query.value(0).toString(), query.value(1));
    const int schemaVersion = values.value(QStringLiteral("schemaVersion")).toInt();
    if (schemaVersion != 1 && schemaVersion != 2) {
        setError(errorMessage, QStringLiteral("不支持的粗剪工程版本")); return std::nullopt;
    }
    RoughCutProject project;
    project.analysisVersion = values.value(QStringLiteral("analysisVersion")).toInt();
    project.mediaPath = absolute(path, values.value(QStringLiteral("mediaPath")).toString());
    project.mediaSha256 = values.value(QStringLiteral("mediaSha256")).toByteArray();
    project.scriptPath = absolute(path, values.value(QStringLiteral("scriptPath")).toString());
    project.scriptText = values.value(QStringLiteral("scriptText")).toString();
    project.sampleRate = values.value(QStringLiteral("sampleRate")).toInt();
    project.channels = values.value(QStringLiteral("channels")).toInt();
    project.sourceSampleCount = values.value(QStringLiteral("sourceSampleCount")).toLongLong();
    const QString segmentSql = schemaVersion == 1
        ? QStringLiteral("SELECT id,text,start_sample,end_sample,auto_decision,user_decision,rule_score,reason,evidence_json FROM segments ORDER BY position")
        : QStringLiteral("SELECT id,text,start_sample,end_sample,auto_decision,user_decision,rule_score,reason,evidence_json FROM analysis_segments WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion);
    if (!query.exec(segmentSql)) {
        setError(errorMessage, query.lastError().text()); return std::nullopt;
    }
    while (query.next()) {
        RecognizedPassage passage{query.value(0).toString(), query.value(1).toString(),
            query.value(2).toLongLong(), query.value(3).toLongLong()};
        const auto automatic = decision(query.value(4).toString());
        const auto user = query.value(5).isNull() ? std::optional<RoughCutDecision>()
                                                  : decision(query.value(5).toString());
        if (passage.id.isEmpty() || passage.endSample <= passage.startSample || !automatic
            || (!query.value(5).isNull() && !user)) {
            setError(errorMessage, QStringLiteral("粗剪工程片段无效")); return std::nullopt;
        }
        RoughCutSegmentDecision result;
        result.recordingIndex = project.recording.size(); result.autoDecision = *automatic;
        result.userDecision = user; result.ruleScore = query.value(6).toDouble();
        result.reason = query.value(7).toString();
        for (const QJsonValue &value : QJsonDocument::fromJson(query.value(8).toString().toUtf8()).array())
            result.evidence.append(value.toString());
        project.recording.append(std::move(passage)); project.decisions.append(std::move(result));
    }
    if (schemaVersion == 2 && query.exec(QStringLiteral("SELECT recording_index,primary_text,funasr_text,whisper_text,funasr_failed,whisper_failed,conflict FROM auxiliary_results WHERE analysis_version=%1 ORDER BY recording_index").arg(project.analysisVersion))) {
        while (query.next()) project.auxiliaryResults.append({query.value(0).toInt(),
            query.value(1).toString(), query.value(2).toString(), query.value(3).toString(),
            query.value(4).toBool(), query.value(5).toBool(), query.value(6).toBool()});
    }
    if (project.recording.isEmpty() || project.sampleRate <= 0 || project.channels <= 0
        || project.sourceSampleCount <= 0 || project.mediaSha256.size() != 32) {
        setError(errorMessage, QStringLiteral("粗剪工程数据不完整")); return std::nullopt;
    }
    return project;
}

} // namespace subcue
