#pragma once

#include "asr/asr_types.h"

namespace subcue {

class AudioChunkPlanner final {
public:
    // 复刻 services/ffmpeg_service.py::create_audio_chunks 的窗口公式：
    // 270 秒分片、后续分片向前重叠 1 秒；毫秒边界使用 CPython round()。
    [[nodiscard]] static QVector<AudioChunkWindow> plan(double durationSeconds);
};

} // namespace subcue
