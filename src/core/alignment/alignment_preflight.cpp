#include "alignment/alignment_preflight.h"

#include "asr/asr_types.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace subcue {
namespace {

void addIssue(QVector<PreflightIssue> &issues, const char *code, const QString &title,
              const QString &detail = {}, const QString &section = {})
{
    issues.push_back({QString::fromLatin1(code), title, detail, section});
}

} // namespace

QVector<PreflightIssue> AlignmentPreflight::check(
    const QString &mediaPath,
    const MediaInfo &mediaInfo,
    const QStringList &lines,
    const QJsonObject &settings,
    const PreflightState &state)
{
    QVector<PreflightIssue> issues;
    if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath)) {
        addIssue(issues, "media_missing", QStringLiteral("请选择有效媒体文件"));
    } else if (mediaInfo.audioStreamIndex < 0) {
        addIssue(issues, "audio_missing", QStringLiteral("媒体文件不包含可用音轨"));
    }
    if (lines.isEmpty()) {
        addIssue(issues, "script_missing", QStringLiteral("请粘贴或导入字幕文稿"));
    }
    if (!settings.value(QStringLiteral("outputSrt")).toBool()
        && !settings.value(QStringLiteral("outputAss")).toBool()) {
        addIssue(issues, "output_format_missing", QStringLiteral("请至少启用一种输出格式"),
                 {}, QStringLiteral("output"));
    }

    QString outputDirectory = settings.value(QStringLiteral("outputDirectory")).toString();
    if (outputDirectory.isEmpty() && !mediaPath.isEmpty()) {
        outputDirectory = QFileInfo(mediaPath).absolutePath();
    }
    if (!outputDirectory.isEmpty()) {
        const QFileInfo outputInfo(outputDirectory);
        if (!outputInfo.exists() || !outputInfo.isDir() || !outputInfo.isWritable()) {
            addIssue(issues, "output_directory_invalid", QStringLiteral("输出目录无效或不可写"),
                     outputDirectory, QStringLiteral("output"));
        }
    }

    const QString asrProvider = settings.value(QStringLiteral("asrProvider")).toString();
    if (asrProvider != QLatin1String(kAsrProviderDashScope)
        && asrProvider != QLatin1String("qwen3")
        && asrProvider != QLatin1String("funasr")) {
        addIssue(issues, "asr_provider_missing", QStringLiteral("请选择语音识别 Provider"),
                 {}, QStringLiteral("asr"));
    } else if (asrProvider == QLatin1String(kAsrProviderDashScope)) {
        if (settings.value(QStringLiteral("asrModel")).toString().isEmpty()) {
            addIssue(issues, "asr_model_missing", QStringLiteral("请选择云端 ASR 模型"),
                     {}, QStringLiteral("asr"));
        }
        if (!state.asrCredentialExists) {
            addIssue(issues, "asr_key_missing", QStringLiteral("云端 ASR API Key 尚未配置"),
                     {}, QStringLiteral("asr"));
        }
        if (!state.asrVerified) {
            addIssue(issues, "asr_not_verified", QStringLiteral("云端 ASR 尚未通过连接测试"),
                     {}, QStringLiteral("asr"));
        }
    }
    return issues;
}

} // namespace subcue
