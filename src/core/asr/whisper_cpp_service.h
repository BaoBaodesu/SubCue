#pragma once

#include "asr/asr_service.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/http_client.h"
#include "asr/model_downloader.h"
#include "asr/whisper_engine.h"

#include <QtCore/QString>

#include <memory>

namespace subcue {

class WhisperCppService final : public IAsrService {
public:
    WhisperCppService(
        QString modelId,
        QString modelsDirectory,
        IHttpClient *http = nullptr,
        IWhisperEngine *engine = nullptr);
    WhisperCppService(
        QString modelId,
        QString modelsDirectory,
        QString device,
        IHttpClient *http,
        IWhisperEngine *engine = nullptr);

    [[nodiscard]] QString providerId() const override;
    [[nodiscard]] ProviderTestResult testConnection(
        const std::atomic<bool> *cancel = nullptr) override;
    [[nodiscard]] AsrResult transcribe(const AsrRequest &request) override;

    [[nodiscard]] AsrResult transcribePcm(
        const QVector<float> &pcm16kMono,
        const std::atomic<bool> *cancel = nullptr);

    [[nodiscard]] AsrResult transcribePreparedChunks(
        const QVector<PreparedAudioChunk> &chunks,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(int, int)> &progress = {});

private:
    QString modelId_;
    QString modelsDirectory_;
    std::unique_ptr<IHttpClient> ownedHttp_;
    std::unique_ptr<ModelDownloader> downloader_;
    std::unique_ptr<IWhisperEngine> ownedEngine_;
    IWhisperEngine *engine_ = nullptr;
};

} // namespace subcue
