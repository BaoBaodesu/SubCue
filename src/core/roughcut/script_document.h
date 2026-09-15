#pragma once

#include "common/app_error.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <variant>
#include <atomic>

namespace subcue {

struct ScriptLine final {
    int lineNumber = 0;
    int paragraph = 0;
    bool table = false;
    QString text;
};

struct ScriptDocument final {
    QVector<ScriptLine> lines;
};

using ScriptDocumentResult = std::variant<ScriptDocument, AppError>;

class ScriptDocumentImporter final {
public:
    [[nodiscard]] static ScriptDocumentResult loadTxt(const QString &path);
    [[nodiscard]] static ScriptDocumentResult loadDocx(
        const QString &path,
        const QString &inferenceExecutable = {},
        const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] static ScriptDocumentResult fromWorkerResult(const QJsonObject &result);
};

} // namespace subcue
