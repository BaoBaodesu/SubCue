#include "project/project_serializer.h"
#include "common/file_path_guard.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <utility>
#include <cmath>

namespace subcue {
namespace {

bool validState(const QJsonObject &state, QString *errorMessage)
{
    for (const auto &key : {QStringLiteral("words"), QStringLiteral("assets")}) {
        if (state.contains(key) && !state.value(key).isArray()) {
            if (errorMessage) *errorMessage = QStringLiteral("工程状态字段无效：%1").arg(key);
            return false;
        }
    }
    QSet<qint64> ids;
    for (const auto &value : state.value(QStringLiteral("words")).toArray()) {
        const auto word = value.toObject();
        const qint64 id = word.value(QStringLiteral("id")).toInteger(-1);
        const qint64 start = word.value(QStringLiteral("startMs")).toInteger(-1);
        const qint64 end = word.value(QStringLiteral("endMs")).toInteger(-1);
        if (!value.isObject() || id < 0 || ids.contains(id) || start < 0 || end < start
            || !word.value(QStringLiteral("text")).isString()) {
            if (errorMessage) *errorMessage = QStringLiteral("工程词级证据范围或 ID 无效");
            return false;
        }
        ids.insert(id);
    }
    for (const auto &value : state.value(QStringLiteral("assets")).toArray()) {
        if (!value.isObject() || !value.toObject().value(QStringLiteral("path")).isString()) {
            if (errorMessage) *errorMessage = QStringLiteral("工程素材引用无效");
            return false;
        }
    }
    return true;
}

QJsonObject subtitleToJson(const Subtitle &subtitle)
{
    QJsonArray wordIds;
    wordIds.append(subtitle.startWordId >= 0 ? QJsonValue(subtitle.startWordId) : QJsonValue::Null);
    wordIds.append(subtitle.endWordId >= 0 ? QJsonValue(subtitle.endWordId) : QJsonValue::Null);
    return {
        {QStringLiteral("id"), subtitle.id},
        {QStringLiteral("startUs"), subtitle.start.microseconds()},
        {QStringLiteral("endUs"), subtitle.end.microseconds()},
        {QStringLiteral("text"), subtitle.text},
        {QStringLiteral("track"), subtitle.track},
        {QStringLiteral("confidence"), subtitle.confidence},
        {QStringLiteral("source"), subtitle.source},
        {QStringLiteral("status"), subtitle.status},
        {QStringLiteral("candidateText"), subtitle.candidateText},
        {QStringLiteral("ambiguity"), subtitle.ambiguity},
        {QStringLiteral("skipReason"), subtitle.skipReason},
        {QStringLiteral("wordIds"), wordIds},
        {QStringLiteral("metadata"), subtitle.metadata},
    };
}

std::optional<Subtitle> subtitleFromJson(const QJsonObject &object, QString *errorMessage)
{
    Subtitle subtitle;
    subtitle.id = object.value(QStringLiteral("id")).toString();
    subtitle.start = MediaTime::fromMicroseconds(
        object.value(QStringLiteral("startUs")).toInteger(-1));
    subtitle.end = MediaTime::fromMicroseconds(
        object.value(QStringLiteral("endUs")).toInteger(-1));
    subtitle.text = object.value(QStringLiteral("text")).toString();
    subtitle.track = object.value(QStringLiteral("track")).toInt(0);
    subtitle.confidence = object.value(QStringLiteral("confidence")).toDouble(0.0);
    subtitle.source = object.value(QStringLiteral("source")).toString(QStringLiteral("local"));
    subtitle.status = object.value(QStringLiteral("status")).toString(QStringLiteral("UNMATCHED"));
    const QJsonArray wordIds = object.value(QStringLiteral("wordIds")).toArray();
    if (wordIds.size() > 0 && wordIds.at(0).isDouble()) subtitle.startWordId = wordIds.at(0).toInteger();
    if (wordIds.size() > 1 && wordIds.at(1).isDouble()) subtitle.endWordId = wordIds.at(1).toInteger();
    subtitle.metadata = object.value(QStringLiteral("metadata")).toObject();
    subtitle.candidateText = object.value(QStringLiteral("candidateText")).toString();
    subtitle.ambiguity = object.value(QStringLiteral("ambiguity")).toDouble(1.0);
    subtitle.skipReason = object.value(QStringLiteral("skipReason")).toString();
    if (!subtitle.isValid() || !std::isfinite(subtitle.confidence) || subtitle.confidence < 0 || subtitle.confidence > 1
        || !std::isfinite(subtitle.ambiguity) || subtitle.ambiguity < 0 || subtitle.ambiguity > 1) {
        if (errorMessage) *errorMessage = QStringLiteral("工程中存在无效字幕：%1").arg(subtitle.id);
        return std::nullopt;
    }
    return subtitle;
}

QString relativeMediaPath(const QString &projectPath, const QString &mediaPath)
{
    if (mediaPath.isEmpty()) return {};
    return QDir::fromNativeSeparators(
        QFileInfo(projectPath).absoluteDir().relativeFilePath(mediaPath));
}

} // namespace

QByteArray ProjectSerializer::fingerprint(const Project &project)
{
    QJsonObject state = project.state;
    state.remove(QStringLiteral("session"));
    QJsonArray subtitles;
    for (const SubtitleTrack &track : project.tracks) {
        QJsonArray cues;
        for (const Subtitle &cue : track.subtitles) cues.append(subtitleToJson(cue));
        subtitles.append(QJsonObject{{QStringLiteral("id"), track.id},
            {QStringLiteral("name"), track.name}, {QStringLiteral("metadata"), track.metadata},
            {QStringLiteral("subtitles"), cues}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("media"), project.mediaPath},
        {QStringLiteral("state"), state}, {QStringLiteral("metadata"), project.metadata},
        {QStringLiteral("tracks"), subtitles}}).toJson(QJsonDocument::Compact);
}

bool ProjectSerializer::save(const QString &path, const Project &project, QString *errorMessage, const std::atomic<bool> *cancel)
{
    if (!safeOutputPath(path, {project.mediaPath}, errorMessage)) return false;
    if (!validState(project.state, errorMessage)) return false;
    QJsonArray tracks;
    QSet<QString> trackIds;
    QSet<QString> subtitleIds;
    for (const SubtitleTrack &track : project.tracks) {
        if (track.id.isEmpty() || trackIds.contains(track.id)) {
            if (errorMessage) *errorMessage = QStringLiteral("字幕轨 ID 为空或重复：%1").arg(track.id);
            return false;
        }
        trackIds.insert(track.id);
        QJsonArray subtitles;
        for (const Subtitle &subtitle : track.subtitles) {
            if (!subtitle.isValid() || subtitleIds.contains(subtitle.id)) {
                if (errorMessage) *errorMessage = QStringLiteral("字幕无效或 ID 重复：%1").arg(subtitle.id);
                return false;
            }
            subtitleIds.insert(subtitle.id);
            subtitles.append(subtitleToJson(subtitle));
        }
        tracks.append(QJsonObject{
            {QStringLiteral("id"), track.id},
            {QStringLiteral("name"), track.name},
            {QStringLiteral("subtitles"), subtitles},
            {QStringLiteral("metadata"), track.metadata},
        });
    }
    QJsonObject state = project.state;
    if (state.contains(QStringLiteral("assets"))) {
        QJsonArray assets;
        for (const auto &value : state.value(QStringLiteral("assets")).toArray()) {
            auto asset = value.toObject();
            asset.insert(QStringLiteral("path"), relativeMediaPath(path, asset.value(QStringLiteral("path")).toString()));
            assets.append(asset);
        }
        state.insert(QStringLiteral("assets"), assets);
    }
    const QJsonObject root{
        {QStringLiteral("schemaVersion"), Project::CurrentSchemaVersion},
        {QStringLiteral("media"), QJsonObject{
            {QStringLiteral("path"), relativeMediaPath(path, project.mediaPath)},
            {QStringLiteral("fingerprint"), project.mediaFingerprint},
        }},
        {QStringLiteral("subtitleTracks"), tracks},
        {QStringLiteral("metadata"), project.metadata},
        {QStringLiteral("state"), state},
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (cancel && cancel->load()) {
        if (errorMessage) *errorMessage = QStringLiteral("工程保存已取消。");
        return false;
    }
    if (file.write(data) != data.size()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (cancel && cancel->load()) {
        if (errorMessage) *errorMessage = QStringLiteral("工程保存已取消。");
        return false;
    }
    if (!file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

std::optional<Project> ProjectSerializer::load(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) *errorMessage = QStringLiteral("工程 JSON 无效：%1").arg(parseError.errorString());
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    const int schema = root.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (schema < 1 || schema > Project::CurrentSchemaVersion
        || !root.value(QStringLiteral("subtitleTracks")).isArray()) {
        if (errorMessage) *errorMessage = QStringLiteral("不支持的工程格式版本");
        return std::nullopt;
    }
    if (schema >= 2 && !root.value(QStringLiteral("state")).isObject()) {
        if (errorMessage) *errorMessage = QStringLiteral("工程状态缺失或无效");
        return std::nullopt;
    }
    Project project;
    project.schemaVersion = schema;
    project.state = root.value(QStringLiteral("state")).toObject();
    if (!validState(project.state, errorMessage)) return std::nullopt;
    if (project.state.contains(QStringLiteral("assets"))) {
        QJsonArray assets;
        for (const auto &value : project.state.value(QStringLiteral("assets")).toArray()) {
            auto asset = value.toObject();
            if (!value.isObject() || !asset.value(QStringLiteral("path")).isString()) {
                if (errorMessage) *errorMessage = QStringLiteral("工程素材引用无效");
                return std::nullopt;
            }
            const QString stored = asset.value(QStringLiteral("path")).toString();
            asset.insert(QStringLiteral("path"), stored.isEmpty() ? QString()
                : QDir::cleanPath(QFileInfo(path).absoluteDir().absoluteFilePath(stored)));
            assets.append(asset);
        }
        project.state.insert(QStringLiteral("assets"), assets);
    }
    const QJsonObject media = root.value(QStringLiteral("media")).toObject();
    const QString storedPath = media.value(QStringLiteral("path")).toString();
    if (!storedPath.isEmpty()) {
        project.mediaPath = QDir::cleanPath(
            QFileInfo(path).absoluteDir().absoluteFilePath(storedPath));
    }
    project.mediaFingerprint = media.value(QStringLiteral("fingerprint")).toObject();
    project.metadata = root.value(QStringLiteral("metadata")).toObject();
    const QJsonArray tracks = root.value(QStringLiteral("subtitleTracks")).toArray();
    QSet<QString> trackIds;
    QSet<QString> subtitleIds;
    for (qsizetype trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (!tracks.at(trackIndex).isObject()) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 条字幕轨无效").arg(trackIndex + 1);
            return std::nullopt;
        }
        const QJsonObject trackObject = tracks.at(trackIndex).toObject();
        SubtitleTrack track;
        track.id = trackObject.value(QStringLiteral("id")).toString();
        track.name = trackObject.value(QStringLiteral("name")).toString();
        track.metadata = trackObject.value(QStringLiteral("metadata")).toObject();
        if (track.id.isEmpty() || trackIds.contains(track.id)) {
            if (errorMessage) *errorMessage = QStringLiteral("第 %1 条字幕轨 ID 为空或重复").arg(trackIndex + 1);
            return std::nullopt;
        }
        trackIds.insert(track.id);
        if (!trackObject.value(QStringLiteral("subtitles")).isArray()) {
            if (errorMessage) *errorMessage = QStringLiteral("字幕轨内容缺失或无效");
            return std::nullopt;
        }
        const QJsonArray subtitles = trackObject.value(QStringLiteral("subtitles")).toArray();
        for (const QJsonValue &value : subtitles) {
            if (!value.isObject()) {
                if (errorMessage) *errorMessage = QStringLiteral("字幕数据无效");
                return std::nullopt;
            }
            auto subtitle = subtitleFromJson(value.toObject(), errorMessage);
            if (!subtitle) return std::nullopt;
            if (subtitleIds.contains(subtitle->id)) {
                if (errorMessage) *errorMessage = QStringLiteral("字幕 ID 重复：%1").arg(subtitle->id);
                return std::nullopt;
            }
            subtitleIds.insert(subtitle->id);
            track.subtitles.append(std::move(*subtitle));
        }
        project.tracks.append(std::move(track));
    }
    return project;
}

} // namespace subcue
