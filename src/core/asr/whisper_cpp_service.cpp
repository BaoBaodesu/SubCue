#include "asr/whisper_cpp_service.h"

#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "asr/whisper_model_catalog.h"
#include "common/logging.h"

#include <QtCore/QDir>
#include "media/media_probe.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace subcue {
namespace {

[[nodiscard]] bool isOverlapDuplicate(
    const TranscriptWord &candidate,
    const QVector<TranscriptWord> &merged)
{
    for (auto iterator = merged.crbegin(); iterator != merged.crend(); ++iterator) {
        if (iterator->endMs < candidate.startMs - 1000) {
            break;
        }
        if (iterator->text == candidate.text
            && std::abs(iterator->startMs - candidate.startMs) <= 500) {
            return true;
        }
    }
    return false;
}

} // namespace

WhisperCppService::WhisperCppService(
    QString modelId,
    QString modelsDirectory,
    IHttpClient *http,
    IWhisperEngine *engine)
    : WhisperCppService(std::move(modelId), std::move(modelsDirectory),
                        QStringLiteral("auto"), http, engine)
{
}

WhisperCppService::WhisperCppService(
    QString modelId,
    QString modelsDirectory,
    QString device,
    IHttpClient *http,
    IWhisperEngine *engine)
    : modelId_(std::move(modelId)),
      modelsDirectory_(std::move(modelsDirectory)),
      engine_(engine)
{
    IHttpClient *client = http;
    if (!client) {
        ownedHttp_ = std::make_unique<QtNetworkHttpClient>();
        client = ownedHttp_.get();
    }
    downloader_ = std::make_unique<ModelDownloader>(client);
    if (!engine_) {
#ifdef SUBCUE_HAS_WHISPER
        ownedEngine_ = std::make_unique<LinkedWhisperEngine>(std::move(device));
#else
        ownedEngine_ = std::make_unique<UnlinkedWhisperEngine>();
#endif
        engine_ = ownedEngine_.get();
    }
}

QString WhisperCppService::providerId() const
{
    return QString::fromLatin1(kAsrProviderWhisper);
}

ProviderTestResult WhisperCppService::testConnection(const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    const std::optional<WhisperModelSpec> spec = WhisperModelCatalog::find(modelId_);
    if (!spec) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("未知的 Whisper 模型：%1").arg(modelId_));
    }
    if (!engine_->isAvailable()) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
            QStringLiteral("whisper.cpp 运行时未链接"));
    }
    const QString path = QDir(modelsDirectory_).filePath(spec->fileName);
    if (!ModelDownloader::matchesSpec(path, *spec)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("Whisper 模型尚未下载或校验失败"));
    }
    QString loadError;
    if (!engine_->setModelPath(path, &loadError)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
            QStringLiteral("无法加载 Whisper 模型"), loadError);
    }
    return ConnectionTestResult{{ModelDescriptor{spec->id, spec->id, spec->size, true}},
                                QDateTime::currentDateTimeUtc()};
}

AsrResult WhisperCppService::transcribePcm(
    const QVector<float> &pcm16kMono,
    const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    AsrResult result = engine_->transcribePcm(pcm16kMono, cancel);
    if (std::holds_alternative<AppError>(result)) {
        return result;
    }
    Transcript transcript = std::get<Transcript>(std::move(result));
    transcript.sortAndReindex();
    return transcript;
}

AsrResult WhisperCppService::transcribePreparedChunks(
    const QVector<PreparedAudioChunk> &chunks,
    const std::atomic<bool> *cancel,
    const std::function<void(int, int)> &progress)
{
    QVector<TranscriptWord> merged;
    if (progress) progress(0, chunks.size());
    for (int index = 0; index < chunks.size(); ++index) {
        if (asrCancelled(cancel)) {
            return asrCancelledError();
        }
        AsrResult local = transcribePcm(chunks.at(index).pcm16kMono, cancel);
        if (std::holds_alternative<AppError>(local)) {
            return local;
        }
        for (TranscriptWord word : std::get<Transcript>(std::move(local)).words) {
            word.startMs += chunks.at(index).window.startMs;
            word.endMs += chunks.at(index).window.startMs;
            if (!isOverlapDuplicate(word, merged)) {
                merged.push_back(std::move(word));
            }
        }
        if (progress) {
            progress(index + 1, chunks.size());
        }
    }
    Transcript transcript;
    transcript.words = std::move(merged);
    transcript.sortAndReindex();
    return transcript;
}

AsrResult WhisperCppService::transcribe(const AsrRequest &request)
{
    if (asrCancelled(request.cancel)) {
        return asrCancelledError();
    }
    const std::optional<WhisperModelSpec> spec = WhisperModelCatalog::find(modelId_);
    if (!spec) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("未知的 Whisper 模型：%1").arg(modelId_));
    }
    std::variant<QString, AppError> modelPath =
        downloader_->ensure(*spec, modelsDirectory_, request.cancel);
    if (std::holds_alternative<AppError>(modelPath)) {
        return std::get<AppError>(modelPath);
    }
    QString loadError;
    if (!engine_->setModelPath(std::get<QString>(modelPath), &loadError)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
            QStringLiteral("无法加载 Whisper 模型"), loadError);
    }
    qCInfo(subcueAsrLog) << "whisper model ready" << spec->id;

    const ProbeResult probed = MediaProbe::probe(request.mediaPath);
    if (std::holds_alternative<AppError>(probed)) {
        return std::get<AppError>(probed);
    }
    const MediaInfo &info = std::get<MediaInfo>(probed);
    const QVector<AudioChunkWindow> windows = AudioChunkPlanner::plan(info.duration.seconds());
    QVector<TranscriptWord> merged;
    for (int index = 0; index < windows.size(); ++index) {
        if (asrCancelled(request.cancel)) {
            return asrCancelledError();
        }
        MediaResult<PreparedAudioChunk> extracted = AudioChunkExtractor::extract(
            request.mediaPath, windows.at(index), request.cancel, false);
        if (std::holds_alternative<AppError>(extracted)) {
            return std::get<AppError>(extracted);
        }
        if (request.progress) request.progress(index, windows.size());
        AsrResult local = transcribePcm(
            std::get<PreparedAudioChunk>(extracted).pcm16kMono, request.cancel);
        if (std::holds_alternative<AppError>(local)) {
            return local;
        }
        for (TranscriptWord word : std::get<Transcript>(std::move(local)).words) {
            word.startMs += windows.at(index).startMs;
            word.endMs += windows.at(index).startMs;
            if (!isOverlapDuplicate(word, merged)) {
                merged.push_back(std::move(word));
            }
        }
        if (request.progress) {
            request.progress(index + 1, windows.size());
        }
    }
    qCInfo(subcueAsrLog) << "whisper chunks" << windows.size();
    Transcript transcript;
    transcript.words = std::move(merged);
    transcript.sortAndReindex();
    return transcript;
}

} // namespace subcue
