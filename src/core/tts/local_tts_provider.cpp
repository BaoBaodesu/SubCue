#include "tts/local_tts_provider.h"
#include "inference/inference_manager.h"
#include "common/file_path_guard.h"
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QSet>
#include <QtCore/QHash>
#include <QtCore/QTemporaryDir>
#include <utility>

namespace subcue {
LocalTtsProvider::LocalTtsProvider(Kind kind, QString python, QString worker, QString modelDirectory)
    : kind_(kind), python_(std::move(python)), worker_(std::move(worker)), modelDirectory_(std::move(modelDirectory)) {}

TtsCapabilities LocalTtsProvider::capabilities() const
{
    if (kind_ == Kind::Index25) return {QStringLiteral("index-local"), QStringLiteral("IndexTTS-2.5"), {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}, {QStringLiteral("seed"), QStringLiteral("durationFactor"), QStringLiteral("emotion"), QStringLiteral("emotionStrength")}, false, true, false, false};
    if (kind_ == Kind::GptProPlus) return {QStringLiteral("gpt-local"), QStringLiteral("GPT-SoVITS-v2ProPlus"), {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}, {QStringLiteral("seed"), QStringLiteral("speedFactor")}, false, true, true, false, {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}};
    const bool mock = kind_ == Kind::Mock;
    return {mock ? QStringLiteral("mock") : QStringLiteral("qwen-local"),
        mock ? QStringLiteral("mock-tone") : QStringLiteral("Qwen3-TTS-12Hz-0.6B-Base"),
        mock ? QStringList{QStringLiteral("Chinese")} : QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}, {QStringLiteral("seed")}, false, !mock, !mock, false};
}
QJsonObject LocalTtsProvider::check(QString *error, const std::atomic<bool> *cancel) const
{
    QJsonObject result;
    QTemporaryDir cache;
    if (!cache.isValid()) { if (error) *error = QStringLiteral("无法创建预检缓存目录。"); return {}; }
    const auto input = QJsonDocument(QJsonObject{{QStringLiteral("action"), QStringLiteral("check")}, {QStringLiteral("provider"), capabilities().provider}, {QStringLiteral("modelDirectory"), modelDirectory_}, {QStringLiteral("outputDirectory"), cache.path()}}).toJson(QJsonDocument::Compact) + '\n';
    const auto process = InferenceManager::instance().run(python_, {QStringLiteral("-B"), worker_}, input, cancel, 120000, [&](const QByteArray &line) {
        const auto event = QJsonDocument::fromJson(line).object();
        if (event.value(QStringLiteral("type")).toString() == QStringLiteral("check")) result = event;
        else if (error) *error = event.value(QStringLiteral("message")).toString();
    }, QStringLiteral("本地 TTS 独立环境／离线模型预检"));
    if (!process.started || process.exitCode != 0 || result.value(QStringLiteral("modelVersion")).toString().isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("本地 TTS 预检失败或已取消：%1").arg(QString::fromUtf8(process.standardError).right(2000)); return {};
    }
    result.insert(QStringLiteral("provider"), capabilities().provider);
    return result;
}

