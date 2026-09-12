#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QList>
#include <QtCore/QString>

namespace subcue {

class SrtWriter final {
public:
    [[nodiscard]] static QString build(const QList<Subtitle> &subtitles);
    [[nodiscard]] static bool save(const QString &path, const QList<Subtitle> &subtitles,
                                   QString *errorMessage = nullptr);
};

} // namespace subcue
