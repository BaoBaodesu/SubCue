#pragma once

#include "settings/model_locator.h"

#include <QtCore/QString>
#include <QtCore/QVector>

namespace subcue {

struct AsrProviderInfo final {
    QString id;
    QString name;
    bool local = false;
    ModelKind modelKind = ModelKind::Qwen3Asr;
    QString modelId;
    QString modelName;
};

class AsrProviderCatalog final {
public:
    [[nodiscard]] static QVector<AsrProviderInfo> providers();
    [[nodiscard]] static AsrProviderInfo byId(const QString &providerId);
    [[nodiscard]] static QString displayName(const QString &providerId);
};

} // namespace subcue
