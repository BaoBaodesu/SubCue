#pragma once

#include "media/media_types.h"
#include <atomic>

namespace subcue {

class MediaProbe final {
public:
    [[nodiscard]] static ProbeResult probe(const QString &path);
    [[nodiscard]] static ProbeResult verifyFrameRate(const QString &path, const std::atomic<bool> *cancel);
};

} // namespace subcue
