#pragma once

#include "common/app_error.h"
#include "common/media_time.h"
#include "media/media_types.h"

#include <QtCore/QSize>
#include <QtGui/QImage>

#include <atomic>
#include <cstdint>

namespace subcue {

class ThumbnailLoader final {
public:
    using Result = MediaResult<QImage>;

    [[nodiscard]] Result decode(
        const QString &path,
        MediaTime pts,
        QSize size,
        int streamIndex = -1,
        const std::atomic<bool> *cancel = nullptr,
        const std::atomic<quint64> *generation = nullptr,
        quint64 expectedGeneration = 0) const;
};

} // namespace subcue
