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
        {QStringLiteral("whisperModelsDirectory"), QString()},
        {QStringLiteral("whisperDevice"), QStringLiteral("auto")},
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
