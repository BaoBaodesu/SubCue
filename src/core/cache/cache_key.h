#pragma once

#include "common/app_error.h"
#include "common/media_time.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QSize>

namespace subcue {

inline constexpr qint64 kCacheKeyChunkBytes = 64 * 1024;

struct CacheKey final {
    QString canonicalPath;
    qint64 size = -1;
    qint64 mtimeMs = -1;
    int streamIndex = -1;
    QByteArray headHash;
    QByteArray tailHash;
    QString qualifier;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] QString id() const;
    [[nodiscard]] bool operator==(const CacheKey &other) const noexcept = default;
};

[[nodiscard]] QString waveformQualifier(int sampleRate);
[[nodiscard]] QString thumbnailQualifier(MediaTime pts, QSize size);

[[nodiscard]] CacheKey makeCacheKey(
    const QString &path,
    int streamIndex,
    const QString &qualifier,
    AppError *error = nullptr);

} // namespace subcue
