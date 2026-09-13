#include "asr/local_python_asr_service.h"

#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "common/logging.h"
#include "media/media_probe.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>

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

AppError workerError(const QByteArray &stderr)
{
    const QString details = QString::fromUtf8(stderr).trimmed();
    if (details.contains(QStringLiteral("out of memory"), Qt::CaseInsensitive)) {
        return AppError(ErrorDomain::Asr, 10,
            QStringLiteral("CUDA 显存不足，请关闭其他占用显卡的程序后重试。"), details);
    }
    return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EngineUnavailable),
        QStringLiteral("本地 ASR Python Worker 执行失败。"), details);
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
    QProcess process;
    process.start(pythonExecutable_, {workerPath(), QStringLiteral("--check"), providerId_, modelDirectory_});
    if (!process.waitForStarted(10'000) || !process.waitForFinished(60'000) || process.exitCode() != 0) {
        return workerError(process.readAllStandardError());
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
    const ProbeResult probed = MediaProbe::probe(request.mediaPath);
    if (std::holds_alternative<AppError>(probed)) return std::get<AppError>(probed);
    const QVector<AudioChunkWindow> windows = AudioChunkPlanner::plan(
        std::get<MediaInfo>(probed).duration.seconds());
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
            {QStringLiteral("startMs"), windows.at(index).startMs}});
        if (request.progress) request.progress(index, windows.size());
    }
    const QString requestPath = QDir(temporary.path()).filePath(QStringLiteral("request.json"));
    QFile input(requestPath);
    if (!input.open(QIODevice::WriteOnly)) return AppError(ErrorDomain::Asr,
        static_cast<int>(AsrErrorCode::InvalidRequest), QStringLiteral("无法准备本地 ASR 请求。"));
    input.write(QJsonDocument(QJsonObject{{QStringLiteral("provider"), providerId_},
        {QStringLiteral("modelDirectory"), modelDirectory_},
        {QStringLiteral("forcedAlignerDirectory"), forcedAlignerDirectory_},
        {QStringLiteral("chunks"), chunks}}).toJson(QJsonDocument::Compact));
    input.close();
    qCInfo(subcueAsrLog) << "[ASR] Provider:" << providerId_ << "[ASR] Loading model...";
    QProcess process;
    process.start(pythonExecutable_, {workerPath(), requestPath});
    if (!process.waitForStarted(10'000)) return workerError(process.readAllStandardError());
    while (!process.waitForFinished(200)) {
        if (asrCancelled(request.cancel)) { process.kill(); process.waitForFinished(); return asrCancelledError(); }
    }
    if (process.exitCode() != 0) return workerError(process.readAllStandardError());
    QJsonParseError error;
    const QJsonDocument response = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
    if (error.error != QJsonParseError::NoError || !response.isObject()) return workerError(process.readAllStandardError());
    Transcript transcript;
    for (const QJsonValue &value : response.object().value(QStringLiteral("segments")).toArray()) {
        const QJsonObject segment = value.toObject();
        const QString text = segment.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) transcript.words.append({-1, text,
            qRound64(segment.value(QStringLiteral("start")).toDouble() * 1000),
            qRound64(segment.value(QStringLiteral("end")).toDouble() * 1000)});
    }
    transcript.sortAndReindex();
    if (request.progress) request.progress(windows.size(), windows.size());
    qCInfo(subcueAsrLog) << "[ASR] Model unload" << providerId_;
    if (transcript.words.isEmpty()) return AppError(ErrorDomain::Asr,
        static_cast<int>(AsrErrorCode::EmptyTranscript), QStringLiteral("本地 ASR 未返回带时间戳的结果。"));
    return transcript;
}

} // namespace subcue
