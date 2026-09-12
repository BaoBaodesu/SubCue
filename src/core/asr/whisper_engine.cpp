#include "asr/whisper_engine.h"

#ifdef SUBCUE_HAS_WHISPER
#include <whisper.h>

#include <QtCore/QFileInfo>

#include <algorithm>
#endif

namespace subcue {

AsrResult UnlinkedWhisperEngine::transcribePcm(
    const QVector<float> &pcm16kMono,
    const std::atomic<bool> *cancel)
{
    Q_UNUSED(pcm16kMono);
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
        QStringLiteral("whisper.cpp 运行时未链接"));
}

#ifdef SUBCUE_HAS_WHISPER
struct LinkedWhisperEngine::Private final {
    whisper_context *context = nullptr;
    QString modelPath;
    QString device;

    ~Private()
    {
        if (context) {
            whisper_free(context);
        }
    }
};

LinkedWhisperEngine::LinkedWhisperEngine(QString device)
    : d_(std::make_unique<Private>())
{
    d_->device = std::move(device);
}

LinkedWhisperEngine::~LinkedWhisperEngine() = default;

bool LinkedWhisperEngine::setModelPath(const QString &path, QString *errorMessage)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    if (d_->context && d_->modelPath == absolutePath) {
        return true;
    }
    if (d_->context) {
        whisper_free(d_->context);
        d_->context = nullptr;
        d_->modelPath.clear();
    }
    whisper_context_params contextParams = whisper_context_default_params();
    contextParams.use_gpu = d_->device != QLatin1String("cpu");
    d_->context = whisper_init_from_file_with_params(absolutePath.toUtf8().constData(), contextParams);
    if (!d_->context) {
        if (errorMessage) *errorMessage = QStringLiteral("whisper.cpp 无法读取模型文件：%1").arg(absolutePath);
        return false;
    }
    d_->modelPath = absolutePath;
    return true;
}

AsrResult LinkedWhisperEngine::transcribePcm(
    const QVector<float> &pcm16kMono,
    const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    if (!d_->context) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("Whisper 模型尚未加载"));
    }
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.token_timestamps = true;
    params.no_context = true;
    params.language = "auto";
    params.abort_callback = [](void *userData) {
        const auto *flag = static_cast<const std::atomic<bool> *>(userData);
        return flag && flag->load(std::memory_order_acquire);
    };
    params.abort_callback_user_data = const_cast<std::atomic<bool> *>(cancel);
    if (whisper_full(d_->context, params, pcm16kMono.constData(), pcm16kMono.size()) != 0) {
        if (asrCancelled(cancel)) {
            return asrCancelledError();
        }
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::InvalidRequest),
            QStringLiteral("Whisper 本地识别失败"));
    }

    Transcript transcript;
    const int segmentCount = whisper_full_n_segments(d_->context);
    for (int segment = 0; segment < segmentCount; ++segment) {
        const int tokenCount = whisper_full_n_tokens(d_->context, segment);
        for (int token = 0; token < tokenCount; ++token) {
            const whisper_token_data data = whisper_full_get_token_data(d_->context, segment, token);
            if (data.id >= whisper_token_eot(d_->context) || data.t0 < 0 || data.t1 <= data.t0) {
                continue;
            }
            const char *raw = whisper_full_get_token_text(d_->context, segment, token);
            const QString text = QString::fromUtf8(raw ? raw : "").trimmed();
            if (text.isEmpty()) {
                continue;
            }
            transcript.words.push_back({
                static_cast<qint64>(transcript.words.size() + 1),
                text,
                static_cast<qint64>(data.t0) * 10,
                static_cast<qint64>(data.t1) * 10,
            });
        }
    }
    if (transcript.words.isEmpty()) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EmptyTranscript),
            QStringLiteral("Whisper 未返回词级时间戳"));
    }
    return transcript;
}
#endif

} // namespace subcue