TtsBatchReport LocalTtsProvider::generate(const QVector<TtsJob> &jobs, const QString &outputDirectory,
    const std::atomic<bool> *cancel, const std::function<void(const TtsResult &)> &onResult, int timeoutMs,
    const std::function<void(const QString &)> &onStarted) const
{
    TtsBatchReport report;
    if (cancel && cancel->load()) { report.canceled = true; return report; }
    if (jobs.isEmpty() || jobs.size() > 500 || !QFileInfo(python_).isFile() || !QFileInfo(worker_).isFile()
        || !QFileInfo(outputDirectory).isDir() || !QFileInfo(outputDirectory).isAbsolute()) {
        report.error = QStringLiteral("批次、独立 Python、Worker 或输出目录配置无效。"); return report;
    }
    const bool mock = kind_ == Kind::Mock;
    QHash<QString, const TtsJob *> inputs;
    QJsonArray requests;
    for (const auto &job : jobs) {
        if (QUuid(job.id).isNull() || QUuid(job.id).toString(QUuid::WithoutBraces) != job.id || inputs.contains(job.id)
            || !job.input.validate(capabilities(), &report.error)) {
            if (report.error.isEmpty()) report.error = QStringLiteral("任务 UUID 无效或重复。");
            return report;
        }
        if (!mock && (!QFileInfo(job.referencePath).isFile() || !QFileInfo(job.referencePath).isAbsolute())) {
            report.error = QStringLiteral("参考资产未解析为本地文件。"); return report;
        }
        const auto seed = job.input.effectiveParameters.value(QStringLiteral("seed"));
        if (!seed.isUndefined() && (!seed.isDouble() || seed.toInteger(-1) < 0 || seed.toInteger(-1) > 2147483647
            || seed.toDouble() != static_cast<double>(seed.toInteger(-1)))) {
            report.error = QStringLiteral("seed 必须是 0–2147483647 的整数。"); return report;
        }
        auto request = job.input.snapshot();
        request.insert(QStringLiteral("jobId"), job.id);
        if (!mock) request.insert(QStringLiteral("referencePath"), job.referencePath);
        else request.insert(QStringLiteral("simulate"), job.simulation);
        requests.append(request); inputs.insert(job.id, &job);
    }
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("action"), QStringLiteral("batch")},
        {QStringLiteral("provider"), capabilities().provider},
        {QStringLiteral("modelDirectory"), modelDirectory_}, {QStringLiteral("outputDirectory"), outputDirectory},
        {QStringLiteral("jobs"), requests}}).toJson(QJsonDocument::Compact) + '\n';
    if (payload.size() > 2 * 1024 * 1024) { report.error = QStringLiteral("批次请求超过 2MiB，请缩小批次。"); return report; }
    QSet<QString> terminal;
    QSet<QString> started;
    bool finished = false;
    const auto process = InferenceManager::instance().run(python_, {QStringLiteral("-B"), worker_}, payload, cancel,
        timeoutMs, [&](const QByteArray &line) {
            if (!report.error.isEmpty() || (cancel && cancel->load())) return;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                report.error = QStringLiteral("TTS Worker 输出了无效 JSON 协议。"); return;
            }
            const auto event = document.object();
            const QString type = event.value(QStringLiteral("type")).toString();
            if (finished) { report.error = QStringLiteral("批次结束后仍收到结果。"); return; }
            if (type == QStringLiteral("batch_finished")) {
                finished = event.value(QStringLiteral("mock")).isBool() && event.value(QStringLiteral("mock")).toBool() == mock;
                if (!finished) report.error = QStringLiteral("批次 Provider 标记不匹配。");
                return;
            }
            if (type == QStringLiteral("fatal")) {
                report.error = event.value(QStringLiteral("message")).toString();
                if (report.error.isEmpty()) report.error = QStringLiteral("TTS Worker 预检失败。");
                auto failure = event; failure.insert(QStringLiteral("jobId"), jobs.first().id);
                report.failures.append(failure); return;
            }
            const QString id = event.value(QStringLiteral("jobId")).toString();
            if (!inputs.contains(id) || terminal.contains(id) || !event.value(QStringLiteral("mock")).isBool()
                || event.value(QStringLiteral("mock")).toBool() != mock) {
                report.error = QStringLiteral("Worker 返回陌生、重复或 Provider 不匹配的任务。"); return;
            }
            if (type == QStringLiteral("job_started")) {
                if (started.contains(id) || event.value(QStringLiteral("inputHash")).toString() != QString::fromLatin1(inputs.value(id)->input.inputHash().toHex())) {
                    report.error = QStringLiteral("Worker 开始事件重复或输入指纹不匹配。"); return;
                }
                started.insert(id);
                if (onStarted) {
                    try { onStarted(id); }
                    catch (...) { report.error = QStringLiteral("开始状态提交失败，已停止本批次。"); }
                }
                return;
            }
            if (!started.contains(id)) { report.error = QStringLiteral("未开始的任务不能提交结果。"); return; }
            if (type == QStringLiteral("error")) { terminal.insert(id); report.failures.append(event); return; }
            if (type != QStringLiteral("result")) { report.error = QStringLiteral("未知 Worker 事件。"); return; }
            const QString path = event.value(QStringLiteral("audioPath")).toString();
            if (event.value(QStringLiteral("inputHash")).toString() != QString::fromLatin1(inputs.value(id)->input.inputHash().toHex())
                || !sameFilePath(path, QDir(outputDirectory).filePath(id + QStringLiteral(".wav")))
                || !QFileInfo(path).isFile() || QFileInfo(path).isSymLink() || QFileInfo(path).size() <= 0) {
                report.error = QStringLiteral("结果指纹或输出路径无效。"); return;
            }
            if (event.value(QStringLiteral("sampleRate")).toInt() <= 0 || event.value(QStringLiteral("channels")).toInt() != 1
                || event.value(QStringLiteral("sampleCount")).toInteger() <= 0) {
                terminal.insert(id);
                report.failures.append(QJsonObject{{QStringLiteral("jobId"), id}, {QStringLiteral("mock"), mock},
                    {QStringLiteral("code"), QStringLiteral("invalid_audio")}, {QStringLiteral("message"), QStringLiteral("原生音频规格无效。")}});
                return;
            }
            TtsResult result;
            result.jobId = id; result.inputHash = inputs.value(id)->input.inputHash(); result.audioPath = path;
            result.sampleRate = event.value(QStringLiteral("sampleRate")).toInt(); result.channels = 1;
            result.sampleCount = event.value(QStringLiteral("sampleCount")).toInteger();
            result.elapsedMs = event.value(QStringLiteral("elapsedMs")).toInteger(); result.mock = mock;
            result.diagnostics = event.value(QStringLiteral("diagnostics")).toObject();
            if (!mock && result.diagnostics.value(QStringLiteral("modelVersion")).toString() != inputs.value(id)->input.modelVersion) { report.error = QStringLiteral("实际模型版本诊断不匹配，未提交结果。"); return; }
            terminal.insert(id); report.results.append(result);
            if (onResult) {
                try { onResult(result); }
                catch (...) { report.error = QStringLiteral("结果提交回调失败，已生成文件仍保留在输出目录。"); }
            }
        }, mock ? QStringLiteral("Mock 配音批次（测试）") : QStringLiteral("Qwen 本地配音批次"), true,
        [&report] { return !report.error.isEmpty(); });
    report.canceled = cancel && cancel->load() && !finished;
    if (report.error.isEmpty() && !report.canceled && (!process.started || process.timedOut || process.exitCode != 0))
        report.error = process.timedOut ? QStringLiteral("TTS Worker 超时。") : QStringLiteral("TTS Worker 未启动或异常退出：%1").arg(QString::fromUtf8(process.standardError).right(4000));
    if (report.error.isEmpty() && !report.canceled && (!finished || terminal.size() != jobs.size()))
        report.error = QStringLiteral("Worker 未完整报告每个句级任务，不能作为成功批次。");
    return report;
}
} // namespace subcue
