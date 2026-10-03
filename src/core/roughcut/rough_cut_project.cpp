#include "roughcut/rough_cut_project.h"
#include "common/file_path_guard.h"
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryFile>
#include <QtCore/QDataStream>
#include <QtCore/QSet>
#include <cmath>

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
        if (!writable) db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
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

static bool saveDatabase(const QString &path, const RoughCutProject &project,
                                     QString *errorMessage)
{
    if (project.recording.size() != project.decisions.size()
        || (!project.mediaPath.isEmpty() && (project.sampleRate <= 0
        || project.channels <= 0 || project.sourceSampleCount <= 0 || project.mediaSha256.size() != 32)))
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
        {QStringLiteral("schemaVersion"), RoughCutProject::CurrentSchemaVersion}, {QStringLiteral("analysisVersion"), project.analysisVersion},
        {QStringLiteral("mediaPath"), relative(path, project.mediaPath)},
        {QStringLiteral("mediaSha256"), project.mediaSha256},
        {QStringLiteral("scriptPath"), relative(path, project.scriptPath)},
        {QStringLiteral("scriptText"), project.scriptText},
        {QStringLiteral("sampleRate"), project.sampleRate}, {QStringLiteral("channels"), project.channels},
        {QStringLiteral("sourceSampleCount"), project.sourceSampleCount},
        {QStringLiteral("state"), QJsonDocument(project.state).toJson(QJsonDocument::Compact)}};
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

bool RoughCutProjectSerializer::save(const QString &path, const RoughCutProject &project,
    QString *errorMessage, const std::atomic<bool> *cancel)
{
    if (!safeOutputPath(path, {project.mediaPath, project.scriptPath}, errorMessage)) return false;
    QTemporaryFile candidate(QFileInfo(path).absoluteDir().filePath(QStringLiteral(".subcue-save-XXXXXX")));
    if (!candidate.open()) return setError(errorMessage, candidate.errorString());
    const QString temporaryPath = candidate.fileName();
    candidate.close();
    if (!saveDatabase(temporaryPath, project, errorMessage)) return false;
    if (!load(temporaryPath, errorMessage)) return false;
    QFile input(temporaryPath);
    QSaveFile output(path);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return setError(errorMessage, input.isOpen() ? output.errorString() : input.errorString());
    while (!input.atEnd()) {
        if (cancel && cancel->load()) return setError(errorMessage, QStringLiteral("工程保存已取消。"));
        const QByteArray data = input.read(1024 * 1024);
        if (data.isEmpty() || output.write(data) != data.size())
            return setError(errorMessage, QStringLiteral("工程写入失败，原文件已保留。"));
    }
    if (cancel && cancel->load()) return setError(errorMessage, QStringLiteral("工程保存已取消。"));
    return output.commit() || setError(errorMessage, output.errorString());
}

