#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace subcue {

enum class ModelKind {
    Qwen3Asr,
    Qwen3ForcedAligner,
    FunAsrNano
};

class ModelLocator final {
public:
    [[nodiscard]] static QString compiledDefaultRoot();
    [[nodiscard]] static QString root(const QJsonObject &settings = {});
    [[nodiscard]] static QString folderName(ModelKind kind);
    [[nodiscard]] static QString directoryFor(ModelKind kind, const QJsonObject &settings = {});
    [[nodiscard]] static bool modelReady(const QString &directory);
    [[nodiscard]] static QString derivedRootFromLegacy(const QJsonObject &loaded);
};

} // namespace subcue
