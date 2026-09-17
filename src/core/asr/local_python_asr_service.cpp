#include "asr/local_python_asr_service.h"

#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "cache/analysis_cache.h"
#include "common/logging.h"
#include "inference/inference_manager.h"
#include "media/media_probe.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

#include <utility>

namespace subcue {
namespace {

QString workerPath()
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("asr_python_worker.py"));
    return QFileInfo::exists(bundled)
        ? bundled : QDir::current().filePath(QStringLiteral("tools/asr_python_worker.py"));
}

QString workerProgram(const QString &pythonExecutable, QStringList *arguments)
{
    QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("inference/SubCueInference.exe"));
    if (!QFileInfo::exists(executable)) {
        executable = QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("SubCueInference.exe"));
    }
    if (QFileInfo::exists(executable)) {
        arguments->append(QStringLiteral("--stdio"));
        return executable;
    }
    arguments->append({workerPath(), QStringLiteral("--stdio")});
    return pythonExecutable;
}

AppError workerError(const QByteArray &errorOutput)
{
    const QString details = QString::fromUtf8(errorOutput).trimmed();
    if (details.contains(QStringLiteral("out of memory"), Qt::CaseInsensitive)) {
        return AppError(ErrorDomain::Asr, 10,
            QStringLiteral("CUDA 显存不足，请关闭其他占用显卡的程序后重试。"), details);
    }
    return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
        QStringLiteral("本地 ASR Python Worker 执行失败。"), details);
}

Transcript transcriptFromResponse(const QJsonObject &response)
{
    Transcript transcript;
    for (const QJsonValue &value : response.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject segment = value.toObject();
        const QString text = segment.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) transcript.words.append({-1, text,
            qRound64(segment.value(QStringLiteral("start")).toDouble() * 1000),
            qRound64(segment.value(QStringLiteral("end")).toDouble() * 1000)});
    }
    transcript.sortAndReindex();
    return transcript;
}

} // namespace

LocalPythonAsrService::LocalPythonAsrService(QString providerId, QString modelDirectory,
                                             QString forcedAlignerDirectory, QString pythonExecutable)
    : providerId_(std::move(providerId)), modelDirectory_(std::move(modelDirectory)),
      forcedAlignerDirectory_(std::move(forcedAlignerDirectory)),
      pythonExecutable_(pythonExecutable.isEmpty() ? QStringLiteral("python") : std::move(pythonExecutable)) {}

QString LocalPythonAsrService::providerId() const { return providerId_; }

bool LocalPythonAsrService::modelReady(const QString &directory)
{
    const QDir dir(directory);
    return QFileInfo::exists(dir.filePath(QStringLiteral("config.json")))
        && (QFileInfo::exists(dir.filePath(QStringLiteral("model.safetensors")))
            || QFileInfo::exists(dir.filePath(QStringLiteral("model.pt"))));
}

ProviderTestResult LocalPythonAsrService::testConnection(const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) return asrCancelledError();
    if (!modelReady(modelDirectory_)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("本地模型未安装或权重不完整：%1").arg(modelDirectory_));
    }
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("taskId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("action"), QStringLiteral("check")},
        {QStringLiteral("provider"), providerId_},
        {QStringLiteral("modelDirectory"), modelDirectory_}}).toJson(QJsonDocument::Compact) + '\n';
    QStringList arguments;
    const QString program = workerProgram(pythonExecutable_, &arguments);
    const InferenceProcessResult result = InferenceManager::instance().run(
        program, arguments, request, cancel, 60'000);
    if (result.cancelled) return asrCancelledError();
    if (!result.started || result.timedOut || result.exitCode != 0) {
        return workerError(result.standardError + result.standardOutput);
    }
    return ConnectionTestResult{{ModelDescriptor{providerId_, providerId_, 0, true}}, QDateTime::currentDateTimeUtc()};
}

