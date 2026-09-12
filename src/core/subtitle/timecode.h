#pragma once

#include "common/media_time.h"

#include <QtCore/QString>

#include <optional>

namespace subcue::timecode {

[[nodiscard]] QString formatSrt(MediaTime time);
[[nodiscard]] QString formatAss(MediaTime time);
[[nodiscard]] std::optional<MediaTime> parseSrt(const QString &value);

} // namespace subcue::timecode
