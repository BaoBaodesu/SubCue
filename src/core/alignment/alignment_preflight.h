#pragma once

#include "common/provider_types.h"
#include "media/media_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace subcue {

struct PreflightState final {
    bool asrCredentialExists = false;
    bool aiCredentialExists = false;
    bool asrVerified = false;
    bool aiVerified = false;
    bool aiProviderOverride = false;
};

class AlignmentPreflight final {
public:
    [[nodiscard]] static QVector<PreflightIssue> check(
        const QString &mediaPath,
        const MediaInfo &mediaInfo,
        const QStringList &lines,
        const QJsonObject &settings,
        const PreflightState &state);
};

} // namespace subcue
