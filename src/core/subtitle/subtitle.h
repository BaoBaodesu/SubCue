#pragma once

#include "common/media_time.h"

#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QUuid>

namespace subcue {

struct Subtitle final {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    MediaTime start;
    MediaTime end;
    QString text;
    int track = 0;
    double confidence = 0.0;
    QString source = QStringLiteral("local");
    QString status = QStringLiteral("UNMATCHED");
    qint64 startWordId = -1;
    qint64 endWordId = -1;
    QString candidateText;
    double ambiguity = 1.0;
    QString skipReason;
    QJsonObject metadata;

    [[nodiscard]] bool isTimed() const noexcept
    {
        return status != QStringLiteral("SKIPPED_NO_AUDIO") && end > start;
    }

    [[nodiscard]] bool isExportable() const
    {
        return isTimed() && !text.trimmed().isEmpty();
    }

    [[nodiscard]] bool isValid() const noexcept
    {
        return !id.isEmpty() && start.microseconds() >= 0 && end >= start && track >= 0;
    }
};

} // namespace subcue

Q_DECLARE_METATYPE(subcue::Subtitle)
