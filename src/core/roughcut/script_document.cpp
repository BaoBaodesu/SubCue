#include "roughcut/script_document.h"

#include "inference/inference_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QStringDecoder>

namespace subcue {

ScriptDocumentResult ScriptDocumentImporter::loadTxt(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("无法读取文案文件"), path);
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(file.readAll());
    if (decoder.hasError()) {
        return AppError(ErrorDomain::Validation, 2, QStringLiteral("文案必须使用 UTF-8 编码"), path);
    }
    ScriptDocument document;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    document.lines.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        QString line = lines.at(index);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        document.lines.append({index + 1, index + 1, false, line});
    }
    return document;
}

ScriptDocumentResult ScriptDocumentImporter::loadDocx(
    const QString &path,
    const QString &inferenceExecutable,
    const std::atomic<bool> *cancel)
{
    QString program = inferenceExecutable;
    if (program.isEmpty()) {
        program = QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("inference/SubCueInference.exe"));
    }
    if (!QFileInfo::exists(path) || !QFileInfo::exists(program)) {
        return AppError(ErrorDomain::Validation, 1,
            QStringLiteral("DOCX 文案或推理 Worker 不存在"), path);
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("无法创建 DOCX 解析临时目录"));
    }
    const QString resultPath = QDir(temporary.path()).filePath(QStringLiteral("document.json"));
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("taskId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("action"), QStringLiteral("parse-docx")},
        {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
        {QStringLiteral("resultPath"), resultPath}}).toJson(QJsonDocument::Compact) + '\n';
    const InferenceProcessResult result = InferenceManager::instance().run(
        program, {QStringLiteral("--stdio")}, request, cancel, 60'000);
    if (result.cancelled) {
        return AppError(ErrorDomain::Validation, 4, QStringLiteral("任务已取消"));
    }
    if (!result.started || result.timedOut || result.exitCode != 0) {
        return AppError(ErrorDomain::Validation, 3, QStringLiteral("DOCX 文案解析失败"),
            QString::fromUtf8(result.standardError + result.standardOutput));
    }
    QFile output(resultPath);
    if (!output.open(QIODevice::ReadOnly)) {
        return AppError(ErrorDomain::Validation, 3, QStringLiteral("DOCX 文案解析结果不存在"));
    }
    return fromWorkerResult(QJsonDocument::fromJson(output.readAll()).object());
}

ScriptDocumentResult ScriptDocumentImporter::fromWorkerResult(const QJsonObject &result)
{
    if (!result.value(QStringLiteral("lines")).isArray()) {
        return AppError(ErrorDomain::Validation, 3, QStringLiteral("DOCX 文案解析结果无效"));
    }
    ScriptDocument document;
    for (const QJsonValue &value : result.value(QStringLiteral("lines")).toArray()) {
        const QJsonObject line = value.toObject();
        const int lineNumber = line.value(QStringLiteral("lineNumber")).toInt();
        const int paragraph = line.value(QStringLiteral("paragraph")).toInt();
        if (lineNumber <= 0 || paragraph <= 0) {
            return AppError(ErrorDomain::Validation, 3, QStringLiteral("DOCX 文案行号无效"));
        }
        document.lines.append({lineNumber, paragraph,
            line.value(QStringLiteral("table")).toBool(),
            line.value(QStringLiteral("text")).toString()});
    }
    return document;
}

} // namespace subcue
