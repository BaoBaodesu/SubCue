#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>

namespace subcue {

struct SubtitleTrack final {
    QString id;
    QString name;
    QList<Subtitle> subtitles;
    QJsonObject metadata;
};

struct Project final {
    static constexpr int CurrentSchemaVersion = 2;
    int schemaVersion = CurrentSchemaVersion;
    QString mediaPath;
    QJsonObject mediaFingerprint;
    QList<SubtitleTrack> tracks;
    QJsonObject metadata;
    QJsonObject state;
};

} // namespace subcue
