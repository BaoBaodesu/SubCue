#pragma once

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QUuid>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <optional>

namespace subcue {

inline bool validateTtsParameters(const QJsonObject &parameters, QString *error = nullptr)
{
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        bool valid = false;
        if (it.key() == QStringLiteral("seed")) valid = it.value().isDouble() && it.value().toInteger(-1) >= 0
            && it.value().toInteger() <= 2147483647 && it.value().toDouble() == static_cast<double>(it.value().toInteger());
        else if (it.key() == QStringLiteral("durationFactor")) valid = it.value().isDouble() && std::isfinite(it.value().toDouble()) && it.value().toDouble() >= 0.5 && it.value().toDouble() <= 2;
        else if (it.key() == QStringLiteral("speedFactor")) valid = it.value().isDouble() && std::isfinite(it.value().toDouble()) && it.value().toDouble() >= 0.5 && it.value().toDouble() <= 2;
        else if (it.key() == QStringLiteral("emotion")) valid = it.value().isString() && QStringList{QStringLiteral("happy"), QStringLiteral("angry"), QStringLiteral("sad"), QStringLiteral("afraid"), QStringLiteral("disgusted"), QStringLiteral("depressed"), QStringLiteral("surprised"), QStringLiteral("calm")}.contains(it.value().toString());
        else if (it.key() == QStringLiteral("emotionStrength")) valid = it.value().isDouble() && std::isfinite(it.value().toDouble()) && it.value().toDouble() > 0 && it.value().toDouble() <= 1;
        if (!valid) { if (error) *error = QStringLiteral("生成参数无效：%1。seed 需为整数；Index 时长倍率和语速倍率为 0.5～2，情绪强度大于 0 且不超过 1。").arg(it.key()); return false; }
    }
    if (parameters.contains(QStringLiteral("emotion")) != parameters.contains(QStringLiteral("emotionStrength"))) {
        if (error) *error = QStringLiteral("情绪类别与强度必须一起保存。"); return false;
    }
    return true;
}

struct TtsCapabilities {
    QString provider;
    QString model;
    QStringList languages;
    QStringList parameters;
    bool cloud = false;
    bool referenceRequired = false;
    bool transcriptRequired = false;
    bool voiceIdRequired = false;
};

// 只保存不可变生成输入；路径由资产层解析，凭据由 Provider 在调用时取得。
struct TtsRequest {
    QString projectId;
    quint64 projectGeneration = 0;
    QString segmentId;
    qint64 inputVersion = 0;
    QString spokenText;
    QString language;
    QString provider;
    QString model;
    QString modelVersion;
    QString voiceVersionId;
    QString referenceAssetId;
    QByteArray referenceSha256;
    QString referenceText;
    QString referenceLanguage = QStringLiteral("Chinese");
    QString providerVoiceId;
    QString region;
    QJsonObject effectiveParameters;

    [[nodiscard]] QJsonObject generationInput() const
    {
        QJsonObject result{{QStringLiteral("hashVersion"), 1}, {QStringLiteral("spokenText"), spokenText},
            {QStringLiteral("language"), language}, {QStringLiteral("provider"), provider},
            {QStringLiteral("model"), model}, {QStringLiteral("modelVersion"), modelVersion},
            {QStringLiteral("voiceVersionId"), voiceVersionId},
            {QStringLiteral("referenceSha256"), QString::fromLatin1(referenceSha256.toHex())},
            {QStringLiteral("referenceText"), referenceText}, {QStringLiteral("providerVoiceId"), providerVoiceId},
            {QStringLiteral("region"), region}, {QStringLiteral("parameters"), effectiveParameters}};
        // 旧任务默认中文，保持已有 v1 指纹；其它参考语言必须进入不可变输入。
        if (!referenceAssetId.isEmpty() && referenceLanguage != QStringLiteral("Chinese")) result.insert(QStringLiteral("referenceLanguage"), referenceLanguage);
        return result;
    }

