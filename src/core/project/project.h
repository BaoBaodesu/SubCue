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
    int schemaVersion = 1;
    QString mediaPath;
    QJsonObject mediaFingerprint;
    QList<SubtitleTrack> tracks;
    QJsonObject metadata;
};

} // namespace subcue
