#include "settings/model_locator.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>

#ifndef SUBCUE_MODELS_ROOT
#define SUBCUE_MODELS_ROOT ""
#endif

namespace subcue {

QString ModelLocator::compiledDefaultRoot()
{
    return QDir::fromNativeSeparators(QString::fromUtf8(SUBCUE_MODELS_ROOT));
}

QString ModelLocator::root(const QJsonObject &settings)
{
    const QString fromSettings = QDir::fromNativeSeparators(
        settings.value(QStringLiteral("modelsRoot")).toString().trimmed());
    if (!fromSettings.isEmpty()) return QDir::cleanPath(fromSettings);

    const QString fromLegacy = derivedRootFromLegacy(settings);
    if (!fromLegacy.isEmpty()) return fromLegacy;

    const QString fromEnv = QDir::fromNativeSeparators(
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBCUE_MODELS_ROOT")).trimmed());
    if (!fromEnv.isEmpty()) return QDir::cleanPath(fromEnv);

    return QDir::cleanPath(compiledDefaultRoot());
}

QString ModelLocator::folderName(ModelKind kind)
{
    switch (kind) {
    case ModelKind::Qwen3Asr:
        return QStringLiteral("qwen3-asr-0.6b");
    case ModelKind::Qwen3ForcedAligner:
        return QStringLiteral("qwen3-forced-aligner-0.6b");
    case ModelKind::FunAsrNano:
        return QStringLiteral("fun-asr-nano-2512");
    }
    return {};
}

QString ModelLocator::directoryFor(ModelKind kind, const QJsonObject &settings)
{
    const QString key = kind == ModelKind::Qwen3Asr ? QStringLiteral("qwen3AsrModelsDirectory")
        : kind == ModelKind::Qwen3ForcedAligner ? QStringLiteral("qwen3ForcedAlignerModelsDirectory")
                                                : QStringLiteral("funAsrModelsDirectory");
    const QString explicitDir = QDir::fromNativeSeparators(settings.value(key).toString().trimmed());
    if (!explicitDir.isEmpty() && settings.value(QStringLiteral("modelsRoot")).toString().trimmed().isEmpty()) {
        return QDir::cleanPath(explicitDir);
    }
    return QDir(root(settings)).filePath(folderName(kind));
}

bool ModelLocator::modelReady(const QString &directory)
{
    const QDir dir(directory);
    const bool hasConfig = QFileInfo::exists(dir.filePath(QStringLiteral("config.json")))
        || QFileInfo::exists(dir.filePath(QStringLiteral("configuration.json")))
        || QFileInfo::exists(dir.filePath(QStringLiteral("config.yaml")));
    const bool hasWeights = QFileInfo::exists(dir.filePath(QStringLiteral("model.safetensors")))
        || QFileInfo::exists(dir.filePath(QStringLiteral("model.pt")));
    return hasConfig && hasWeights;
}

QString ModelLocator::derivedRootFromLegacy(const QJsonObject &loaded)
{
    const QString qwen = QDir::fromNativeSeparators(
        loaded.value(QStringLiteral("qwen3AsrModelsDirectory")).toString());
    if (qwen.endsWith(QLatin1String("/qwen3-asr-0.6b"), Qt::CaseInsensitive)
        || qwen.endsWith(QLatin1String("\\qwen3-asr-0.6b"), Qt::CaseInsensitive)) {
        return QDir::cleanPath(QFileInfo(qwen).absolutePath());
    }
    const QString fun = QDir::fromNativeSeparators(
        loaded.value(QStringLiteral("funAsrModelsDirectory")).toString());
    if (fun.endsWith(QLatin1String("/fun-asr-nano-2512"), Qt::CaseInsensitive)
        || fun.endsWith(QLatin1String("\\fun-asr-nano-2512"), Qt::CaseInsensitive)) {
        return QDir::cleanPath(QFileInfo(fun).absolutePath());
    }
    return {};
}

} // namespace subcue