AsrResult LocalPythonAsrService::transcribe(const AsrRequest &request)
{
    if (asrCancelled(request.cancel)) return asrCancelledError();
    if (!modelReady(modelDirectory_)) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::ModelNotFound),
            QStringLiteral("本地模型未安装或权重不完整：%1").arg(modelDirectory_));
    }
    AnalysisCache cache;
    const QFileInfo modelWeight(QDir(modelDirectory_).filePath(QStringLiteral("model.safetensors")));
    const QJsonObject cacheParameters{
        {QStringLiteral("chunkSeconds"), providerId_ == QLatin1String("qwen3")
            ? 20.0 : kAsrChunkDurationSeconds},
        {QStringLiteral("overlapSeconds"), kAsrChunkOverlapSeconds},
        {QStringLiteral("forcedAligner"), forcedAlignerDirectory_},
        {QStringLiteral("modelBytes"), modelWeight.size()},
        {QStringLiteral("modelModifiedMs"), modelWeight.lastModified().toMSecsSinceEpoch()}};
    const std::variant<QString, AppError> cacheKeyResult = cache.keyFor(
        request.mediaPath, providerId_ + QLatin1Char(':') + modelDirectory_,
        QStringLiteral("Chinese"), cacheParameters, QStringLiteral("local-asr-v2"), request.cancel);
    if (std::holds_alternative<AppError>(cacheKeyResult)) return std::get<AppError>(cacheKeyResult);
    const QString cacheKey = std::get<QString>(cacheKeyResult);
    if (const std::optional<QJsonObject> cached = cache.load(cacheKey)) {
        Transcript transcript = transcriptFromResponse(*cached);
        if (!transcript.words.isEmpty()) return transcript;
    }
    const ProbeResult probed = MediaProbe::probe(request.mediaPath);
    if (std::holds_alternative<AppError>(probed)) return std::get<AppError>(probed);
    const QVector<AudioChunkWindow> windows = AudioChunkPlanner::plan(
        std::get<MediaInfo>(probed).duration.seconds(),
        providerId_ == QLatin1String("qwen3") ? 20.0 : kAsrChunkDurationSeconds);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return AppError(ErrorDomain::Asr,
        static_cast<int>(AsrErrorCode::InvalidRequest), QStringLiteral("无法创建本地 ASR 临时目录。"));
    QJsonArray chunks;
    for (int index = 0; index < windows.size(); ++index) {
        if (asrCancelled(request.cancel)) return asrCancelledError();
        const MediaResult<PreparedAudioChunk> extracted = AudioChunkExtractor::extract(
            request.mediaPath, windows.at(index), request.cancel, true);
        if (std::holds_alternative<AppError>(extracted)) return std::get<AppError>(extracted);
        const QString path = QDir(temporary.path()).filePath(QStringLiteral("chunk_%1.flac").arg(index));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(std::get<PreparedAudioChunk>(extracted).flac) < 0) {
            return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::InvalidRequest),
                QStringLiteral("无法准备本地 ASR 音频分块。"));
        }
        chunks.append(QJsonObject{{QStringLiteral("path"), path},
            {QStringLiteral("startMs"), windows.at(index).startMs},
            {QStringLiteral("endMs"), windows.at(index).endMs}});
        if (request.progress) request.progress(index, windows.size());
    }
    const QString resultPath = QDir(temporary.path()).filePath(QStringLiteral("result.json"));
    const QByteArray workerRequest = QJsonDocument(QJsonObject{
        {QStringLiteral("taskId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("action"), QStringLiteral("transcribe")},
        {QStringLiteral("provider"), providerId_},
        {QStringLiteral("modelDirectory"), modelDirectory_},
        {QStringLiteral("forcedAlignerDirectory"), forcedAlignerDirectory_},
        {QStringLiteral("resultPath"), resultPath},
        {QStringLiteral("chunks"), chunks}}).toJson(QJsonDocument::Compact) + '\n';
    qCInfo(subcueAsrLog) << "[ASR] Provider:" << providerId_ << "[ASR] Loading model...";
    QStringList arguments;
    const QString program = workerProgram(pythonExecutable_, &arguments);
    const InferenceProcessResult result = InferenceManager::instance().run(
        program, arguments, workerRequest, request.cancel, -1,
        [&request](const QByteArray &line) {
            if (!request.progress) return;
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            if (event.value(QStringLiteral("type")).toString() == QLatin1String("progress")) {
                request.progress(event.value(QStringLiteral("completed")).toInt(),
                    event.value(QStringLiteral("total")).toInt());
            }
        });
    if (result.cancelled) return asrCancelledError();
    if (!result.started || result.exitCode != 0) {
        return workerError(result.standardError + result.standardOutput);
    }
    QFile output(resultPath);
    if (!output.open(QIODevice::ReadOnly)) return workerError(result.standardOutput);
    QJsonParseError error;
    const QJsonDocument response = QJsonDocument::fromJson(output.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !response.isObject()) {
        return workerError(result.standardError + result.standardOutput);
    }
    Transcript transcript = transcriptFromResponse(response.object());
    if (request.progress) request.progress(windows.size(), windows.size());
    qCInfo(subcueAsrLog) << "[ASR] Model unload" << providerId_;
    if (transcript.words.isEmpty()) return AppError(ErrorDomain::Asr,
        static_cast<int>(AsrErrorCode::EmptyTranscript), QStringLiteral("本地 ASR 未返回带时间戳的结果。"));
    (void)cache.store(cacheKey, response.object());
    return transcript;
}

} // namespace subcue
