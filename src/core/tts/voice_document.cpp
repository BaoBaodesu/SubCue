#include "tts/voice_document.h"
#include "tts/tts_types.h"
#include <QtCore/QUuid>
#include <QtCore/QSet>
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QDate>
#include <cmath>
#include <limits>

namespace subcue {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString readNumber(const QString &text)
{
    const auto digits = QStringLiteral("零一二三四五六七八九");
    const auto readDigits = [&digits](const QString &value) { QString result; for (const auto c : value) result += c == u'.' ? QStringLiteral("点") : QString(digits[c.digitValue()]); return result; };
    if (text.startsWith(u'0') && text.size() > 1 && text[1] != u'.') return readDigits(text);
    const auto parts = text.split(u'.'); const auto whole = parts.first();
    if (whole.size() > 8) return readDigits(text);
    const auto group = [&digits](int number) {
        if (!number) return QString(digits[0]); QString result; bool zero = false;
        const QStringList units{QStringLiteral("千"), QStringLiteral("百"), QStringLiteral("十"), QString()};
        for (int divisor = 1000, i = 0; divisor; divisor /= 10, ++i) {
            const int value = number / divisor % 10;
            if (!value) { if (!result.isEmpty()) zero = true; continue; }
            if (zero) result += digits[0]; zero = false;
            if (value != 1 || divisor != 10 || !result.isEmpty()) result += digits[value]; result += units[i];
        }
        return result;
    };
    const int number = whole.toInt(); QString result;
    if (number >= 10000) { result = group(number / 10000) + QStringLiteral("万"); if (number % 10000) { if (number % 10000 < 1000) result += digits[0]; result += group(number % 10000); } }
    else result = group(number);
    if (parts.size() > 1) result += QStringLiteral("点") + readDigits(parts[1]); return result;
}
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
void archive(QJsonObject &document, const QJsonObject &segment)
{
    auto history = document.value(QStringLiteral("archivedSegments")).toArray(); history.append(segment);
    document.insert(QStringLiteral("archivedSegments"), history);
}
void revise(const QJsonObject &document, const QJsonObject &old, QJsonObject &updated)
{
    if (VoiceDocument::inputKey(document, old) != VoiceDocument::inputKey(document, updated))
        updated.insert(QStringLiteral("inputVersion"), QString::number(old.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1));
}
}
QJsonObject VoiceDocument::empty()
{
    return {{QStringLiteral("version"), 2}, {QStringLiteral("defaultVoiceVersionId"), QString()}, {QStringLiteral("paragraphs"), QJsonArray{}},
        {QStringLiteral("segments"), QJsonArray{}}, {QStringLiteral("archivedSegments"), QJsonArray{}}, {QStringLiteral("replacements"), QJsonArray{}}};
}
bool VoiceDocument::validate(const QJsonObject &document, QString *error)
{
    if ((document.value(QStringLiteral("version")).toInt(-1) != 1 && document.value(QStringLiteral("version")).toInt(-1) != 2) || !document.value(QStringLiteral("paragraphs")).isArray()
        || !document.value(QStringLiteral("segments")).isArray() || !document.value(QStringLiteral("archivedSegments")).isArray()
        || !document.value(QStringLiteral("replacements")).isArray() || QJsonDocument(document).toJson(QJsonDocument::Compact).size() > 16 * 1024 * 1024)
        return fail(error, QStringLiteral("配音文稿版本、结构无效或超过 16MiB。"));
    if (document.value(QStringLiteral("version")).toInt() == 2
        && (!document.value(QStringLiteral("defaultVoiceVersionId")).isString()
            || (!document.value(QStringLiteral("defaultVoiceVersionId")).toString().isEmpty()
                && QUuid(document.value(QStringLiteral("defaultVoiceVersionId")).toString()).isNull())))
        return fail(error, QStringLiteral("默认旁白音色版本无效。"));
    QSet<QString> paragraphs, ids;
    for (const auto &value : document.value(QStringLiteral("paragraphs")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("id")).toString();
        if (QUuid(id).isNull() || paragraphs.contains(id)) return fail(error, QStringLiteral("段落 ID 无效或重复。"));
        paragraphs.insert(id);
    }
    for (const auto &key : {QStringLiteral("segments"), QStringLiteral("archivedSegments")}) {
        const auto segments = document.value(key).toArray();
        if (segments.size() > 10000) return fail(error, QStringLiteral("文稿片段超过 10000 段，请拆分工程。"));
        for (const auto &value : segments) {
            const auto item = value.toObject(); const auto id = item.value(QStringLiteral("id")).toString();
            bool versionValid = false; const auto version = item.value(QStringLiteral("inputVersion")).toString().toLongLong(&versionValid);
            if (QUuid(id).isNull() || ids.contains(id) || !paragraphs.contains(item.value(QStringLiteral("paragraphId")).toString())
                || !item.value(QStringLiteral("text")).isString() || !item.value(QStringLiteral("spoken")).isString()
                || !item.value(QStringLiteral("manualSpoken")).isBool() || !item.value(QStringLiteral("notes")).isString()
                || !item.value(QStringLiteral("voiceVersionId")).isString()
                || !item.value(QStringLiteral("parameters")).isObject() || !versionValid || version < 0 || version >= std::numeric_limits<qint64>::max()
                || (!item.value(QStringLiteral("voiceVersionId")).toString().isEmpty() && QUuid(item.value(QStringLiteral("voiceVersionId")).toString()).isNull()))
                return fail(error, QStringLiteral("片段 ID、段落、声音引用或编辑字段无效。"));
            if (item.contains(QStringLiteral("locked")) && !item.value(QStringLiteral("locked")).isBool()) return fail(error, QStringLiteral("句子锁定状态必须为布尔值。"));
            ids.insert(id);
            for (const auto &field : {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")})
                if (!item.value(field).isString() || item.value(field).toString().isEmpty()) return fail(error, QStringLiteral("片段生成型号和语言字段无效。"));
            const auto parameters = item.value(QStringLiteral("parameters")).toObject();
            if (!validateTtsParameters(parameters, error)) return false;
            if (!item.value(QStringLiteral("selectedTake")).isObject()) return fail(error, QStringLiteral("选中 Take 结构无效。"));
            const auto take = item.value(QStringLiteral("selectedTake")).toObject();
            if (!take.isEmpty()) {
                bool validCount = false, validStart = false, validEnd = false;
                const auto count = take.value(QStringLiteral("sampleCount")).toString().toLongLong(&validCount);
                const auto start = take.value(QStringLiteral("trimStart")).toString().toLongLong(&validStart), end = take.value(QStringLiteral("trimEnd")).toString().toLongLong(&validEnd);
                if (QUuid(take.value(QStringLiteral("id")).toString()).isNull() || !validCount || !validStart || !validEnd || count <= 0 || start < 0 || end <= start || end > count
                    || take.value(QStringLiteral("sampleRate")).toInt() <= 0 || !take.value(QStringLiteral("manualCrop")).isBool()
                    || QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()).size() != 32)
                    return fail(error, QStringLiteral("选中 Take 或人工裁切范围无效。"));
            }
            for (const auto &number : {QStringLiteral("gain"), QStringLiteral("pauseMs")}) {
                if (!item.value(number).isDouble() || !std::isfinite(item.value(number).toDouble()) || item.value(number).toDouble() < 0)
                    return fail(error, QStringLiteral("编排增益或停顿无效。"));
            }
            if (item.value(QStringLiteral("gain")).toDouble() > 4 || item.value(QStringLiteral("pauseMs")).toDouble() > 60000)
                return fail(error, QStringLiteral("增益超过 4 倍或停顿超过 60 秒。"));
            if (!item.value(QStringLiteral("manualSpoken")).toBool() && item.value(QStringLiteral("spoken")).toString() != item.value(QStringLiteral("text")).toString())
                return fail(error, QStringLiteral("未手工修正的朗读文本必须跟随正文。"));
        }
    }
    for (const auto &value : document.value(QStringLiteral("replacements")).toArray()) {
        const auto item = value.toObject();
        if (!item.value(QStringLiteral("from")).isString() || item.value(QStringLiteral("from")).toString().isEmpty() || !item.value(QStringLiteral("to")).isString())
            return fail(error, QStringLiteral("朗读替换表必须提供非空原词与替换文本。"));
    }
    return true;
}
QStringList VoiceDocument::sentences(const QString &text)
{
    QStringList result; QString current;
    for (const auto character : text) {
        current += character;
        if (QStringLiteral("。！？!?；;\n").contains(character)) {
            if (!current.trimmed().isEmpty()) result.append(current.trimmed()); current.clear();
        }
    }
    if (!current.trimmed().isEmpty()) result.append(current.trimmed());
    return result;
}
QString VoiceDocument::normalizeChinese(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("(\\d{4})[-/](\\d{1,2})[-/](\\d{1,2})|(\\d+(?:\\.\\d+)?)\\s*%|(\\d{4})(?=年)|(\\d+(?:\\.\\d+)?)(kg|km|cm|mm|℃)?"));
    auto result = text; auto matches = pattern.globalMatch(text); qsizetype offset = 0;
    const auto year = [](const QString &value) { QString result; const auto digits = QStringLiteral("零一二三四五六七八九"); for (const auto c : value) result += digits[c.digitValue()]; return result; };
    while (matches.hasNext()) {
        const auto match = matches.next(); QString replacement;
        if (!match.captured(1).isEmpty()) {
            if (!QDate(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()).isValid()) continue;
            replacement = year(match.captured(1)) + QStringLiteral("年") + readNumber(QString::number(match.captured(2).toInt())) + QStringLiteral("月") + readNumber(QString::number(match.captured(3).toInt())) + QStringLiteral("日");
        } else if (!match.captured(4).isEmpty()) replacement = QStringLiteral("百分之") + readNumber(match.captured(4));
        else if (!match.captured(5).isEmpty()) replacement = year(match.captured(5));
        else {
            replacement = readNumber(match.captured(6));
            const QHash<QString, QString> units{{QStringLiteral("kg"), QStringLiteral("千克")}, {QStringLiteral("km"), QStringLiteral("千米")}, {QStringLiteral("cm"), QStringLiteral("厘米")}, {QStringLiteral("mm"), QStringLiteral("毫米")}, {QStringLiteral("℃"), QStringLiteral("摄氏度")}};
            replacement += units.value(match.captured(7));
        }
        result.replace(match.capturedStart() + offset, match.capturedLength(), replacement); offset += replacement.size() - match.capturedLength();
    }
    return result;
}
QJsonObject VoiceDocument::segment(const QString &paragraphId, const QString &text)
{
    return {{QStringLiteral("id"), uuid()}, {QStringLiteral("paragraphId"), paragraphId}, {QStringLiteral("text"), text},
        {QStringLiteral("spoken"), text}, {QStringLiteral("manualSpoken"), false}, {QStringLiteral("notes"), QString()},
        {QStringLiteral("voiceVersionId"), QString()}, {QStringLiteral("inputVersion"), QStringLiteral("0")},
        {QStringLiteral("provider"), QStringLiteral("mock")}, {QStringLiteral("model"), QStringLiteral("mock-tone")},
        {QStringLiteral("modelVersion"), QStringLiteral("protocol-v1")}, {QStringLiteral("language"), QStringLiteral("Chinese")},
        {QStringLiteral("parameters"), QJsonObject{}}, {QStringLiteral("gain"), 1.0}, {QStringLiteral("pauseMs"), 0}, {QStringLiteral("selectedTake"), QJsonObject{}}};
}
QString VoiceDocument::spokenText(const QJsonObject &document, const QJsonObject &item)
{
    QString text = item.value(QStringLiteral("spoken")).toString();
    if (!item.value(QStringLiteral("manualSpoken")).toBool()) {
        for (const auto &value : document.value(QStringLiteral("replacements")).toArray()) {
            const auto rule = value.toObject(); const auto from = rule.value(QStringLiteral("from")).toString();
            if (!from.isEmpty()) text.replace(from, rule.value(QStringLiteral("to")).toString());
        }
    }
    return text;
}
QByteArray VoiceDocument::inputKey(const QJsonObject &document, const QJsonObject &item)
{
    const QJsonObject input{{QStringLiteral("spokenText"), spokenText(document, item)},
        {QStringLiteral("provider"), item.value(QStringLiteral("provider"))}, {QStringLiteral("model"), item.value(QStringLiteral("model"))},
        {QStringLiteral("modelVersion"), item.value(QStringLiteral("modelVersion"))}, {QStringLiteral("language"), item.value(QStringLiteral("language"))},
        {QStringLiteral("voiceVersionId"), effectiveVoiceVersion(document, item)}, {QStringLiteral("parameters"), item.value(QStringLiteral("parameters"))}};
    return QCryptographicHash::hash(QJsonDocument(input).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
QString VoiceDocument::effectiveVoiceVersion(const QJsonObject &document, const QJsonObject &item)
{
    const auto voice = item.value(QStringLiteral("voiceVersionId")).toString();
    return voice.isEmpty() && !item.value(QStringLiteral("locked")).toBool() ? document.value(QStringLiteral("defaultVoiceVersionId")).toString() : voice;
}
QJsonObject VoiceDocument::setDefaultVoice(QJsonObject document, const QString &versionId)
{
    if (!versionId.isEmpty() && QUuid(versionId).isNull()) return document;
    if (document.value(QStringLiteral("defaultVoiceVersionId")).toString() == versionId) return document;
    document.insert(QStringLiteral("version"), 2);
    document.insert(QStringLiteral("defaultVoiceVersionId"), versionId);
    auto items = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (item.value(QStringLiteral("locked")).toBool() || !item.value(QStringLiteral("voiceVersionId")).toString().isEmpty()) continue;
        item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1));
        items[i] = item;
    }
    document.insert(QStringLiteral("segments"), items);
    return document;
}
QJsonObject VoiceDocument::appendText(QJsonObject document, const QString &text)
{
    auto paragraphs = document.value(QStringLiteral("paragraphs")).toArray(); auto segments = document.value(QStringLiteral("segments")).toArray();
    for (const auto &body : text.split(u'\n', Qt::SkipEmptyParts)) {
        const auto lines = sentences(body); if (lines.isEmpty()) continue;
        const auto id = uuid(); paragraphs.append(QJsonObject{{QStringLiteral("id"), id}});
        for (int i = 0; i < lines.size(); ++i) {
            auto item = segment(id, lines[i]);
            const auto voice = effectiveVoiceVersion(document, item);
            if (!voice.isEmpty()) for (int previous = segments.size(); previous-- > 0;) {
                const auto source = segments[previous].toObject();
                if (effectiveVoiceVersion(document, source) != voice) continue;
                for (const auto &field : {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language"), QStringLiteral("parameters")}) item.insert(field, source.value(field));
                break;
            }
            if (i == lines.size() - 1) item.insert(QStringLiteral("pauseMs"), 300); segments.append(item);
        }
    }
    document.insert(QStringLiteral("paragraphs"), paragraphs); document.insert(QStringLiteral("segments"), segments); return document;
}
QJsonObject VoiceDocument::appendStructured(QJsonObject document, const QJsonObject &incoming, QString *error)
{
    if ((incoming.value(QStringLiteral("version")).toInt(-1) != 1 && incoming.value(QStringLiteral("version")).toInt(-1) != 2) || !incoming.value(QStringLiteral("segments")).isArray()) {
        fail(error, QStringLiteral("结构化新增导入需要 v1 segments JSON。")); return document;
    }
    const auto origin = incoming.value(QStringLiteral("originProjectId")).toString();
    if (!origin.isEmpty() && QUuid(origin).isNull()) { fail(error, QStringLiteral("JSON 来源项目标识无效。")); return document; }
    auto paragraphs = document.value(QStringLiteral("paragraphs")).toArray(), segments = document.value(QStringLiteral("segments")).toArray();
    QHash<QString, QString> mapping; QSet<QString> seen;
    for (const auto &value : incoming.value(QStringLiteral("segments")).toArray()) {
        const auto source = value.toObject(); const auto sourceId = source.value(QStringLiteral("id")).toString(), parentId = source.value(QStringLiteral("paragraphId")).toString();
        if (!source.value(QStringLiteral("text")).isString() || (!sourceId.isEmpty() && (QUuid(sourceId).isNull() || seen.contains(sourceId)))
            || (!parentId.isEmpty() && QUuid(parentId).isNull())) { fail(error, QStringLiteral("新增 JSON 的正文、来源 ID 或段落 ID 无效；不能猜配原句。")); return document; }
        if (!sourceId.isEmpty()) seen.insert(sourceId);
        if (!mapping.contains(parentId)) { const auto id = uuid(); mapping.insert(parentId, id); paragraphs.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("originParagraphId"), parentId}}); }
        auto item = segment(mapping.value(parentId), source.value(QStringLiteral("text")).toString());
        for (const auto &key : {QStringLiteral("notes"), QStringLiteral("parameters"), QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language"), QStringLiteral("gain"), QStringLiteral("pauseMs")}) if (source.contains(key)) item.insert(key, source.value(key));
        if (source.value(QStringLiteral("manualSpoken")).toBool()) {
            if (!source.value(QStringLiteral("spoken")).isString()) { fail(error, QStringLiteral("手工朗读标记需要明确朗读文本。")); return document; }
            item.insert(QStringLiteral("manualSpoken"), true); item.insert(QStringLiteral("spoken"), source.value(QStringLiteral("spoken")));
        }
        const auto voice = source.value(QStringLiteral("voiceVersionId")).toString();
        if (!voice.isEmpty() && QUuid(voice).isNull()) { fail(error, QStringLiteral("导入声音版本必须是稳定 UUID。")); return document; }
        item.insert(QStringLiteral("requestedVoiceVersionId"), voice);
        item.insert(QStringLiteral("importOrigin"), QJsonObject{{QStringLiteral("projectId"), origin}, {QStringLiteral("segmentId"), sourceId}});
        segments.append(item);
    }
    auto result = document; result.insert(QStringLiteral("paragraphs"), paragraphs); result.insert(QStringLiteral("segments"), segments);
    return validate(result, error) ? result : document;
}
QJsonObject VoiceDocument::edit(QJsonObject document, const QString &id, const QString &text, const QString &spoken, bool manual, const QString &notes)
{
    auto segments = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < segments.size(); ++i) {
        const auto old = segments[i].toObject(); if (old.value(QStringLiteral("id")).toString() != id) continue;
        auto item = old; item.insert(QStringLiteral("text"), text); item.insert(QStringLiteral("spoken"), manual ? spoken : text);
        item.insert(QStringLiteral("manualSpoken"), manual); item.insert(QStringLiteral("notes"), notes); revise(document, old, item); segments[i] = item;
    }
    document.insert(QStringLiteral("segments"), segments); return document;
}
QJsonObject VoiceDocument::split(QJsonObject document, const QString &id, int offset, QString *error)
{
    auto segments = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < segments.size(); ++i) {
        const auto old = segments[i].toObject(); if (old.value(QStringLiteral("id")).toString() != id) continue;
        const auto text = old.value(QStringLiteral("text")).toString();
        if (offset <= 0 || offset >= text.size() || (text[offset].isLowSurrogate() && text[offset - 1].isHighSurrogate())) {
            fail(error, QStringLiteral("拆分位置必须位于正文内部且不能拆开字符。")); return document;
        }
        archive(document, old); segments.removeAt(i);
        auto first = segment(old.value(QStringLiteral("paragraphId")).toString(), text.left(offset));
        auto second = segment(old.value(QStringLiteral("paragraphId")).toString(), text.mid(offset));
        for (auto *item : {&first, &second}) {
            item->insert(QStringLiteral("voiceVersionId"), old.value(QStringLiteral("voiceVersionId"))); item->insert(QStringLiteral("parameters"), old.value(QStringLiteral("parameters")));
            for (const auto &key : {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")}) item->insert(key, old.value(key));
            item->insert(QStringLiteral("parentIds"), QJsonArray{id});
        }
        second.insert(QStringLiteral("pauseMs"), old.value(QStringLiteral("pauseMs")));
        segments.insert(i, second); segments.insert(i, first); document.insert(QStringLiteral("segments"), segments); return document;
    }
    fail(error, QStringLiteral("拆分片段已不存在。")); return document;
}
QJsonObject VoiceDocument::merge(QJsonObject document, const QString &id, QString *error)
{
    auto segments = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i + 1 < segments.size(); ++i) {
        const auto first = segments[i].toObject(), second = segments[i + 1].toObject();
        if (first.value(QStringLiteral("id")).toString() != id) continue;
        if (first.value(QStringLiteral("paragraphId")) != second.value(QStringLiteral("paragraphId"))) break;
        archive(document, first); archive(document, second);
        auto item = segment(first.value(QStringLiteral("paragraphId")).toString(), first.value(QStringLiteral("text")).toString() + second.value(QStringLiteral("text")).toString());
        const bool manual = first.value(QStringLiteral("manualSpoken")).toBool() || second.value(QStringLiteral("manualSpoken")).toBool();
        item.insert(QStringLiteral("manualSpoken"), manual);
        if (manual) item.insert(QStringLiteral("spoken"), spokenText(document, first) + spokenText(document, second));
        item.insert(QStringLiteral("voiceVersionId"), first.value(QStringLiteral("voiceVersionId"))); item.insert(QStringLiteral("parameters"), first.value(QStringLiteral("parameters")));
        for (const auto &key : {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")}) item.insert(key, first.value(key));
        item.insert(QStringLiteral("notes"), first.value(QStringLiteral("notes")).toString() + u'\n' + second.value(QStringLiteral("notes")).toString());
        item.insert(QStringLiteral("parentIds"), QJsonArray{first.value(QStringLiteral("id")), second.value(QStringLiteral("id"))}); item.insert(QStringLiteral("pauseMs"), second.value(QStringLiteral("pauseMs")));
        segments.removeAt(i + 1); segments[i] = item; document.insert(QStringLiteral("segments"), segments); return document;
    }
    fail(error, QStringLiteral("只能合并同一段落内相邻的两句。")); return document;
}
QJsonObject VoiceDocument::remove(QJsonObject document, const QString &id)
{
    auto segments = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < segments.size(); ++i) if (segments[i].toObject().value(QStringLiteral("id")).toString() == id) { archive(document, segments[i].toObject()); segments.removeAt(i); break; }
    document.insert(QStringLiteral("segments"), segments); return document;
}
QJsonObject VoiceDocument::resegment(QJsonObject document, const QString &paragraphId, const QString &text)
{
    auto segments = document.value(QStringLiteral("segments")).toArray(); int position = -1;
    for (int i = 0; i < segments.size();) {
        if (segments[i].toObject().value(QStringLiteral("paragraphId")).toString() == paragraphId) {
            if (position < 0) position = i; archive(document, segments[i].toObject()); segments.removeAt(i);
        } else ++i;
    }
    if (position < 0) return document;
    const auto lines = sentences(text);
    for (int i = 0; i < lines.size(); ++i) { auto item = segment(paragraphId, lines[i]); if (i == lines.size() - 1) item.insert(QStringLiteral("pauseMs"), 300); segments.insert(position + i, item); }
    document.insert(QStringLiteral("segments"), segments); return document;
}
QJsonObject VoiceDocument::targetedUpdate(QJsonObject document, const QJsonObject &incoming, const QString &projectId, QJsonArray *differences, QString *error)
{
    if ((incoming.value(QStringLiteral("version")).toInt(-1) != 1 && incoming.value(QStringLiteral("version")).toInt(-1) != 2) || incoming.value(QStringLiteral("originProjectId")).toString() != projectId
        || QUuid(projectId).isNull() || !incoming.value(QStringLiteral("segments")).isArray()) {
        fail(error, QStringLiteral("定向更新必须是本项目来源的 v1 JSON，不能按名称或行号覆盖。")); return document;
    }
    auto segments = document.value(QStringLiteral("segments")).toArray(); QSet<QString> seen;
    for (const auto &value : incoming.value(QStringLiteral("segments")).toArray()) {
        const auto update = value.toObject(); const auto id = update.value(QStringLiteral("id")).toString(); int index = -1;
        if (QUuid(id).isNull() || seen.contains(id)) { fail(error, QStringLiteral("导入 ID 无效或重复。")); return document; } seen.insert(id);
        for (int i = 0; i < segments.size(); ++i) if (segments[i].toObject().value(QStringLiteral("id")).toString() == id) { index = i; break; }
        if (index < 0) { fail(error, QStringLiteral("定向更新出现陌生或已删除 ID；请使用明确的新增导入。")); return document; }
        const auto old = segments[index].toObject(); auto item = old;
        for (const auto &key : {QStringLiteral("text"), QStringLiteral("notes"), QStringLiteral("voiceVersionId"), QStringLiteral("parameters"), QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")}) if (update.contains(key)) item.insert(key, update.value(key));
        if ((update.contains(QStringLiteral("manualSpoken")) && !update.value(QStringLiteral("manualSpoken")).isBool())
            || (update.contains(QStringLiteral("spoken")) && !update.value(QStringLiteral("spoken")).isString())) {
            fail(error, QStringLiteral("JSON 朗读修正字段类型无效。")); return document;
        }
        if (!old.value(QStringLiteral("manualSpoken")).toBool()) {
            if (update.value(QStringLiteral("manualSpoken")).toBool()) {
                if (!update.value(QStringLiteral("spoken")).isString()) { fail(error, QStringLiteral("JSON 手工朗读需要明确文本。")); return document; }
                item.insert(QStringLiteral("manualSpoken"), true); item.insert(QStringLiteral("spoken"), update.value(QStringLiteral("spoken")));
            } else item.insert(QStringLiteral("spoken"), item.value(QStringLiteral("text")));
        }
        revise(document, old, item); segments[index] = item;
        if (differences && item != old) differences->append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("before"), old}, {QStringLiteral("after"), item},
            {QStringLiteral("manualSpokenProtected"), old.value(QStringLiteral("manualSpoken")).toBool()}});
    }
    auto result = document; result.insert(QStringLiteral("segments"), segments);
    return validate(result, error) ? result : document;
}
QJsonObject VoiceDocument::applyTake(QJsonObject document, const QString &id, const QJsonObject &take, const QString &cropPolicy, QString *error)
{
    auto segments = document.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < segments.size(); ++i) {
        auto item = segments[i].toObject(); if (item.value(QStringLiteral("id")).toString() != id) continue;
        const auto selected = item.value(QStringLiteral("selectedTake")).toObject();
        bool valid = false; const auto count = take.value(QStringLiteral("sampleCount")).toString().toLongLong(&valid);
        if (!valid || count <= 0 || take.value(QStringLiteral("sampleRate")).toInt() <= 0 || QUuid(take.value(QStringLiteral("id")).toString()).isNull()
            || QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()) != inputKey(document, item)) {
            fail(error, QStringLiteral("候选输入已过期或音频规格无效，未应用。")); return document;
        }
        auto replacement = take;
        replacement.remove(QStringLiteral("path")); // 音频位置只由 Take 资产引用解析，搬迁工程不依赖旧绝对路径。
        replacement.insert(QStringLiteral("trimStart"), QStringLiteral("0")); replacement.insert(QStringLiteral("trimEnd"), QString::number(count));
        replacement.insert(QStringLiteral("manualCrop"), false);
        if (selected.value(QStringLiteral("manualCrop")).toBool()) {
            if (cropPolicy != QStringLiteral("keep") && cropPolicy != QStringLiteral("reset")) { fail(error, QStringLiteral("已有人工裁切，请比较后选择保留时间或重置。")); return document; }
            if (cropPolicy == QStringLiteral("keep")) {
                const int previousRate = selected.value(QStringLiteral("sampleRate")).toInt();
                if (previousRate <= 0) { fail(error, QStringLiteral("旧裁切采样率无效。")); return document; }
                const auto start = qRound64(selected.value(QStringLiteral("trimStart")).toString().toDouble() * take.value(QStringLiteral("sampleRate")).toInt() / previousRate);
                const auto end = qRound64(selected.value(QStringLiteral("trimEnd")).toString().toDouble() * take.value(QStringLiteral("sampleRate")).toInt() / previousRate);
                if (start < 0 || end <= start || end > count) { fail(error, QStringLiteral("保留裁切超出新音频范围，未静默截断。")); return document; }
                replacement.insert(QStringLiteral("trimStart"), QString::number(start)); replacement.insert(QStringLiteral("trimEnd"), QString::number(end)); replacement.insert(QStringLiteral("manualCrop"), true);
            }
        }
        item.insert(QStringLiteral("selectedTake"), replacement); segments[i] = item; document.insert(QStringLiteral("segments"), segments); return document;
    }
    fail(error, QStringLiteral("片段已删除，候选保留在历史。")); return document;
}
}
