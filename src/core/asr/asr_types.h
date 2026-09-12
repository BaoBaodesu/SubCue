#pragma once

#include "alignment/transcript.h"
#include "common/app_error.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <atomic>
#include <functional>
#include <variant>

namespace subcue {

inline constexpr double kAsrChunkDurationSeconds = 270.0;
inline constexpr double kAsrChunkOverlapSeconds = 1.0;
inline constexpr int kAsrSampleRate = 16'000;

inline constexpr auto kAsrProviderDashScope = "dashscope";
inline constexpr auto kAsrProviderWhisper = "whisper";

enum class AsrErrorCode {
    Cancelled = 1,
    HttpFailure = 2,
    EmptyTranscript = 3,
    InvalidRequest = 4,
    ModelNotFound = 5,
    ChecksumMismatch = 6,
    EngineUnavailable = 7,
    DownloadFailure = 8
};

struct AudioChunkWindow final {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    qint64 startMs = 0;
    qint64 endMs = 0;
};

struct PreparedAudioChunk final {
    AudioChunkWindow window;
    QByteArray flac;
    QVector<float> pcm16kMono;
};

struct AsrRequest final {
    QString mediaPath;
    const std::atomic<bool> *cancel = nullptr;
    std::function<void(int current, int total)> progress;
};

using AsrResult = std::variant<Transcript, AppError>;

[[nodiscard]] inline bool asrCancelled(const std::atomic<bool> *cancel) noexcept
{
    return cancel && cancel->load(std::memory_order_acquire);
}

[[nodiscard]] inline AppError asrCancelledError()
{
    return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::Cancelled),
        QStringLiteral("任务已取消"));
}

} // namespace subcue
