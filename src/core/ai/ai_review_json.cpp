#include "ai/ai_review_json.h"

#include "ai/ai_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <utility>

namespace subcue {
namespace {

QByteArray lastSseJson(const QByteArray &body)
{
    QByteArray last;
    const QList<QByteArray> lines = body.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.startsWith("data:")) {
            line = line.mid(5).trimmed();
            if (line != "[DONE]") last = line;
        }
    }
    return last;
}

std::optional<double> readConfidence(const QJsonObject &object)
{
    if (!object.contains(QStringLiteral("confidence"))) return std::nullopt;
    const QJsonValue value = object.value(QStringLiteral("confidence"));
    if (!value.isDouble() && !value.isString()) return std::nullopt;
    bool ok = true;
    const double confidence = value.isDouble() ? value.toDouble() : value.toString().toDouble(&ok);
    if (!ok || !omniConfidenceValid(confidence)) return std::nullopt;
    return confidence;
}

} // namespace

AppError OmniReviewJson::invalidResponse(QString details)
{
    return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidResponse),
        QStringLiteral("AI Review 返回无效结果"), std::move(details));
}

std::optional<QJsonObject> OmniReviewJson::objectFromSseOrJson(const QByteArray &body)
{
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        const QByteArray sse = lastSseJson(body);
        if (sse.isEmpty()) return std::nullopt;
        document = QJsonDocument::fromJson(sse, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    }
    return document.object();
}

std::optional<QJsonObject> OmniReviewJson::structuredObject(const QJsonObject &message)
{
    const QJsonArray toolCalls = message.value(QStringLiteral("tool_calls")).toArray();
    if (!toolCalls.isEmpty()) {
        const QJsonObject function = toolCalls.at(0).toObject().value(QStringLiteral("function")).toObject();
        const QByteArray arguments = function.value(QStringLiteral("arguments")).toString().toUtf8();
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(arguments, &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) return document.object();
        return std::nullopt;
    }
    QString content = message.value(QStringLiteral("content")).toString().trimmed();
    if (content.startsWith(QLatin1String("```"))) {
        const int start = content.indexOf(QLatin1Char('{'));
        const int end = content.lastIndexOf(QLatin1Char('}'));
        if (start >= 0 && end > start) content = content.mid(start, end - start + 1);
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) return document.object();
    return std::nullopt;
}

OmniUsage OmniReviewJson::usageFromObject(const QJsonObject &usage)
{
    auto readCount = [](const QJsonObject &object, const QString &key) -> qint64 {
        const QJsonValue value = object.value(key);
        if (value.isUndefined() || value.isNull()) return -1;
        const qint64 count = value.toInteger(-1);
        return count >= 0 ? count : -1;
    };
    OmniUsage result;
    result.promptTokens = readCount(usage, QStringLiteral("prompt_tokens"));
    if (result.promptTokens < 0) result.promptTokens = readCount(usage, QStringLiteral("input_tokens"));
    result.completionTokens = readCount(usage, QStringLiteral("completion_tokens"));
    if (result.completionTokens < 0)
        result.completionTokens = readCount(usage, QStringLiteral("output_tokens"));
    result.totalTokens = readCount(usage, QStringLiteral("total_tokens"));
    const QJsonObject details = usage.value(QStringLiteral("prompt_tokens_details")).toObject();
    if (!details.isEmpty()) {
        result.promptTextTokens = readCount(details, QStringLiteral("text_tokens"));
        result.promptAudioTokens = readCount(details, QStringLiteral("audio_tokens"));
    }
    return result;
}

SubtitleOmniResult OmniReviewJson::parseSubtitleSuggestions(const QJsonObject &root)
{
    QJsonArray items = root.value(QStringLiteral("results")).toArray();
    if (items.isEmpty()) items = root.value(QStringLiteral("suggestions")).toArray();
    if (items.isEmpty() && root.contains(QStringLiteral("segment_id"))) items.append(root);
    QVector<SubtitleOmniSuggestion> suggestions;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        SubtitleOmniSuggestion suggestion;
        suggestion.segmentId = item.value(QStringLiteral("segment_id")).toString();
        if (suggestion.segmentId.isEmpty()) {
            return invalidResponse(QStringLiteral("缺少 segment_id"));
        }
        const auto issue = subtitleIssueFromName(item.value(QStringLiteral("issue_type")).toString());
        const auto decision = subtitleOmniDecisionFromName(item.value(QStringLiteral("decision")).toString());
        const auto confidence = readConfidence(item);
        if (!issue || !decision || !confidence) {
            return invalidResponse(QStringLiteral("字幕复核字段无效"));
        }
        suggestion.issueType = *issue;
        suggestion.decision = *decision;
        suggestion.confidence = *confidence;
        suggestion.originalText = item.value(QStringLiteral("original_text")).toString();
        suggestion.suggestedText = item.value(QStringLiteral("suggested_text")).toString();
        suggestion.reason = item.value(QStringLiteral("reason")).toString();
        suggestions.append(std::move(suggestion));
    }
    return suggestions;
}

std::variant<QVector<QJsonObject>, AppError> OmniReviewJson::parseWordMappings(const QJsonObject &root)
{
    QJsonArray items = root.value(QStringLiteral("mappings")).toArray();
    if (items.isEmpty()) items = root.value(QStringLiteral("results")).toArray();
    if (items.isEmpty() && root.contains(QStringLiteral("line_id"))) items.append(root);
    QVector<QJsonObject> mappings;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("line_id")).toString().trimmed().isEmpty()) {
            return invalidResponse(QStringLiteral("缺少 line_id"));
        }
        mappings.append(item);
    }
    return mappings;
}

RoughCutOmniResult OmniReviewJson::parseRoughCutSuggestions(const QJsonObject &root)
{
    QJsonArray items = root.value(QStringLiteral("results")).toArray();
    if (items.isEmpty()) items = root.value(QStringLiteral("suggestions")).toArray();
    if (items.isEmpty() && root.contains(QStringLiteral("candidate_id"))) items.append(root);
    QVector<RoughCutOmniSuggestion> suggestions;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        RoughCutOmniSuggestion suggestion;
        suggestion.candidateId = item.value(QStringLiteral("candidate_id")).toString();
        if (suggestion.candidateId.isEmpty()) {
            return invalidResponse(QStringLiteral("缺少 candidate_id"));
        }
        const auto decision = roughCutDecisionFromName(item.value(QStringLiteral("decision")).toString());
        const auto reasonType = roughCutOmniReasonFromName(item.value(QStringLiteral("reason_type")).toString());
        const auto confidence = readConfidence(item);
        if (!decision || !reasonType || !confidence) {
            return invalidResponse(QStringLiteral("粗剪复核字段无效"));
        }
        suggestion.decision = *decision;
        suggestion.reasonType = *reasonType;
        suggestion.confidence = *confidence;
        suggestion.replacementCandidateId =
            item.value(QStringLiteral("replacement_candidate_id")).toString();
        suggestion.reason = item.value(QStringLiteral("reason")).toString();
        suggestions.append(std::move(suggestion));
    }
    return suggestions;
}

} // namespace subcue
