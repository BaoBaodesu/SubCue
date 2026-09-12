#pragma once

#include <QtCore/QStringList>

namespace subcue {

class TxtImporter final {
public:
    [[nodiscard]] static QStringList parse(const QString &content);
};

} // namespace subcue
