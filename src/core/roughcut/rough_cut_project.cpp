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

#include <algorithm>
#include <atomic>

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

RoughCutFailureType failureType(const QString &value)
{
    if (value == QLatin1String("RETAKE")) return RoughCutFailureType::Retake;
    if (value == QLatin1String("INTERRUPTED")) return RoughCutFailureType::Interrupted;
    if (value == QLatin1String("DUPLICATE")) return RoughCutFailureType::Duplicate;
    if (value == QLatin1String("WRONG_TAKE")) return RoughCutFailureType::WrongTake;
    if (value == QLatin1String("FILLER")) return RoughCutFailureType::Filler;
    return RoughCutFailureType::None;
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

QByteArray RoughCutProjectSerializer::mediaSha256(const QString &path, QString *errorMessage,
    const std::atomic<bool> *cancel, const std::function<void(qint64, qint64)> &progress)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 total = std::max<qint64>(0, file.size());
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    qint64 done = 0;
    while (!file.atEnd()) {
        if (cancel && cancel->load()) {
            setError(errorMessage, QStringLiteral("校验已取消。"));
            return {};
        }
        const qint64 read = file.read(chunk.data(), chunk.size());
        if (read < 0) {
            setError(errorMessage, file.errorString());
            return {};
        }
        if (read == 0) break;
        hash.addData(QByteArrayView(chunk.constData(), qsizetype(read)));
        done += read;
        if (progress) progress(done, total);
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
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_segment_details(analysis_version INTEGER NOT NULL,position INTEGER NOT NULL,silence_before INTEGER NOT NULL,silence_after INTEGER NOT NULL,vad_confidence REAL NOT NULL,audio_complete INTEGER NOT NULL,boundary_trustworthy INTEGER NOT NULL,script_line INTEGER NOT NULL,take_group INTEGER NOT NULL,best_take INTEGER NOT NULL,PRIMARY KEY(analysis_version,position))"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_decision_details(analysis_version INTEGER NOT NULL,position INTEGER NOT NULL,script_line_end INTEGER NOT NULL,failure_type TEXT NOT NULL,model_probability REAL NOT NULL,model_version TEXT NOT NULL,decision_source TEXT NOT NULL,PRIMARY KEY(analysis_version,position))"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_match_details(analysis_version INTEGER NOT NULL,position INTEGER NOT NULL,text_similarity REAL NOT NULL,edit_similarity REAL NOT NULL,continuous_coverage REAL NOT NULL,PRIMARY KEY(analysis_version,position))"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS analysis_alignment_details(analysis_version INTEGER NOT NULL,position INTEGER NOT NULL,token_start INTEGER NOT NULL,token_end INTEGER NOT NULL,precise_timing INTEGER NOT NULL,replacement_index INTEGER NOT NULL,PRIMARY KEY(analysis_version,position))"))
        || !query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS auxiliary_results(analysis_version INTEGER NOT NULL,recording_index INTEGER NOT NULL,primary_text TEXT NOT NULL,funasr_text TEXT NOT NULL,whisper_text TEXT NOT NULL,funasr_failed INTEGER NOT NULL,whisper_failed INTEGER NOT NULL,conflict INTEGER NOT NULL,PRIMARY KEY(analysis_version,recording_index))"))
        || !query.exec(QStringLiteral("DELETE FROM metadata"))) {
        holder.db.rollback(); return setError(errorMessage, query.lastError().text());
    }
    query.prepare(QStringLiteral("INSERT INTO metadata VALUES(?,?)"));
    const QList<QPair<QString, QVariant>> values{
        {QStringLiteral("schemaVersion"), 5}, {QStringLiteral("analysisVersion"), project.analysisVersion},
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
    query.prepare(QStringLiteral(
        "INSERT INTO analysis_segments VALUES(?,?,COALESCE(?,''),COALESCE(?,''),?,?,?,?,?,COALESCE(?,''),COALESCE(?,''))"));
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
    query.prepare(QStringLiteral("DELETE FROM analysis_decision_details WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO analysis_decision_details VALUES(?,?,?,?,?,COALESCE(?,''),COALESCE(?,''))"));
    for (int index = 0; index < project.recording.size(); ++index) {
        const auto &passage = project.recording.at(index);
        const auto &result = project.decisions.at(index);
        query.bindValue(0, project.analysisVersion); query.bindValue(1, index);
        query.bindValue(2, passage.scriptLineEndIndex);
        query.bindValue(3, roughCutFailureName(result.failureType));
        query.bindValue(4, result.modelProbability); query.bindValue(5, result.modelVersion);
        query.bindValue(6, result.decisionSource);
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    query.prepare(QStringLiteral("DELETE FROM analysis_match_details WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO analysis_match_details VALUES(?,?,?,?,?)"));
    for (int index = 0; index < project.recording.size(); ++index) {
        const auto &passage = project.recording.at(index);
        query.bindValue(0, project.analysisVersion); query.bindValue(1, index);
        query.bindValue(2, passage.textSimilarity); query.bindValue(3, passage.editSimilarity);
        query.bindValue(4, passage.continuousCoverage);
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    query.prepare(QStringLiteral("DELETE FROM analysis_alignment_details WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO analysis_alignment_details VALUES(?,?,?,?,?,?)"));
    for (int index = 0; index < project.recording.size(); ++index) {
        const auto &passage = project.recording.at(index);
        query.bindValue(0, project.analysisVersion); query.bindValue(1, index);
        query.bindValue(2, passage.scriptTokenStart); query.bindValue(3, passage.scriptTokenEnd);
        query.bindValue(4, passage.preciseTiming);
        query.bindValue(5, project.decisions.at(index).replacementRecordingIndex);
        if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    }
    query.prepare(QStringLiteral("DELETE FROM analysis_segment_details WHERE analysis_version=?"));
    query.bindValue(0, project.analysisVersion);
    if (!query.exec()) { holder.db.rollback(); return setError(errorMessage, query.lastError().text()); }
    query.prepare(QStringLiteral("INSERT INTO analysis_segment_details VALUES(?,?,?,?,?,?,?,?,?,?)"));
    for (int index = 0; index < project.recording.size(); ++index) {
        const auto &passage = project.recording.at(index);
        const auto &result = project.decisions.at(index);
        query.bindValue(0, project.analysisVersion); query.bindValue(1, index);
        query.bindValue(2, passage.silenceBeforeSamples); query.bindValue(3, passage.silenceAfterSamples);
        query.bindValue(4, passage.vadConfidence); query.bindValue(5, passage.audioComplete);
        query.bindValue(6, passage.boundaryTrustworthy); query.bindValue(7, passage.scriptLineIndex);
        query.bindValue(8, result.takeGroupId); query.bindValue(9, result.bestTake);
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
    if (schemaVersion < 1 || schemaVersion > 5) {
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
    if (schemaVersion >= 3 && query.exec(QStringLiteral("SELECT position,silence_before,silence_after,vad_confidence,audio_complete,boundary_trustworthy,script_line,take_group,best_take FROM analysis_segment_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size()) continue;
            project.recording[position].silenceBeforeSamples = query.value(1).toLongLong();
            project.recording[position].silenceAfterSamples = query.value(2).toLongLong();
            project.recording[position].vadConfidence = query.value(3).toDouble();
            project.recording[position].audioComplete = query.value(4).toBool();
            project.recording[position].boundaryTrustworthy = query.value(5).toBool();
            project.recording[position].scriptLineIndex = query.value(6).toInt();
            project.recording[position].takeGroupId = query.value(7).toInt();
            project.decisions[position].takeGroupId = query.value(7).toInt();
            project.decisions[position].bestTake = query.value(8).toBool();
        }
    }
    if (schemaVersion >= 4 && query.exec(QStringLiteral("SELECT position,script_line_end,failure_type,model_probability,model_version,decision_source FROM analysis_decision_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size()) continue;
            project.recording[position].scriptLineEndIndex = query.value(1).toInt();
            project.decisions[position].failureType = failureType(query.value(2).toString());
            project.decisions[position].modelProbability = query.value(3).toDouble();
            project.decisions[position].modelVersion = query.value(4).toString();
            project.decisions[position].decisionSource = query.value(5).toString();
        }
    }
    if (schemaVersion >= 4 && query.exec(QStringLiteral("SELECT position,text_similarity,edit_similarity,continuous_coverage FROM analysis_match_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size()) continue;
            project.recording[position].textSimilarity = query.value(1).toDouble();
            project.recording[position].editSimilarity = query.value(2).toDouble();
            project.recording[position].continuousCoverage = query.value(3).toDouble();
        }
    }
    if (schemaVersion >= 5 && query.exec(QStringLiteral("SELECT position,token_start,token_end,precise_timing,replacement_index FROM analysis_alignment_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size()) continue;
            project.recording[position].scriptTokenStart = query.value(1).toInt();
            project.recording[position].scriptTokenEnd = query.value(2).toInt();
            project.recording[position].preciseTiming = query.value(3).toBool();
            project.decisions[position].replacementRecordingIndex = query.value(4).toInt();
        }
    }
    if (schemaVersion >= 2 && query.exec(QStringLiteral("SELECT recording_index,primary_text,funasr_text,whisper_text,funasr_failed,whisper_failed,conflict FROM auxiliary_results WHERE analysis_version=%1 ORDER BY recording_index").arg(project.analysisVersion))) {
        while (query.next()) project.auxiliaryResults.append({query.value(0).toInt(),
            query.value(1).toString(), query.value(2).toString(), query.value(3).toString(),
            query.value(4).toBool(), query.value(5).toBool(), query.value(6).toBool()});
    }
    if (project.recording.size() != project.decisions.size() || project.sampleRate <= 0
        || project.channels <= 0 || project.sourceSampleCount <= 0 || project.mediaSha256.size() != 32) {
        setError(errorMessage, QStringLiteral("粗剪工程数据不完整")); return std::nullopt;
    }
    return project;
}

} // namespace subcue
