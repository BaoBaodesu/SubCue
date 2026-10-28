#include "tts/cloud_tts_provider.h"
#include "tts/audio_quality.h"
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QtEndian>
#include <QtCore/QSaveFile>

namespace subcue {
namespace {
QJsonObject voiceIdentity(const QJsonObject &binding)
{
    QJsonObject result;
    for (const auto &key : {QStringLiteral("cloudAccountId"), QStringLiteral("model"), QStringLiteral("region"), QStringLiteral("businessSpace"), QStringLiteral("providerVoiceId")}) result.insert(key, binding.value(key));
    return result;
}
bool finalizeFlashWav(const QString &path)
{
    QFile file(path); if (!file.open(QIODevice::ReadWrite)) return false;
    auto header = file.read(44);
    // 仅修复已实测的 Flash PCM 流占位长度，不接受任意声明长度不符的 WAV。
    if (header.size() != 44 || header.mid(0, 4) != "RIFF" || header.mid(8, 8) != "WAVEfmt " || header.mid(36, 4) != "data"
        || qFromLittleEndian<quint32>(header.constData() + 4) != 0x7fffffbfU
        || qFromLittleEndian<quint32>(header.constData() + 40) != 0x7fffff9bU) return true;
    if (qFromLittleEndian<quint32>(header.constData() + 16) != 16 || qFromLittleEndian<quint16>(header.constData() + 20) != 1
        || qFromLittleEndian<quint16>(header.constData() + 22) != 1 || qFromLittleEndian<quint32>(header.constData() + 24) != 24000
        || qFromLittleEndian<quint32>(header.constData() + 28) != 48000 || qFromLittleEndian<quint16>(header.constData() + 32) != 2
        || qFromLittleEndian<quint16>(header.constData() + 34) != 16 || file.size() <= 44 || file.size() > 128 * 1024 * 1024 || (file.size() - 44) % 2) return false;
    qToLittleEndian<quint32>(quint32(file.size() - 8), header.data() + 4);
    qToLittleEndian<quint32>(quint32(file.size() - 44), header.data() + 40);
    return file.seek(0) && file.write(header) == header.size() && file.flush();
}
}
QJsonObject TtsPricing::flashPrice()
{
    return {{QStringLiteral("model"), QStringLiteral("qwen-audio-3.1-tts-flash")}, {QStringLiteral("region"), QStringLiteral("beijing")},
        {QStringLiteral("currency"), QStringLiteral("CNY")}, {QStringLiteral("unit"), QStringLiteral("million_tokens")},
        {QStringLiteral("inputRate"), 1.5}, {QStringLiteral("outputRate"), 12.0}, {QStringLiteral("verifiedAt"), QStringLiteral("2026-10-14")},
        {QStringLiteral("version"), QStringLiteral("flash-beijing-20261014")}};
}
std::optional<double> TtsPricing::amount(const QJsonObject &usage, const QJsonObject &price)
{
    if (usage.value(QStringLiteral("state")).toString() != QStringLiteral("known")
        || price.value(QStringLiteral("currency")).toString() != QStringLiteral("CNY") || price.value(QStringLiteral("unit")).toString() != QStringLiteral("million_tokens")) return {};
    for (const auto &key : {QStringLiteral("inputTokens"), QStringLiteral("outputTokens")}) {
        const auto value = usage.value(key);
        if (!value.isDouble() || value.toDouble() < 0 || !std::isfinite(value.toDouble()) || value.toDouble() != double(value.toInteger(-1))) return {};
    }
    for (const auto &key : {QStringLiteral("inputRate"), QStringLiteral("outputRate")}) if (!price.value(key).isDouble() || price.value(key).toDouble() < 0 || !std::isfinite(price.value(key).toDouble())) return {};
    const auto value = (usage.value(QStringLiteral("inputTokens")).toDouble() * price.value(QStringLiteral("inputRate")).toDouble()
        + usage.value(QStringLiteral("outputTokens")).toDouble() * price.value(QStringLiteral("outputRate")).toDouble()) / 1000000;
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}
CloudTtsProvider::CloudTtsProvider(QString workspace, QString key, double budget, IHttpClient *http)
    : workspace_(std::move(workspace)), key_(std::move(key)), budget_(budget), http_(http) {}
std::optional<QString> CloudTtsProvider::voiceRevision(const QString &directory, const QJsonObject &binding, QString *error)
{
    const auto identity = voiceIdentity(binding);
    QFile file(QDir(directory).filePath(QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json")));
    if (!file.exists()) return QString();
    if (file.open(QIODevice::ReadOnly) && file.size() <= 16384) {
        const auto record = QJsonDocument::fromJson(file.readAll()).object(); const auto revision = record.value(QStringLiteral("revision")).toString();
        if (record.value(QStringLiteral("schemaVersion")).toInt() == 1 && record.value(QStringLiteral("identity")).toObject() == identity
            && !QUuid(revision).isNull() && QUuid(revision).toString(QUuid::WithoutBraces) == revision && record.value(QStringLiteral("blocked")).isBool()) {
            if (!record.value(QStringLiteral("blocked")).toBool()) return revision;
            if (error) *error = QStringLiteral("该云端音色正在更新或结果不明，请先在原账号控制台核对，再查询并重新选用。"); return {};
        }
    }
    if (error) *error = QStringLiteral("云端音色修订记录无法读取，请恢复有效备份后再生成。"); return {};
}
bool CloudTtsProvider::saveVoiceRevision(const QString &directory, const QJsonObject &binding, const QString &revision, bool blocked, QString *error)
{
    const auto identity = voiceIdentity(binding);
    if (QUuid(revision).isNull() || QUuid(revision).toString(QUuid::WithoutBraces) != revision || !QFileInfo(directory).isAbsolute()) { if (error) *error = QStringLiteral("云端音色修订身份无效，未发送更新。"); return false; }
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("identity"), identity}, {QStringLiteral("revision"), revision}, {QStringLiteral("blocked"), blocked}}).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(directory).filePath(QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json")));
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { if (error) *error = QStringLiteral("云端音色修订记录保存失败，禁止继续使用旧生成身份。"); return false; }
    return true;
}
TtsCapabilities CloudTtsProvider::capabilities() const
{
    return {QStringLiteral("qwen-cloud"), QStringLiteral("qwen-audio-3.1-tts-flash"), {QStringLiteral("Chinese")}, {QStringLiteral("seed"), QStringLiteral("speedFactor")}, true, false, false, true};
}
TtsBatchReport CloudTtsProvider::generate(const QVector<TtsJob> &jobs, const QString &outputDirectory,
    const std::atomic<bool> *cancel, const std::function<void(const TtsResult &)> &onResult, int timeoutMs,
    const std::function<void(const QString &)> &onStarted,
    const std::function<void(const QString &, const QJsonObject &, const QJsonObject &)> &onUsage,
    const std::function<std::optional<AudioQuality>(const QString &, QString *)> &inspect) const
{
    TtsBatchReport report;
    if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9-]{0,127}$")).match(workspace_).hasMatch()
        || key_.isEmpty() || key_.contains(u'\r') || key_.contains(u'\n') || !std::isfinite(budget_) || budget_ <= 0
        || jobs.isEmpty() || jobs.size() > 500 || !QFileInfo(outputDirectory).isDir()) { report.error = QStringLiteral("云业务空间、凭据、软预算或批次无效。"); return report; }
    QSet<QString> ids;
    for (const auto &job : jobs) {
        if (QUuid(job.id).isNull() || QUuid(job.id).toString(QUuid::WithoutBraces) != job.id || ids.contains(job.id)
            || !QFileInfo(outputDirectory).isAbsolute() || job.billing.value(QStringLiteral("price")).toObject().value(QStringLiteral("model")).toString() != job.input.model
            || job.billing.value(QStringLiteral("price")).toObject().value(QStringLiteral("region")).toString() != job.input.region) { report.error = QStringLiteral("任务身份、输出目录或价格快照无效，未调用云端。"); return report; }
        ids.insert(job.id);
        if (!job.input.validate(capabilities(), &report.error)) return report;
        const auto seed = job.input.effectiveParameters.value(QStringLiteral("seed"));
        if (job.input.region != QStringLiteral("beijing") || job.input.spokenText.size() > 4096
            || (!seed.isUndefined() && (!seed.isDouble() || seed.toInteger(-1) < 0 || seed.toInteger(-1) > 65535 || double(seed.toInteger()) != seed.toDouble()))) {
            report.error = QStringLiteral("Flash 首版仅支持北京、句级中文及 0～65535 的 seed。"); return report;
        }
    }
    QtNetworkHttpClient network; auto *http = http_ ? http_ : &network; double spent = 0;
    for (const auto &job : jobs) {
        if (cancel && cancel->load()) { report.canceled = true; break; }
        if (spent >= budget_) {
            report.error = QStringLiteral("实际已知费用达到软预算，未派发下一句。"); break;
        }
        if (onStarted) onStarted(job.id);
        QElapsedTimer timer; timer.start();
        HttpRequest request; request.url = QUrl(QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer").arg(workspace_));
        request.method = QByteArrayLiteral("POST"); request.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key_.toUtf8()}, {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
        request.followRedirects = false;
        request.maxResponseBytes = 64 * 1024;
        if (timeoutMs > 0) request.transferTimeoutMs = timeoutMs;
        QJsonObject input{{QStringLiteral("text"), job.input.spokenText}, {QStringLiteral("voice"), job.input.providerVoiceId},
            {QStringLiteral("format"), QStringLiteral("wav")}, {QStringLiteral("sample_rate"), 24000}, {QStringLiteral("language_hints"), QJsonArray{QStringLiteral("zh")}}};
        if (!job.input.effectiveParameters.value(QStringLiteral("seed")).isUndefined()) input.insert(QStringLiteral("seed"), job.input.effectiveParameters.value(QStringLiteral("seed")));
        if (!job.input.effectiveParameters.value(QStringLiteral("speedFactor")).isUndefined()) input.insert(QStringLiteral("rate"), job.input.effectiveParameters.value(QStringLiteral("speedFactor")));
        request.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), job.input.model}, {QStringLiteral("input"), input}}).toJson(QJsonDocument::Compact);
        const auto response = http->send(request, cancel);
        const auto fail = [&](const QString &code, const QString &message) { report.failures.append(QJsonObject{{QStringLiteral("jobId"), job.id}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}}); report.error = message; };
        if (std::holds_alternative<AppError>(response)) { fail(QStringLiteral("outcome_unknown"), QStringLiteral("合成请求网络失败或取消，费用与结果不确定；未自动重发。")); break; }
        const auto reply = std::get<HttpResponse>(response);
        if (reply.status < 200 || reply.status >= 300) {
            const auto details = QJsonDocument::fromJson(reply.body).object();
            if (!details.value(QStringLiteral("request_id")).toString().isEmpty() && onUsage) {
                const auto tokens = details.value(QStringLiteral("usage")).toObject(), price = job.billing.value(QStringLiteral("price")).toObject();
                QJsonObject usage{{QStringLiteral("state"), QStringLiteral("known")}, {QStringLiteral("inputTokens"), tokens.value(QStringLiteral("input_tokens"))}, {QStringLiteral("outputTokens"), tokens.value(QStringLiteral("output_tokens"))}};
                const auto amount = TtsPricing::amount(usage, price);
                if (!amount) usage = {{QStringLiteral("state"), QStringLiteral("unknown")}};
                usage.insert(QStringLiteral("price"), price); usage.insert(QStringLiteral("requestId"), details.value(QStringLiteral("request_id"))); if (amount) usage.insert(QStringLiteral("yuan"), *amount);
                onUsage(job.id, usage, {{QStringLiteral("requestId"), details.value(QStringLiteral("request_id"))}});
            }
            fail(reply.status >= 500 || reply.status == 408 ? QStringLiteral("outcome_unknown") : QStringLiteral("http_error"), QStringLiteral("Flash 返回 HTTP %1；批次已停止，未自动重发。").arg(reply.status)); break;
        }
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(reply.body, &parse); const auto root = document.object();
        if (parse.error != QJsonParseError::NoError || !document.isObject() || root.value(QStringLiteral("request_id")).toString().isEmpty()) { fail(QStringLiteral("outcome_unknown"), QStringLiteral("合成响应格式不完整，结果不确定；未自动重发。")); break; }
        const auto rawUsage = root.value(QStringLiteral("usage")).toObject();
        QJsonObject usage{{QStringLiteral("state"), QStringLiteral("known")}, {QStringLiteral("inputTokens"), rawUsage.value(QStringLiteral("input_tokens"))}, {QStringLiteral("outputTokens"), rawUsage.value(QStringLiteral("output_tokens"))}};
        const auto price = job.billing.value(QStringLiteral("price")).toObject();
        const auto amount = TtsPricing::amount(usage, price);
        if (!amount) usage = {{QStringLiteral("state"), QStringLiteral("unknown")}};
        usage.insert(QStringLiteral("price"), price); if (amount) usage.insert(QStringLiteral("yuan"), *amount);
        if (amount) usage.insert(QStringLiteral("batchKnownYuan"), spent + *amount);
        const auto audio = root.value(QStringLiteral("output")).toObject().value(QStringLiteral("audio")).toObject();
        QJsonObject metadata{{QStringLiteral("requestId"), root.value(QStringLiteral("request_id"))}, {QStringLiteral("expiresAt"), audio.value(QStringLiteral("expires_at"))}};
        usage.insert(QStringLiteral("requestId"), root.value(QStringLiteral("request_id"))); usage.insert(QStringLiteral("expiresAt"), audio.value(QStringLiteral("expires_at")));
        if (onUsage) onUsage(job.id, usage, metadata);
        auto url = QUrl(audio.value(QStringLiteral("url")).toString());
        // 官方 OSS 结果可能返回 HTTP 签名地址；只对北京 OSS 升级 HTTPS，不做明文回退。
        if (url.scheme() == QStringLiteral("http") && url.host().endsWith(QStringLiteral(".oss-cn-beijing.aliyuncs.com")) && url.port(-1) == -1) url.setScheme(QStringLiteral("https"));
        if (root.value(QStringLiteral("output")).toObject().value(QStringLiteral("finish_reason")).toString() != QStringLiteral("stop")
            || url.scheme() != QStringLiteral("https") || !url.host().endsWith(QStringLiteral(".oss-cn-beijing.aliyuncs.com")) || !url.userInfo().isEmpty()
            || (url.port(-1) != -1 && url.port() != 443) || audio.value(QStringLiteral("expires_at")).toInteger() <= QDateTime::currentSecsSinceEpoch()) {
            fail(QStringLiteral("download_invalid"), QStringLiteral("音频地址无效、不安全或已过期；用量已保存，未重新合成。")); break;
        }
        const auto path = QDir(outputDirectory).filePath(job.id + QStringLiteral(".wav")); QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { fail(QStringLiteral("publish_failed"), QStringLiteral("无法创建结果文件；用量已保存。")); break; }
        HttpRequest download; download.url = url; download.followRedirects = false; download.maxDownloadBytes = 128 * 1024 * 1024; std::variant<qint64, AppError> downloaded = AppError(ErrorDomain::Network, 0, QStringLiteral("下载未执行"));
        if (timeoutMs > 0) download.transferTimeoutMs = timeoutMs;
        // 仅重试已成功合成的下载，不再次发送付费 POST。
        for (int attempt = 0; attempt < 2 && !(cancel && cancel->load()); ++attempt) { file.resize(0); file.seek(0); downloaded = http->download(download, &file, 0, cancel); if (std::holds_alternative<qint64>(downloaded)) break; }
        file.close(); QString error;
        const bool finalized = !std::holds_alternative<qint64>(downloaded) || finalizeFlashWav(path);
        if (!finalized) error = QStringLiteral("Flash WAV 占位长度与 PCM 规格不匹配，拒绝修复。");
        const auto quality = std::holds_alternative<qint64>(downloaded) && finalized ? (inspect ? inspect(path, &error) : AudioQuality::inspect(path, &error, cancel)) : std::nullopt;
        if (!quality) {
            QFile::remove(path);
            const auto networkError = std::get_if<AppError>(&downloaded);
            QString reason = networkError ? networkError->userMessage() : error;
            reason.replace(key_, QStringLiteral("[凭据已隐藏]"));
            reason.replace(QRegularExpression(QStringLiteral("https?://[^\\s]+")), QStringLiteral("[地址已隐藏]"));
            fail(QStringLiteral("download_failed"), QStringLiteral("%1：%2；用量已保存，未重新合成。")
                .arg(networkError ? QStringLiteral("音频下载失败（错误码 %1）").arg(networkError->code()) : QStringLiteral("音频解码／校验失败"), reason.left(1000)));
            auto failure = report.failures.last().toObject();
            failure.insert(QStringLiteral("stage"), networkError ? QStringLiteral("download") : QStringLiteral("decode"));
            if (networkError) failure.insert(QStringLiteral("networkCode"), networkError->code());
            failure.insert(QStringLiteral("requestId"), root.value(QStringLiteral("request_id")));
            report.failures[report.failures.size() - 1] = failure;
            break;
        }
        TtsResult result; result.jobId = job.id; result.inputHash = job.input.inputHash(); result.audioPath = path; result.sampleRate = quality->sampleRate; result.sampleCount = quality->sampleCount; result.channels = quality->channels;
        result.requestId = root.value(QStringLiteral("request_id")).toString(); result.elapsedMs = timer.elapsed(); result.usage = usage; result.diagnostics = metadata;
        report.results.append(result); if (onResult) onResult(result);
        if (!amount) { report.error = QStringLiteral("候选已保存，但实际用量未知；暂停后续请求。" ); break; }
        spent += *amount;
    }
    report.canceled = cancel && cancel->load(); return report;
}
}
