#include "alignment/alignment_pipeline.h"

#include "alignment/alignment_engine.h"
#include "alignment/forced_align.h"
#include "asr/asr_provider_factory.h"
#include "common/logging.h"
#include "media/media_probe.h"
#include "subtitle/ass_writer.h"
#include "subtitle/srt_writer.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QList>

#include <utility>

namespace subcue {
namespace {

void reportProgress(
    const AlignmentProgress &progress,
    const QString &state,
    const QString &text,
    int completed = 0, int total = 0)
{
    if (progress) {
        progress(AlignmentProgressState{state, text, completed, total});
    }
}

[[nodiscard]] AppError cancelledError()
{
    return asrCancelledError();
}

[[nodiscard]] bool cancelled(const std::atomic<bool> *cancel) noexcept
{
    return asrCancelled(cancel);
}

[[nodiscard]] QList<Subtitle> toList(const QVector<Subtitle> &subtitles)
{
    QList<Subtitle> list;
    list.reserve(subtitles.size());
    for (const Subtitle &subtitle : subtitles) {
        list.append(subtitle);
    }
    return list;
}

} // namespace

AlignmentPipeline::AlignmentPipeline(
    QJsonObject settings,
    AlignmentCredentials credentials,
    IAsrService *asrOverride,
    IAiProvider *aiOverride)
    : settings_(std::move(settings)),
      credentials_(std::move(credentials)),
      asrOverride_(asrOverride)
{
    Q_UNUSED(aiOverride);
}

std::pair<int, int> AlignmentPipeline::videoSize(const MediaInfo &mediaInfo)
{
    if (mediaInfo.videoStreamIndex >= 0 && mediaInfo.videoStreamIndex < mediaInfo.streams.size()) {
        const MediaStreamInfo &stream = mediaInfo.streams.at(mediaInfo.videoStreamIndex);
        if (stream.width > 0 && stream.height > 0) {
            return {stream.width, stream.height};
        }
    }
    for (const MediaStreamInfo &stream : mediaInfo.streams) {
        if (stream.type == MediaStreamType::Video && stream.width > 0 && stream.height > 0) {
            return {stream.width, stream.height};
        }
    }
    return {1920, 1080};
}

std::variant<QStringList, AppError> AlignmentPipeline::exportResult(
    const QString &mediaPath,
    const AlignmentResult &result,
    const MediaInfo &mediaInfo,
    const QJsonObject &settings)
{
    const bool outputSrt = settings.value(QStringLiteral("outputSrt")).toBool();
    const bool outputAss = settings.value(QStringLiteral("outputAss")).toBool();
    if (!outputSrt && !outputAss) {
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("请至少选择一种输出格式。"));
    }

    QString root = settings.value(QStringLiteral("outputDirectory")).toString();
    if (root.isEmpty()) {
        root = QFileInfo(mediaPath).absolutePath();
    }
    if (root.isEmpty()) {
        root = QDir::currentPath();
    }
    if (!QDir().mkpath(root)) {
        return AppError(ErrorDomain::Validation, 2, QStringLiteral("输出目录不可用。"));
    }

    const QString stem = mediaPath.isEmpty()
        ? QStringLiteral("subtitles")
        : QFileInfo(mediaPath).completeBaseName();
    int suffix = 0;
    QString subfolder;
    while (true) {
        const QString folderName = suffix == 0
            ? stem + QStringLiteral("_字幕文件")
            : stem + QStringLiteral("_字幕文件_") + QString::number(suffix);
        subfolder = QDir(root).filePath(folderName);
        if (!QFileInfo::exists(subfolder)) {
            break;
        }
        ++suffix;
    }
    if (!QDir().mkpath(subfolder)) {
        return AppError(ErrorDomain::Validation, 2, QStringLiteral("输出目录不可用。"));
    }

    const QList<Subtitle> exportable = toList(result.exportableSubtitles());
    QStringList paths;
    QString error;
    if (outputSrt) {
        const QString path = QDir(subfolder).filePath(stem + QStringLiteral(".srt"));
        if (!SrtWriter::save(path, exportable, &error)) {
            return AppError(ErrorDomain::Validation, 3, QStringLiteral("导出失败：%1").arg(error));
        }
        paths.append(path);
    }
    if (outputAss) {
        const auto size = videoSize(mediaInfo);
        const QString path = QDir(subfolder).filePath(stem + QStringLiteral(".ass"));
        if (!AssWriter::save(path, exportable, size.first, size.second, settings, &error)) {
            return AppError(ErrorDomain::Validation, 3, QStringLiteral("导出失败：%1").arg(error));
        }
        paths.append(path);
    }
    return paths;
}

