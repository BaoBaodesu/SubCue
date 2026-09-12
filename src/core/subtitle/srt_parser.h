#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace subcue {

class SrtParser final {
public:
    [[nodiscard]] static QStringList parseTextOnly(const QString &content);
    [[nodiscard]] static std::optional<QList<Subtitle>> parseTimed(
        const QString &content, QString *errorMessage = nullptr);
};

} // namespace subcue
