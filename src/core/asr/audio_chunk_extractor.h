#pragma once

#include "asr/asr_types.h"
#include "media/media_types.h"

#include <QtCore/QString>

namespace subcue {

class AudioChunkExtractor final {
public:
    [[nodiscard]] static MediaResult<PreparedAudioChunk> extract(
        const QString &mediaPath,
        const AudioChunkWindow &window,
        const std::atomic<bool> *cancel = nullptr,
        bool encodeFlac = true);

    [[nodiscard]] static MediaResult<QVector<float>> decodePcm16kMono(
        const QString &mediaPath,
        const AudioChunkWindow &window,
        const std::atomic<bool> *cancel = nullptr);

    [[nodiscard]] static MediaResult<QByteArray> encodeFlac(const QVector<float> &pcm16kMono);
};

} // namespace subcue
