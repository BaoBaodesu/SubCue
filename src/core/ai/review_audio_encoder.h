#pragma once

#include "ai/ai_review_types.h"
#include "media/media_types.h"

#include <atomic>

namespace subcue {

class ReviewAudioEncoder final {
public:
    [[nodiscard]] static MediaResult<OmniAudioClip> encodeWindow(
        const QString &mediaPath,
        qint64 startMs,
        qint64 endMs,
        const std::atomic<bool> *cancel = nullptr);

    [[nodiscard]] static QByteArray encodeWav16kMono(const QVector<float> &pcm16kMono);
    [[nodiscard]] static MediaResult<QByteArray> encodeAac16kMono(const QVector<float> &pcm16kMono);
};

} // namespace subcue
