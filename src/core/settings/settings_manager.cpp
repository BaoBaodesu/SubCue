#include "settings/settings_manager.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

#include <utility>

namespace subcue {

SettingsManager::SettingsManager(QString path)
    : path_(path.isEmpty() ? defaultPath() : std::move(path))
{
}

QJsonObject SettingsManager::defaults()
{
    // 本地 Whisper 默认使用显卡：CUDA 构建默认走 CUDA，纯 CPU 构建默认回落 CPU。
#ifdef SUBCUE_HAS_CUDA
    const QString defaultWhisperDevice = QStringLiteral("cuda");
#else
    const QString defaultWhisperDevice = QStringLiteral("cpu");
#endif
    return {
        {QStringLiteral("version"), 2},
        {QStringLiteral("appearanceMode"), QStringLiteral("dark")},
        {QStringLiteral("fontFamily"), QStringLiteral("Microsoft YaHei")},
        {QStringLiteral("fontSize1080p"), 52},
        {QStringLiteral("alignment"), QStringLiteral("bottom-center")},
        {QStringLiteral("bottomMargin1080p"), 90},
        {QStringLiteral("fontColor"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("outlineColor"), QStringLiteral("#000000")},
        {QStringLiteral("outlineSize"), 2},
        {QStringLiteral("shadow"), 0},
        {QStringLiteral("outputSrt"), true},
        {QStringLiteral("outputAss"), true},
        {QStringLiteral("outputDirectory"), QString()},
        {QStringLiteral("aiAssistEnabled"), false},
        {QStringLiteral("aiProviderId"), QString()},
        {QStringLiteral("aiProviders"), QJsonArray{}},
        {QStringLiteral("asrVerification"), QJsonObject{}},
        {QStringLiteral("asrConfigRevision"), 0},
        {QStringLiteral("asrCredentialRevision"), 0},
        {QStringLiteral("asrModel"), QStringLiteral("fun-asr-flash-2026-06-15")},
        {QStringLiteral("asrProvider"), QStringLiteral("dashscope")},
        {QStringLiteral("whisperModel"), QStringLiteral("small")},
        {QStringLiteral("whisperModelsDirectory"),
         QString::fromUtf8(SUBCUE_PROJECT_WHISPER_MODELS_DIR)},
        {QStringLiteral("whisperDevice"), defaultWhisperDevice},
        {QStringLiteral("qwen3AsrModelsDirectory"),
         QDir(QString::fromUtf8(SUBCUE_PROJECT_MODELS_DIR)).filePath(QStringLiteral("qwen3-asr-0.6b"))},
        {QStringLiteral("qwen3ForcedAlignerModelsDirectory"),
         QDir(QString::fromUtf8(SUBCUE_PROJECT_MODELS_DIR)).filePath(QStringLiteral("qwen3-forced-aligner-0.6b"))},
        {QStringLiteral("funAsrModelsDirectory"),
         QDir(QString::fromUtf8(SUBCUE_PROJECT_MODELS_DIR)).filePath(QStringLiteral("fun-asr-nano-2512"))},
        {QStringLiteral("localAsrPython"), QStringLiteral("python")},
        {QStringLiteral("region"), QStringLiteral("beijing")},
        {QStringLiteral("asrApiHost"), QString()},
        {QStringLiteral("ffmpegPath"), QString()},
    };
}

QString SettingsManager::defaultPath()
{
    const QString appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    const QString base = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        : QDir(appData).filePath(QStringLiteral("SubCue"));
    const QString destination = QDir(base).filePath(QStringLiteral("settings.json"));
    if (!appData.isEmpty() && !QFileInfo::exists(destination)) {
        const QString source = QDir(appData).filePath(
            QStringLiteral("AutoSubtitleAligner/settings.json"));
        if (QFileInfo::exists(source) && QDir().mkpath(QFileInfo(destination).absolutePath())) {
            QFile::copy(source, destination);
        }
    }
    return destination;
}

QJsonObject SettingsManager::load() const
{
    QJsonObject result = defaults();
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return result;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return result;
    const QJsonObject loaded = document.object();
    for (auto iterator = result.begin(); iterator != result.end(); ++iterator) {
        if (loaded.contains(iterator.key())) iterator.value() = loaded.value(iterator.key());
    }
    if (loaded.value(QStringLiteral("version")).toInt(1) < 2
        && loaded.value(QStringLiteral("aiProviders")).toArray().isEmpty()) {
        // 旧版 Qwen 配置不再隐式创建云 Provider，等待用户显式配置兼容端点。
        result.insert(QStringLiteral("aiAssistEnabled"), false);
    }
    // 仅迁移旧默认目录；用户明确配置的其他自定义目录保持不变。
    const QString appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    const QString legacyWhisperDirectory = QDir(appData).filePath(QStringLiteral("SubCue/models"));
    if (!appData.isEmpty()
        && QDir::cleanPath(result.value(QStringLiteral("whisperModelsDirectory")).toString())
            == QDir::cleanPath(legacyWhisperDirectory)) {
        result.insert(QStringLiteral("whisperModelsDirectory"),
            QString::fromUtf8(SUBCUE_PROJECT_WHISPER_MODELS_DIR));
    }
    result.insert(QStringLiteral("version"), 2);
    return result;
}

bool SettingsManager::save(const QJsonObject &settings, QString *errorMessage) const
{
    QJsonObject safe = defaults();
    for (auto iterator = safe.begin(); iterator != safe.end(); ++iterator) {
        if (settings.contains(iterator.key())) iterator.value() = settings.value(iterator.key());
    }
    safe.insert(QStringLiteral("version"), 2);
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        if (errorMessage) *errorMessage = QStringLiteral("无法创建设置目录");
        return false;
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(safe).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

const QString &SettingsManager::path() const noexcept { return path_; }

} // namespace subcue
