#pragma once

#include "ai/ai_review_types.h"
#include "settings/credential_store.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace subcue {
class CloudConfigStore;

class OmniReviewSettingsStore final {
public:
    [[nodiscard]] static OmniReviewSettings fromJson(const QJsonObject &settings);
    [[nodiscard]] static bool resolveTask(const CloudConfigStore &store, const QString &taskId,
        const QJsonObject &defaults, OmniReviewSettings *settings, QString *key, QString *error);
    [[nodiscard]] static QJsonObject toJsonPatch(const OmniReviewSettings &settings);
    [[nodiscard]] static QString resolveApiKey(
        const QString &uiKey = {},
        const ICredentialStore *store = nullptr);
    [[nodiscard]] static AiKeySource resolveApiKeySource(
        const QString &uiKey = {},
        const ICredentialStore *store = nullptr,
        bool ignoreSaved = false);
    [[nodiscard]] static QString resolveApiKeyLabel(
        const QString &uiKey = {},
        const ICredentialStore *store = nullptr,
        bool ignoreSaved = false);
};

} // namespace subcue
