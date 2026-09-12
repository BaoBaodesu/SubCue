#include "asr/audio_chunk_plan.h"

#include "alignment/python_round.h"

#include <algorithm>
#include <cmath>

namespace subcue {

QVector<AudioChunkWindow> AudioChunkPlanner::plan(double durationSeconds)
{
    durationSeconds = std::max(0.0, durationSeconds);
    const int count = std::max(1,
        static_cast<int>(std::floor((durationSeconds + 269.999) / kAsrChunkDurationSeconds)));
    QVector<AudioChunkWindow> windows;
    windows.reserve(count);
    for (int index = 0; index < count; ++index) {
        const double start = std::max(0.0,
            static_cast<double>(index) * kAsrChunkDurationSeconds
                - (index == 0 ? 0.0 : kAsrChunkOverlapSeconds));
        const double end = std::min(durationSeconds,
            (static_cast<double>(index) + 1.0) * kAsrChunkDurationSeconds + kAsrChunkOverlapSeconds);
        windows.push_back(AudioChunkWindow{
            start,
            end,
            pythonRound(start * 1000.0),
            pythonRound(end * 1000.0),
        });
    }
    return windows;
}

} // namespace subcue
