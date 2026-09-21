#include "settings/settings_manager.h"
#include "settings/model_locator.h"

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
        {QStringLiteral("version"), 3},
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
        {QStringLiteral("autoReviewEnabled"), false},
        {QStringLiteral("reviewUseSameAsr"), true},
        {QStringLiteral("reviewAsrProvider"), QStringLiteral("funasr")},
        {QStringLiteral("reviewAsrModel"), QStringLiteral("Fun-ASR-Nano-2512")},
        {QStringLiteral("legacyAiCredentialIds"), QJsonArray{}},
        {QStringLiteral("omniReviewTimeoutMs"), 120000},
        {QStringLiteral("omniReviewCutConfidence"), 0.90},
        {QStringLiteral("omniReviewReviewConfidence"), 0.65},
        {QStringLiteral("omniReviewMinTailMs"), 150},
        {QStringLiteral("omniReviewPreferredTailMs"), 250},
        {QStringLiteral("omniReviewMaxTailMs"), 500},
        {QStringLiteral("omniReviewHandleMs"), 150},
        {QStringLiteral("omniReviewWindowMs"), 90000},
        {QStringLiteral("omniReviewOverlapMs"), 15000},
        {QStringLiteral("omniReviewCandidatePreMs"), 8000},
        {QStringLiteral("omniReviewCandidatePostMs"), 15000},
        {QStringLiteral("asrVerification"), QJsonObject{}},
        {QStringLiteral("asrConfigRevision"), 0},
        {QStringLiteral("asrCredentialRevision"), 0},
        {QStringLiteral("asrModel"), QStringLiteral("Fun-ASR-Nano-2512")},
        {QStringLiteral("asrProvider"), QStringLiteral("funasr")},
        {QStringLiteral("modelsRoot"), ModelLocator::compiledDefaultRoot()},
        {QStringLiteral("localAsrPython"), QString::fromUtf8(SUBCUE_PROJECT_INFERENCE_PYTHON)},
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
    const int loadedVersion = loaded.value(QStringLiteral("version")).toInt(1);
    const QStringList droppedAiKeys = {
        QStringLiteral("aiAssistEnabled"),
        QStringLiteral("aiProviderId"),
        QStringLiteral("aiProviders"),
        QStringLiteral("omniReviewEnabled"),
        QStringLiteral("omniReviewProvider"),
        QStringLiteral("omniReviewModel"),
        QStringLiteral("omniReviewBaseUrl"),
        QStringLiteral("omniReviewReasoningEffort"),
    };
    for (auto iterator = result.begin(); iterator != result.end(); ++iterator) {
        if (droppedAiKeys.contains(iterator.key())) continue;
        if (loaded.contains(iterator.key())) iterator.value() = loaded.value(iterator.key());
    }
    if (loadedVersion < 3) {
        QJsonArray leftoverIds = result.value(QStringLiteral("legacyAiCredentialIds")).toArray();
        for (const QJsonValue &value : loaded.value(QStringLiteral("aiProviders")).toArray()) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            bool exists = false;
            for (const QJsonValue &known : leftoverIds) {
                if (known.toString() == id) {
                    exists = true;
                    break;
                }
            }
            if (!exists) leftoverIds.append(id);
        }
        result.insert(QStringLiteral("legacyAiCredentialIds"), leftoverIds);
    }
    if (!loaded.contains(QStringLiteral("asrProvider"))) {
        // 旧配置中的模型名来自云端，不将其误认为本地 Fun-ASR 模型。
        result.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope"));
    }
    if (result.value(QStringLiteral("asrProvider")).toString() == QLatin1String("whisper")) {
        result.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope"));
        result.insert(QStringLiteral("asrVerification"), QJsonObject{});
    }
    if (!loaded.contains(QStringLiteral("modelsRoot"))) {
        const QString derived = ModelLocator::derivedRootFromLegacy(loaded);
        if (!derived.isEmpty()) result.insert(QStringLiteral("modelsRoot"), derived);
    }
    result.insert(QStringLiteral("qwen3AsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3Asr, result));
    result.insert(QStringLiteral("qwen3ForcedAlignerModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, result));
    result.insert(QStringLiteral("funAsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::FunAsrNano, result));
    result.insert(QStringLiteral("version"), 3);
    return result;
}

bool SettingsManager::save(const QJsonObject &settings, QString *errorMessage) const
{
    QJsonObject safe = defaults();
    for (auto iterator = safe.begin(); iterator != safe.end(); ++iterator) {
        if (settings.contains(iterator.key())) iterator.value() = settings.value(iterator.key());
    }
    safe.insert(QStringLiteral("qwen3AsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3Asr, safe));
    safe.insert(QStringLiteral("qwen3ForcedAlignerModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, safe));
    safe.insert(QStringLiteral("funAsrModelsDirectory"),
        ModelLocator::directoryFor(ModelKind::FunAsrNano, safe));
    safe.insert(QStringLiteral("version"), 3);
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
