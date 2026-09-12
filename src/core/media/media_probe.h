#pragma once

#include "media/media_types.h"

namespace subcue {

class MediaProbe final {
public:
    [[nodiscard]] static ProbeResult probe(const QString &path);
};

} // namespace subcue