    [[nodiscard]] QByteArray inputHash() const
    {
        return QCryptographicHash::hash(QJsonDocument(generationInput()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }

    [[nodiscard]] QJsonObject snapshot() const
    {
        auto result = generationInput();
        result.insert(QStringLiteral("projectId"), projectId);
        // JSON 数字不能可靠传输任意 quint64，工程代次以十进制字符串保存。
        result.insert(QStringLiteral("projectGeneration"), QString::number(projectGeneration));
        result.insert(QStringLiteral("segmentId"), segmentId);
        result.insert(QStringLiteral("inputVersion"), QString::number(inputVersion));
        result.insert(QStringLiteral("referenceAssetId"), referenceAssetId);
        if (!referenceAssetId.isEmpty()) result.insert(QStringLiteral("referenceLanguage"), referenceLanguage);
        result.insert(QStringLiteral("inputHash"), QString::fromLatin1(inputHash().toHex()));
        return result;
    }

    [[nodiscard]] bool validate(const TtsCapabilities &capabilities, QString *error = nullptr) const
    {
        const auto reject = [error](const QString &message) {
            if (error) *error = message;
            return false;
        };
        if (QUuid(projectId).isNull() || QUuid(segmentId).isNull() || QUuid(voiceVersionId).isNull()
            || projectGeneration == 0 || inputVersion < 0 || spokenText.trimmed().isEmpty())
            return reject(QStringLiteral("生成输入缺少有效工程、片段、声音版本或朗读文本。"));
        if (provider != capabilities.provider || model != capabilities.model || modelVersion.isEmpty()
            || !capabilities.languages.contains(language))
            return reject(QStringLiteral("Provider、实际模型或目标语言不匹配。"));
        if (provider != QStringLiteral("index-local") && QRegularExpression(QStringLiteral("<[^<>|]+\\|[^<>|]+>")).match(spokenText).hasMatch())
            return reject(QStringLiteral("当前引擎不支持 Index 发音标注，请先修改手工朗读文本。"));
        if (capabilities.referenceRequired && (QUuid(referenceAssetId).isNull() || referenceSha256.size() != 32))
            return reject(QStringLiteral("克隆需要已保存的参考资产及内容指纹。"));
        if (capabilities.transcriptRequired && referenceText.trimmed().isEmpty())
            return reject(QStringLiteral("当前克隆型号需要参考转写。"));
        if (capabilities.referenceRequired && !capabilities.languages.contains(referenceLanguage))
            return reject(QStringLiteral("当前引擎尚未接入此参考语言，请选择已验收的引擎或参考版本。"));
        if (capabilities.voiceIdRequired && providerVoiceId.trimmed().isEmpty())
            return reject(QStringLiteral("云端合成需要系统音色或已有 Voice ID。"));
        if (capabilities.cloud && region.trimmed().isEmpty())
            return reject(QStringLiteral("云端合成缺少地域。"));
        if (!capabilities.referenceRequired && (!referenceAssetId.isEmpty() || !referenceSha256.isEmpty() || !referenceText.isEmpty()))
            return reject(QStringLiteral("当前 Provider 不接受参考音频，不能隐式上传。"));
        if (!capabilities.voiceIdRequired && !providerVoiceId.isEmpty())
            return reject(QStringLiteral("本地参考克隆不能使用云 Voice ID。"));
        if (!validateTtsParameters(effectiveParameters, error)) return false;
        for (auto it = effectiveParameters.begin(); it != effectiveParameters.end(); ++it) {
            if (!capabilities.parameters.contains(it.key()))
                return reject(QStringLiteral("当前型号不支持参数：%1。请作为未生效备注保存。").arg(it.key()));
            if (!it.value().isDouble() && !it.value().isBool() && !it.value().isString())
                return reject(QStringLiteral("生成参数必须是明确的标量值。"));
            if (it.value().isDouble() && !std::isfinite(it.value().toDouble()))
                return reject(QStringLiteral("生成参数不是有限数值。"));
        }
        return true;
    }

    [[nodiscard]] static std::optional<TtsRequest> fromSnapshot(const QJsonObject &data,
        const TtsCapabilities &capabilities, QString *error = nullptr)
    {
        const auto reject = [error](const QString &message) -> std::optional<TtsRequest> {
            if (error) *error = message;
            return {};
        };
        if (data.value(QStringLiteral("hashVersion")).toInt(-1) != 1 || !data.value(QStringLiteral("parameters")).isObject())
            return reject(QStringLiteral("任务没有可恢复的 TTS v1 输入，请重新生成新任务。"));
        for (const auto &key : {"projectId", "projectGeneration", "segmentId", "inputVersion", "spokenText", "language",
            "provider", "model", "modelVersion", "voiceVersionId", "referenceAssetId", "referenceSha256",
            "referenceText", "providerVoiceId", "region", "inputHash"}) {
            if (!data.value(QString::fromLatin1(key)).isString()) return reject(QStringLiteral("任务输入字段缺失或类型错误。"));
        }
        TtsRequest result;
        result.projectId = data.value(QStringLiteral("projectId")).toString();
        result.segmentId = data.value(QStringLiteral("segmentId")).toString();
        bool generationOk = false, versionOk = false;
        const auto generation = data.value(QStringLiteral("projectGeneration")).toString();
        const auto version = data.value(QStringLiteral("inputVersion")).toString();
        result.projectGeneration = generation.toULongLong(&generationOk);
        result.inputVersion = version.toLongLong(&versionOk);
        if (!generationOk || !versionOk || generation != QString::number(result.projectGeneration) || version != QString::number(result.inputVersion))
            return reject(QStringLiteral("工程代次或输入版本无效。"));
        result.spokenText = data.value(QStringLiteral("spokenText")).toString();
        result.language = data.value(QStringLiteral("language")).toString();
        result.provider = data.value(QStringLiteral("provider")).toString();
        result.model = data.value(QStringLiteral("model")).toString();
        result.modelVersion = data.value(QStringLiteral("modelVersion")).toString();
        result.voiceVersionId = data.value(QStringLiteral("voiceVersionId")).toString();
        result.referenceAssetId = data.value(QStringLiteral("referenceAssetId")).toString();
        const auto referenceHash = data.value(QStringLiteral("referenceSha256")).toString();
        result.referenceSha256 = QByteArray::fromHex(referenceHash.toLatin1());
        if (QString::fromLatin1(result.referenceSha256.toHex()) != referenceHash)
            return reject(QStringLiteral("参考指纹编码无效。"));
        result.referenceText = data.value(QStringLiteral("referenceText")).toString();
        if (data.contains(QStringLiteral("referenceLanguage"))) {
            if (!data.value(QStringLiteral("referenceLanguage")).isString()) return reject(QStringLiteral("参考语言类型无效。"));
            result.referenceLanguage = data.value(QStringLiteral("referenceLanguage")).toString();
        }
        result.providerVoiceId = data.value(QStringLiteral("providerVoiceId")).toString();
        result.region = data.value(QStringLiteral("region")).toString();
        result.effectiveParameters = data.value(QStringLiteral("parameters")).toObject();
        if (!result.validate(capabilities, error)) return {};
        if (data.value(QStringLiteral("inputHash")).toString() != QString::fromLatin1(result.inputHash().toHex()))
            return reject(QStringLiteral("任务输入被修改或指纹损坏，不能按原任务恢复。"));
        return result;
    }
};

struct TtsResult {
    QString jobId;
    QByteArray inputHash;
    QString audioPath;
    int sampleRate = 0;
    int channels = 0;
    qint64 sampleCount = 0;
    qint64 elapsedMs = 0;
    QString requestId;
    QJsonObject usage{{QStringLiteral("state"), QStringLiteral("unknown")}};
    QJsonObject diagnostics;
    bool mock = false;
};

} // namespace subcue