QByteArray RoughCutProjectSerializer::fingerprint(const RoughCutProject &project)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    QJsonObject state = project.state;
    state.remove(QStringLiteral("session"));
    state.remove(QStringLiteral("mediaSize"));
    state.remove(QStringLiteral("mediaMtime"));
    state.remove(QStringLiteral("frameRateError"));
    auto mediaInfo = state.value(QStringLiteral("mediaInfo")).toObject();
    mediaInfo.remove(QStringLiteral("cfrVerified"));
    mediaInfo.remove(QStringLiteral("vfr"));
    state.insert(QStringLiteral("mediaInfo"), mediaInfo);
    out << project.mediaPath << project.scriptPath << project.scriptText << project.analysisVersion
        << project.sampleRate << project.channels << project.sourceSampleCount
        << QJsonDocument(state).toJson(QJsonDocument::Compact) << project.recording.size();
    for (const auto &p : project.recording)
        out << p.id << p.text << p.startSample << p.endSample << p.silenceBeforeSamples
            << p.silenceAfterSamples << p.vadConfidence << p.audioComplete << p.boundaryTrustworthy
            << p.scriptLineIndex << p.takeGroupId << p.scriptLineEndIndex << p.textSimilarity
            << p.editSimilarity << p.continuousCoverage << p.scriptTokenStart << p.scriptTokenEnd << p.preciseTiming;
    for (const auto &d : project.decisions)
        out << d.recordingIndex << int(d.autoDecision) << (d.userDecision ? int(*d.userDecision) : -1)
            << d.ruleScore << d.reason << d.evidence << d.takeGroupId << d.replacementRecordingIndex
            << d.bestTake << int(d.failureType) << d.modelProbability << d.modelVersion << d.decisionSource;
    for (const auto &a : project.auxiliaryResults)
        out << a.recordingIndex << a.primaryText << a.funAsrText << a.whisperText
            << a.funAsrFailed << a.whisperFailed << a.conflict;
    return bytes;
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
    if (schemaVersion < 1 || schemaVersion > RoughCutProject::CurrentSchemaVersion) {
        setError(errorMessage, QStringLiteral("不支持的粗剪工程版本")); return std::nullopt;
    }
    RoughCutProject project;
    project.schemaVersion = schemaVersion;
    if (schemaVersion >= 6) {
        QJsonParseError parseError;
        const auto state = QJsonDocument::fromJson(values.value(QStringLiteral("state")).toByteArray(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !state.isObject()) {
            setError(errorMessage, QStringLiteral("粗剪工程状态无效")); return std::nullopt;
        }
        project.state = state.object();
    } else {
        project.state.insert(QStringLiteral("legacyEvidenceMissing"), true);
    }
    project.analysisVersion = values.value(QStringLiteral("analysisVersion"), schemaVersion == 1 ? 1 : 0).toInt();
    project.mediaPath = absolute(path, values.value(QStringLiteral("mediaPath")).toString());
    project.mediaSha256 = values.value(QStringLiteral("mediaSha256")).toByteArray();
    project.scriptPath = absolute(path, values.value(QStringLiteral("scriptPath")).toString());
    project.scriptText = values.value(QStringLiteral("scriptText")).toString();
    project.sampleRate = values.value(QStringLiteral("sampleRate")).toInt();
    project.channels = values.value(QStringLiteral("channels")).toInt();
    project.sourceSampleCount = values.value(QStringLiteral("sourceSampleCount")).toLongLong();
    const QString segmentSql = schemaVersion == 1
        ? QStringLiteral("SELECT id,text,start_sample,end_sample,auto_decision,user_decision,rule_score,reason,evidence_json,position FROM segments ORDER BY position")
        : QStringLiteral("SELECT id,text,start_sample,end_sample,auto_decision,user_decision,rule_score,reason,evidence_json,position FROM analysis_segments WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion);
    if (!query.exec(segmentSql)) {
        setError(errorMessage, query.lastError().text()); return std::nullopt;
    }
    QSet<QString> segmentIds;
    while (query.next()) {
        RecognizedPassage passage{query.value(0).toString(), query.value(1).toString(),
            query.value(2).toLongLong(), query.value(3).toLongLong()};
        const auto automatic = decision(query.value(4).toString());
        const auto user = query.value(5).isNull() ? std::optional<RoughCutDecision>()
                                                  : decision(query.value(5).toString());
        if (query.value(9).toInt() != project.recording.size() || passage.id.isEmpty() || segmentIds.contains(passage.id) || passage.startSample < 0
            || passage.endSample > project.sourceSampleCount || passage.endSample <= passage.startSample || !automatic
            || (!query.value(5).isNull() && !user)) {
            setError(errorMessage, QStringLiteral("粗剪工程片段无效")); return std::nullopt;
        }
        segmentIds.insert(passage.id);
        RoughCutSegmentDecision result;
        result.recordingIndex = project.recording.size(); result.autoDecision = *automatic;
        result.userDecision = user; result.ruleScore = query.value(6).toDouble();
        result.reason = query.value(7).toString();
        QJsonParseError evidenceError;
        const auto evidence = QJsonDocument::fromJson(query.value(8).toString().toUtf8(), &evidenceError);
        if (evidenceError.error != QJsonParseError::NoError || !evidence.isArray()) {
            setError(errorMessage, QStringLiteral("判断证据无效")); return std::nullopt;
        }
        for (const auto &value : evidence.array()) {
            if (!value.isString()) { setError(errorMessage, QStringLiteral("判断证据无效")); return std::nullopt; }
            result.evidence.append(value.toString());
        }
        project.recording.append(std::move(passage)); project.decisions.append(std::move(result));
    }
    if (schemaVersion >= 3) {
        if (!query.exec(QStringLiteral("SELECT position,silence_before,silence_after,vad_confidence,audio_complete,boundary_trustworthy,script_line,take_group,best_take FROM analysis_segment_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
            setError(errorMessage, query.lastError().text()); return std::nullopt;
        }
        QSet<int> positions;
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size() || positions.contains(position)) {
                setError(errorMessage, QStringLiteral("粗剪工程明细引用无效")); return std::nullopt;
            }
            positions.insert(position);
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
        if (positions.size() != project.recording.size()) {
            setError(errorMessage, QStringLiteral("粗剪工程明细不完整")); return std::nullopt;
        }
    }
    if (schemaVersion >= 4) {
        if (!query.exec(QStringLiteral("SELECT position,script_line_end,failure_type,model_probability,model_version,decision_source FROM analysis_decision_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
            setError(errorMessage, query.lastError().text()); return std::nullopt;
        }
        QSet<int> positions;
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size() || positions.contains(position)) {
                setError(errorMessage, QStringLiteral("粗剪工程明细引用无效")); return std::nullopt;
            }
            positions.insert(position);
            if (!QStringList{QStringLiteral("NONE"), QStringLiteral("RETAKE"), QStringLiteral("INTERRUPTED"), QStringLiteral("DUPLICATE"), QStringLiteral("WRONG_TAKE"), QStringLiteral("FILLER")}.contains(query.value(2).toString())) {
                setError(errorMessage, QStringLiteral("失败类型无效")); return std::nullopt;
            }
            project.recording[position].scriptLineEndIndex = query.value(1).toInt();
            project.decisions[position].failureType = failureType(query.value(2).toString());
            project.decisions[position].modelProbability = query.value(3).toDouble();
            project.decisions[position].modelVersion = query.value(4).toString();
            project.decisions[position].decisionSource = query.value(5).toString();
        }
        if (positions.size() != project.recording.size()) {
            setError(errorMessage, QStringLiteral("粗剪工程明细不完整")); return std::nullopt;
        }
    }
    if (schemaVersion >= 4) {
        if (!query.exec(QStringLiteral("SELECT position,text_similarity,edit_similarity,continuous_coverage FROM analysis_match_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
            setError(errorMessage, query.lastError().text()); return std::nullopt;
        }
        QSet<int> positions;
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size() || positions.contains(position)) {
                setError(errorMessage, QStringLiteral("粗剪工程明细引用无效")); return std::nullopt;
            }
            positions.insert(position);
            project.recording[position].textSimilarity = query.value(1).toDouble();
            project.recording[position].editSimilarity = query.value(2).toDouble();
            project.recording[position].continuousCoverage = query.value(3).toDouble();
        }
        if (positions.size() != project.recording.size()) {
            setError(errorMessage, QStringLiteral("粗剪工程明细不完整")); return std::nullopt;
        }
    }
    if (schemaVersion >= 5) {
        if (!query.exec(QStringLiteral("SELECT position,token_start,token_end,precise_timing,replacement_index FROM analysis_alignment_details WHERE analysis_version=%1 ORDER BY position").arg(project.analysisVersion))) {
            setError(errorMessage, query.lastError().text()); return std::nullopt;
        }
        QSet<int> positions;
        while (query.next()) {
            const int position = query.value(0).toInt();
            if (position < 0 || position >= project.recording.size() || positions.contains(position)) {
                setError(errorMessage, QStringLiteral("粗剪工程明细引用无效")); return std::nullopt;
            }
            positions.insert(position);
            project.recording[position].scriptTokenStart = query.value(1).toInt();
            project.recording[position].scriptTokenEnd = query.value(2).toInt();
            project.recording[position].preciseTiming = query.value(3).toBool();
            project.decisions[position].replacementRecordingIndex = query.value(4).toInt();
        }
        if (positions.size() != project.recording.size()) {
            setError(errorMessage, QStringLiteral("粗剪工程明细不完整")); return std::nullopt;
        }
    }
    if (schemaVersion >= 2) {
        if (!query.exec(QStringLiteral("SELECT recording_index,primary_text,funasr_text,whisper_text,funasr_failed,whisper_failed,conflict FROM auxiliary_results WHERE analysis_version=%1 ORDER BY recording_index").arg(project.analysisVersion))) {
            setError(errorMessage, query.lastError().text()); return std::nullopt;
        }
        while (query.next()) project.auxiliaryResults.append({query.value(0).toInt(),
            query.value(1).toString(), query.value(2).toString(), query.value(3).toString(),
            query.value(4).toBool(), query.value(5).toBool(), query.value(6).toBool()});
    }
    if (project.analysisVersion <= 0 || project.recording.size() != project.decisions.size()
        || (!project.mediaPath.isEmpty() && (project.sampleRate <= 0
        || project.channels <= 0 || project.sourceSampleCount <= 0 || project.mediaSha256.size() != 32))) {
        setError(errorMessage, QStringLiteral("粗剪工程数据不完整")); return std::nullopt;
    }
    for (const auto &result : project.auxiliaryResults)
        if (result.recordingIndex < 0 || result.recordingIndex >= project.recording.size()) {
            setError(errorMessage, QStringLiteral("辅助识别引用无效")); return std::nullopt;
        }
    for (const auto &result : project.decisions)
        if (!std::isfinite(result.ruleScore) || !std::isfinite(result.modelProbability)
            || result.replacementRecordingIndex < -1 || result.replacementRecordingIndex >= project.recording.size()) {
            setError(errorMessage, QStringLiteral("判断参数或引用无效")); return std::nullopt;
        }
    qint64 timelineEnd = 0;
    for (const auto &key : {QStringLiteral("timeline"), QStringLiteral("words")}) {
        if (project.state.contains(key) && !project.state.value(key).isArray()) {
            setError(errorMessage, QStringLiteral("粗剪工程状态字段无效：%1").arg(key)); return std::nullopt;
        }
    }
    QSet<qint64> wordIds;
    for (const auto &value : project.state.value(QStringLiteral("words")).toArray()) {
        const auto word = value.toObject();
        const qint64 id = word.value(QStringLiteral("id")).toInteger(-1);
        const qint64 start = word.value(QStringLiteral("startMs")).toInteger(-1);
        const qint64 end = word.value(QStringLiteral("endMs")).toInteger(-1);
        if (!value.isObject() || id < 0 || wordIds.contains(id) || start < 0 || end < start
            || !word.value(QStringLiteral("text")).isString()) {
            setError(errorMessage, QStringLiteral("粗剪词级证据范围或 ID 无效")); return std::nullopt;
        }
        wordIds.insert(id);
    }
    QSet<int> timelineRows;
    for (const auto &value : project.state.value(QStringLiteral("timeline")).toArray()) {
        const auto clip = value.toObject();
        const qint64 start = clip.value(QStringLiteral("start")).toInteger(-1);
        const qint64 end = clip.value(QStringLiteral("end")).toInteger(-1);
        const qint64 position = clip.value(QStringLiteral("timelineStart")).toInteger(-1);
        const int row = clip.value(QStringLiteral("recording")).toInt(-1);
        const int decisionValue = clip.value(QStringLiteral("decision")).toInt(-1);
        if (!value.isObject() || start < 0 || end <= start || end > project.sourceSampleCount
            || position < timelineEnd || row < 0 || row >= project.recording.size()
            || timelineRows.contains(row) || decisionValue < 0 || decisionValue > 2) {
            setError(errorMessage, QStringLiteral("已保存时间线范围或引用无效")); return std::nullopt;
        }
        timelineRows.insert(row);
        timelineEnd = position + end - start;
    }
    return project;
}

} // namespace subcue
