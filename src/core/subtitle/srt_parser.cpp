#include "subtitle/srt_parser.h"

#include "subtitle/timecode.h"

#include <QtCore/QRegularExpression>

#include <utility>

namespace subcue {
namespace {

QString normalizeNewlines(QString content)
{
    content.remove(QChar::ByteOrderMark);
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace(QChar(u'\r'), QChar(u'\n'));
    return content;
}

} // namespace

QStringList SrtParser::parseTextOnly(const QString &content)
{
    static const QRegularExpression timing(QStringLiteral(
        R"(^\s*\d{1,3}:\d{2}:\d{2}[,.]\d{3}\s*-->\s*\d{1,3}:\d{2}:\d{2}[,.]\d{3})"));
    const QString normalized = normalizeNewlines(content);
    const QString trimmed = normalized.trimmed();
    if (trimmed.isEmpty()) return {};

    QStringList result;
    const QStringList blocks = trimmed.split(
        QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts);
    for (const QString &block : blocks) {
        const QStringList parts = block.split(QChar(u'\n'));
        qsizetype timingIndex = -1;
        for (qsizetype index = 0; index < parts.size(); ++index) {
            if (timing.match(parts.at(index)).hasMatch()) {
                timingIndex = index;
                break;
            }
        }
        if (timingIndex < 0) continue;
        QStringList text;
        for (qsizetype index = timingIndex + 1; index < parts.size(); ++index) {
            const QString line = parts.at(index).trimmed();
            if (!line.isEmpty()) text.append(line);
        }
        if (!text.isEmpty()) result.append(text.join(QChar(u'\n')));
    }
    return result;
}

std::optional<QList<Subtitle>> SrtParser::parseTimed(const QString &content, QString *errorMessage)
{
    static const QRegularExpression timing(QStringLiteral(
        R"(^\s*(\d{1,3}:\d{2}:\d{2}[,.]\d{3})\s*-->\s*(\d{1,3}:\d{2}:\d{2}[,.]\d{3})(?:\s+.*)?$)"));
    const QString normalized = normalizeNewlines(content);
    const QString trimmed = normalized.trimmed();
    if (trimmed.isEmpty()) return QList<Subtitle>{};

    QList<Subtitle> result;
    const QStringList blocks = trimmed.split(
        QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts);
    for (qsizetype blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
        const QStringList parts = blocks.at(blockIndex).split(QChar(u'\n'));
        qsizetype timingIndex = -1;
        QRegularExpressionMatch timingMatch;
        for (qsizetype index = 0; index < parts.size(); ++index) {
            timingMatch = timing.match(parts.at(index));
            if (timingMatch.hasMatch()) {
                timingIndex = index;
                break;
            }
        }
        if (timingIndex < 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 个字幕块缺少有效时间行").arg(blockIndex + 1);
            }
            return std::nullopt;
        }
        const auto start = timecode::parseSrt(timingMatch.captured(1));
        const auto end = timecode::parseSrt(timingMatch.captured(2));
        if (!start || !end || *end <= *start) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("第 %1 个字幕块时间范围无效").arg(blockIndex + 1);
            }
            return std::nullopt;
        }
        QStringList text;
        for (qsizetype index = timingIndex + 1; index < parts.size(); ++index) {
            const QString line = parts.at(index).trimmed();
            if (!line.isEmpty()) text.append(line);
        }
        Subtitle subtitle;
        subtitle.start = *start;
        subtitle.end = *end;
        subtitle.text = text.join(QChar(u'\n'));
        subtitle.source = QStringLiteral("srt");
        subtitle.status = QStringLiteral("IMPORTED");
        result.append(std::move(subtitle));
    }
    return result;
}

} // namespace subcue
