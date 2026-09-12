#pragma once

#include "common/media_time.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace subcue {

inline constexpr AVRational kMicrosecondTimeBase{1, 1'000'000};

[[nodiscard]] inline MediaTime mediaTimeFromTimestamp(qint64 timestamp, AVRational timeBase) noexcept
{
    if (timestamp == AV_NOPTS_VALUE || timeBase.num <= 0 || timeBase.den <= 0) {
        return MediaTime::fromMicroseconds(-1);
    }
    return MediaTime::fromMicroseconds(av_rescale_q(timestamp, timeBase, kMicrosecondTimeBase));
}

[[nodiscard]] inline qint64 timestampFromMediaTime(MediaTime time, AVRational timeBase) noexcept
{
    if (timeBase.num <= 0 || timeBase.den <= 0) {
        return AV_NOPTS_VALUE;
    }
    return av_rescale_q(time.microseconds(), kMicrosecondTimeBase, timeBase);
}

} // namespace subcue