AlignmentRunResult AlignmentPipeline::run(
    const QString &mediaPath,
    const QStringList &lines,
    const std::atomic<bool> *cancel,
    const AlignmentProgress &progress)
{
    if (cancelled(cancel)) {
        return cancelledError();
    }

    const QString providerId = asrOverride_
        ? asrOverride_->providerId()
        : AsrProviderFactory::providerIdFromSettings(settings_);
    if (asrOverride_ == nullptr
        && providerId == QLatin1String(kAsrProviderDashScope)
        && credentials_.asrApiKey.trimmed().isEmpty()) {
        return AppError(ErrorDomain::Settings, 1,
            QStringLiteral("云端 ASR API Key 尚未配置，请在设置中保存凭据。"));
    }

    qCInfo(subcueAppLog) << "task_start media_type=" << QFileInfo(mediaPath).suffix().toLower();
    reportProgress(progress, QStringLiteral("PreparingAudio"), QStringLiteral("正在准备音频…"));

    ProbeResult probed = MediaProbe::probe(mediaPath);
    if (std::holds_alternative<AppError>(probed)) {
        return std::get<AppError>(probed);
    }
    const MediaInfo mediaInfo = std::get<MediaInfo>(probed);
    qCInfo(subcueAppLog) << "media duration_ms=" << mediaInfo.duration.milliseconds()
                         << "has_video=" << (mediaInfo.videoStreamIndex >= 0);

    if (cancelled(cancel)) {
        return cancelledError();
    }

    std::unique_ptr<IAsrService> ownedAsr;
    IAsrService *asr = asrOverride_;
    if (asr == nullptr) {
        ownedAsr = AsrProviderFactory().create(settings_, credentials_.asrApiKey);
        asr = ownedAsr.get();
    }

    QElapsedTimer timer;
    timer.start();
    const AsrResult asrResult = asr->transcribe(AsrRequest{
        mediaPath,
        cancel,
        [&progress](int current, int total) {
            reportProgress(
                progress,
                QStringLiteral("ASR"),
                total >= 100
                    ? QStringLiteral("正在识别 %1%…").arg(
                        total > 0 ? static_cast<int>((static_cast<qint64>(current) * 100) / total) : 0)
                    : QStringLiteral("正在识别 %1 / %2…").arg(current).arg(total),
                current, total);
        },
    });
    if (std::holds_alternative<AppError>(asrResult)) {
        const AppError &error = std::get<AppError>(asrResult);
        if (error.domain() != ErrorDomain::Asr
            || error.code() != static_cast<int>(AsrErrorCode::EmptyTranscript)) return error;
        // 空识别结果仍保留完整文稿，交给用户手动定位。
    }
    const Transcript transcript = std::holds_alternative<Transcript>(asrResult)
        ? std::get<Transcript>(asrResult) : Transcript{};
    qCInfo(subcueAppLog) << "asr elapsed_ms=" << timer.elapsed()
                         << "words=" << transcript.words.size();

    if (cancelled(cancel)) {
        return cancelledError();
    }

    reportProgress(progress, QStringLiteral("Alignment"), QStringLiteral("正在对齐字幕…"));
    timer.restart();
    AlignmentResult result = AlignmentEngine::alignSubtitleLines(lines, transcript.words);
    for (Subtitle &subtitle : result.subtitles) {
        subtitle.metadata.insert(QStringLiteral("asrProvider"), providerId);
        // 当前 Provider 契约未提供独立 ASR 分数，明确记录未知而非复用对齐分数。
        subtitle.metadata.insert(QStringLiteral("asrConfidence"), QJsonValue::Null);
        subtitle.metadata.insert(QStringLiteral("alignmentConfidence"), subtitle.confidence);
        subtitle.metadata.insert(QStringLiteral("audioEvidence"), subtitle.end > subtitle.start
            && subtitle.startWordId >= 0 && subtitle.endWordId >= subtitle.startWordId);
        subtitle.metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("notReviewed"));
        subtitle.metadata.insert(QStringLiteral("manualConfirmed"), false);
    }
    qCInfo(subcueAppLog) << "alignment elapsed_ms=" << timer.elapsed();

    AlignmentEngine::finalizeAudioFirst(result.subtitles);
    // ASR 是证据而非删稿依据：漏识别的原稿行保留为待复核的候选时间段。
    if (!transcript.words.isEmpty()) {
        const AlignmentResult recovered = ForcedAligner::forcedAlignSubtitleLines(
            lines, transcript.words, mediaInfo.duration.milliseconds());
        for (qsizetype index = 0; index < result.subtitles.size(); ++index) {
            if (result.subtitles.at(index).status == QStringLiteral("SKIPPED_NO_AUDIO")
                && index < recovered.subtitles.size() && recovered.subtitles.at(index).isTimed()) {
                result.subtitles[index] = recovered.subtitles.at(index);
                result.subtitles[index].status = QStringLiteral("REVIEW");
                result.subtitles[index].source = QStringLiteral("forced-align-candidate");
                result.subtitles[index].skipReason = QStringLiteral("ASR 未定位到原稿句，需复核候选时间段");
            }
        }
    }
    for (Subtitle &subtitle : result.subtitles) {
        subtitle.metadata.insert(QStringLiteral("diagnosticReason"), transcript.words.isEmpty()
            ? QStringLiteral("ASR_NO_WORDS") : !subtitle.isTimed()
            ? QStringLiteral("ALIGNMENT_NO_LOCATION") : subtitle.status == QStringLiteral("LOW_CONFIDENCE")
            ? QStringLiteral("ALIGNMENT_LOW_CONFIDENCE") : QStringLiteral("ALIGNED"));
    }
    qCInfo(subcueAppLog) << "alignment_summary words=" << transcript.words.size()
        << "timed=" << result.exportableSubtitles().size() << "pending=" << result.lowCount()
        << "unlocated=" << result.skippedCount() << "audio_stream=" << mediaInfo.audioStreamIndex;

    if (cancelled(cancel)) {
        return cancelledError();
    }

    reportProgress(progress, QStringLiteral("Applying"), QStringLiteral("正在应用字幕…"));
    AlignmentTaskOutput output;
    output.result = std::move(result);
    output.mediaInfo = mediaInfo;
    output.words = transcript.words;
    reportProgress(progress, QStringLiteral("Completed"), QStringLiteral("完成。"), 1, 1);
    return output;
}

} // namespace subcue
