#pragma once

#include "roughcut/rough_cut_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace subcue {

class XmemlExporter final {
public:
    [[nodiscard]] static bool validate(const RoughCutExportRequest &request,
                                       QString *errorMessage = nullptr);
    [[nodiscard]] static QByteArray build(const RoughCutExportRequest &request);
    [[nodiscard]] static bool save(const QString &path, const RoughCutExportRequest &request,
                                   QString *errorMessage = nullptr);
};

} // namespace subcue
