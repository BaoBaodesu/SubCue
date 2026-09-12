#pragma once

#include "project/project.h"

#include <QtCore/QString>

#include <optional>

namespace subcue {

class ProjectSerializer final {
public:
    [[nodiscard]] static bool save(const QString &path, const Project &project,
                                   QString *errorMessage = nullptr);
    [[nodiscard]] static std::optional<Project> load(const QString &path,
                                                     QString *errorMessage = nullptr);
};

} // namespace subcue
