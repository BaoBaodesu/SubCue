#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>

namespace subcue {

class AssWriter final {
public:
    [[nodiscard]] static QString build(const QList<Subtitle> &subtitles, int width, int height,
                                       const QJsonObject &settings);
    [[nodiscard]] static bool save(const QString &path, const QList<Subtitle> &subtitles,
                                   int width, int height, const QJsonObject &settings,
                                   QString *errorMessage = nullptr);
};

} // namespace subcue
