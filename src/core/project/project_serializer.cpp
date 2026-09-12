#include "project/project_serializer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

#include <utility>

namespace subcue {
namespace {

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
    if (!subtitle.isValid()) {
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

bool ProjectSerializer::save(const QString &path, const Project &project, QString *errorMessage)
{
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
    const QJsonObject root{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("media"), QJsonObject{
            {QStringLiteral("path"), relativeMediaPath(path, project.mediaPath)},
            {QStringLiteral("fingerprint"), project.mediaFingerprint},
        }},
        {QStringLiteral("subtitleTracks"), tracks},
        {QStringLiteral("metadata"), project.metadata},
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
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
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) {
        if (errorMessage) *errorMessage = QStringLiteral("不支持的工程格式版本");
        return std::nullopt;
    }
    Project project;
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
