#include "ai/openai_compatible_provider.h"

#include "ai/ai_types.h"
#include "alignment/alignment_engine.h"
#include "alignment/normalizer.h"
#include "alignment/python_round.h"
#include "common/logging.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrlQuery>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <algorithm>
#include <utility>

namespace subcue {
namespace {

[[nodiscard]] AppError aiHttpError(int status)
{
    return AppError(ErrorDomain::Ai, status,
        QStringLiteral("AI 复核请求失败（HTTP %1）").arg(status));
}

[[nodiscard]] AppError aiInvalidResponse(QString details = {})
{
    return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidResponse),
        QStringLiteral("AI 复核返回无效结果"), std::move(details));
}

[[nodiscard]] int pythonIndexOf(int targetIndex) noexcept
{
    return targetIndex + 1;
}

[[nodiscard]] QString wordToken(qint64 wordId)
{
    if (wordId <= 0) {
        return QStringLiteral("null");
    }
    return QStringLiteral("W%1").arg(wordId);
}

[[nodiscard]] std::optional<qint64> parseWordId(const QJsonValue &value)
{
    const QString text = value.isString()
        ? value.toString()
        : (value.isDouble() ? QString() : value.toVariant().toString());
    static const QRegularExpression pattern(QStringLiteral(R"(^W(\d+)$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const qint64 id = match.captured(1).toLongLong(&ok);
    if (!ok || id < 1) {
        return std::nullopt;
    }
    return id;
}

[[nodiscard]] QJsonObject mappingItemSchema()
{
    QJsonObject properties;
    properties.insert(QStringLiteral("line_id"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}});
    properties.insert(QStringLiteral("start_word_id"),
        QJsonObject{{QStringLiteral("type"), QJsonArray{
            QStringLiteral("string"), QStringLiteral("null")}}});
    properties.insert(QStringLiteral("end_word_id"),
        QJsonObject{{QStringLiteral("type"), QJsonArray{
            QStringLiteral("string"), QStringLiteral("null")}}});
    properties.insert(QStringLiteral("confidence"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}});
    QJsonObject status;
    status.insert(QStringLiteral("type"), QStringLiteral("string"));
    status.insert(QStringLiteral("enum"), QJsonArray{
        QStringLiteral("matched"),
        QStringLiteral("not_present"),
        QStringLiteral("uncertain"),
    });
    properties.insert(QStringLiteral("status"), status);

    QJsonObject item;
    item.insert(QStringLiteral("type"), QStringLiteral("object"));
    item.insert(QStringLiteral("properties"), properties);
    item.insert(QStringLiteral("required"), QJsonArray{
        QStringLiteral("line_id"),
        QStringLiteral("start_word_id"),
        QStringLiteral("end_word_id"),
        QStringLiteral("confidence"),
        QStringLiteral("status"),
    });
    item.insert(QStringLiteral("additionalProperties"), false);
    return item;
}

void markNotPresent(Subtitle &target, const QString &reason)
{
    target.status = QStringLiteral("SKIPPED_NO_AUDIO");
    target.start = MediaTime{};
    target.end = MediaTime{};
    target.skipReason = reason;
}

QString normalizedApiBase(QUrl url)
{
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (!path.endsWith(QLatin1String("/v1"))) {
        path += QStringLiteral("/v1");
    }
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

} // namespace

OpenAICompatibleProvider::OpenAICompatibleProvider(
    QString apiKey,
    QString model,
    QString region,
    IHttpClient *http)
    : apiKey_(std::move(apiKey)),
      model_(std::move(model)),
      endpoint_(aiEndpointForRegion(region)),
      modelsEndpoint_(region == QLatin1String("singapore")
            ? QStringLiteral("https://dashscope-intl.aliyuncs.com/compatible-mode/v1/models")
            : QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1/models"))
{
    if (http) {
        http_ = http;
    } else {
        ownedHttp_ = std::make_unique<QtNetworkHttpClient>();
        http_ = ownedHttp_.get();
    }
}

OpenAICompatibleProvider::OpenAICompatibleProvider(
    QString apiKey,
    QString model,
    QUrl baseUrl,
    bool useBearer,
    IHttpClient *http)
    : OpenAICompatibleProvider(std::move(apiKey), std::move(model), std::move(baseUrl),
                               useBearer, QUrl{}, http)
{
}

OpenAICompatibleProvider::OpenAICompatibleProvider(
    QString apiKey,
    QString model,
    QUrl baseUrl,
    bool useBearer,
    QUrl modelsUrl,
    IHttpClient *http)
    : apiKey_(std::move(apiKey)),
      model_(std::move(model)),
      useBearer_(useBearer)
{
    const QString base = normalizedApiBase(std::move(baseUrl));
    endpoint_ = base + QStringLiteral("/chat/completions");
    modelsEndpoint_ = modelsUrl.isEmpty()
        ? base + QStringLiteral("/models")
        : modelsUrl.toString(QUrl::FullyEncoded);
    if (http) {
        http_ = http;
    } else {
        ownedHttp_ = std::make_unique<QtNetworkHttpClient>();
        http_ = ownedHttp_.get();
    }
}

QUrl OpenAICompatibleProvider::qwenModelsEndpoint(QUrl baseUrl)
{
    QString path = baseUrl.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.endsWith(QLatin1String("/compatible-mode/v1"))) {
        path.chop(QStringLiteral("/compatible-mode/v1").size());
    } else if (path.endsWith(QLatin1String("/v1"))) {
        path.chop(3);
    }
    baseUrl.setPath(path + QStringLiteral("/api/v1/models"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("providers"), QStringLiteral("qwen"));
    query.addQueryItem(QStringLiteral("capabilities"), QStringLiteral("TG"));
    query.addQueryItem(QStringLiteral("page_size"), QStringLiteral("100"));
    baseUrl.setQuery(query);
    baseUrl.setFragment({});
    return baseUrl;
}

QString OpenAICompatibleProvider::providerId() const
{
    return QString::fromLatin1(kAiProviderOpenAiCompatible);
}

ModelListResult OpenAICompatibleProvider::listModels(const std::atomic<bool> *cancel)
{
    if (aiCancelled(cancel)) {
        return aiCancelledError();
    }
    HttpRequest request;
    request.url = QUrl(modelsEndpoint_);
    if (useBearer_) {
        request.headers.push_back({QByteArrayLiteral("Authorization"),
                                   QByteArrayLiteral("Bearer ") + apiKey_.toUtf8()});
    }
    request.connectTimeoutMs = kAiConnectTimeoutMs;
    request.transferTimeoutMs = kAiConnectTimeoutMs;
    std::variant<HttpResponse, AppError> sent = http_->send(request, cancel);
    if (std::holds_alternative<AppError>(sent)) {
        return std::get<AppError>(std::move(sent));
    }
    const HttpResponse response = std::get<HttpResponse>(std::move(sent));
    if (response.status >= 400) {
        return AppError(ErrorDomain::Ai, response.status,
            QStringLiteral("AI Provider 连接失败（HTTP %1）").arg(response.status));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidResponse),
            QStringLiteral("Provider 不支持模型发现，无法用于 SubCue"));
    }
    QVector<ModelDescriptor> models;
    for (const QJsonValue &value : document.object().value(QStringLiteral("data")).toArray()) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty()) {
            models.push_back({id, id, 0, true});
        }
    }
    for (const QJsonValue &value : document.object().value(QStringLiteral("output")).toObject()
             .value(QStringLiteral("models")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString id = item.value(QStringLiteral("model")).toString().trimmed();
        if (!id.isEmpty()) {
            models.push_back({id, item.value(QStringLiteral("name")).toString(id), 0, true});
        }
    }
    std::sort(models.begin(), models.end(), [](const ModelDescriptor &left, const ModelDescriptor &right) {
        return left.id.compare(right.id, Qt::CaseInsensitive) < 0;
    });
    if (models.isEmpty()) {
        return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidResponse),
            QStringLiteral("Provider 不支持模型发现，无法用于 SubCue"));
    }
    return models;
}

ProviderTestResult OpenAICompatibleProvider::testConnection(const std::atomic<bool> *cancel)
{
    ModelListResult result = listModels(cancel);
    if (std::holds_alternative<AppError>(result)) {
        return std::get<AppError>(std::move(result));
    }
    return ConnectionTestResult{std::get<QVector<ModelDescriptor>>(std::move(result)),
                                QDateTime::currentDateTimeUtc()};
}

QVector<AppError> OpenAICompatibleProvider::errors() const
{
    return errors_;
}

QString OpenAICompatibleProvider::systemPrompt()
{
    return QStringLiteral(
        "你是“字幕音文对齐纠错器”。你的唯一任务是根据用户提供的字幕文案和 ASR 识别词序列，判断每条字幕最可能对应哪一段连续的 ASR word_id。\n"
        "重要规则：\n"
        "1. 不得改写字幕文字。2. 不得生成新的字幕。3. 不得改变字幕顺序。\n"
        "5. 每条字幕只能匹配一个连续的 ASR word_id 区间。6. 匹配结果必须满足字幕顺序单调递增。\n"
        "7. 允许错别字、同音字、漏字、多字、标点差异和英文大小写差异。\n"
        "8. 音频是唯一时间依据，不得为音频中不存在的文案创造区间。\n"
        "9. 明确不存在时返回 not_present，证据不足时返回 uncertain，不得猜测。\n"
        "10. 不需要解释推理过程。11. 只返回指定 JSON 数据。");
}

QJsonObject OpenAICompatibleProvider::responseFormatSchema()
{
    QJsonObject mappings;
    mappings.insert(QStringLiteral("type"), QStringLiteral("array"));
    mappings.insert(QStringLiteral("items"), mappingItemSchema());

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"),
        QJsonObject{{QStringLiteral("mappings"), mappings}});
    schema.insert(QStringLiteral("required"), QJsonArray{QStringLiteral("mappings")});
    schema.insert(QStringLiteral("additionalProperties"), false);

    QJsonObject jsonSchema;
    jsonSchema.insert(QStringLiteral("name"), QStringLiteral("subtitle_alignment"));
    jsonSchema.insert(QStringLiteral("strict"), true);
    jsonSchema.insert(QStringLiteral("schema"), schema);

    QJsonObject format;
    format.insert(QStringLiteral("type"), QStringLiteral("json_schema"));
    format.insert(QStringLiteral("json_schema"), jsonSchema);
    return format;
}

QString OpenAICompatibleProvider::buildUserPrompt(
    const QVector<Subtitle> &subtitles,
    const QVector<TranscriptWord> &words,
    int targetIndex)
{
    const Subtitle &target = subtitles.at(targetIndex);
    const int pythonIndex = pythonIndexOf(targetIndex);
    const int lineStart = std::max(0, pythonIndex - kAiNearbyLineBefore);
    const int lineEnd = std::min(static_cast<int>(subtitles.size()),
        pythonIndex + kAiNearbyLineAfter);

    QStringList nearbyLines;
    nearbyLines.reserve(lineEnd - lineStart);
    for (int index = lineStart; index < lineEnd; ++index) {
        nearbyLines.push_back(
            QLatin1Char('L') + QString::number(index + 1) + QStringLiteral(": ")
                + subtitles.at(index).text);
    }

    qint64 center = target.startWordId;
    if (center <= 0) {
        const double denominator = static_cast<double>(std::max(1, static_cast<int>(subtitles.size())));
        center = std::max<qint64>(1, pythonRound(
            static_cast<double>(pythonIndex) / denominator * static_cast<double>(words.size())));
    }
    const qint64 wordStart = std::max<qint64>(1, center - kAiWordWindowRadius);
    const qint64 wordEnd = std::min<qint64>(words.size(), center + kAiWordWindowRadius);

    QStringList nearbyWords;
    for (const TranscriptWord &word : words) {
        if (word.id >= wordStart && word.id <= wordEnd) {
            nearbyWords.push_back(
                QLatin1Char('W') + QString::number(word.id) + QStringLiteral(": ") + word.text);
        }
    }

    const QString pythonIndexText = QString::number(pythonIndex);
    QString prompt;
    prompt += QStringLiteral("SCRIPT_LINES:\n");
    prompt += nearbyLines.join(QLatin1Char('\n'));
    prompt += QStringLiteral("\n\nASR_WORDS:\n");
    prompt += nearbyWords.join(QLatin1Char('\n'));
    prompt += QStringLiteral("\n\nLOCAL_CANDIDATE:\nline_id: L");
    prompt += pythonIndexText;
    prompt += QStringLiteral("\nlocal_start_word_id: ");
    prompt += wordToken(target.startWordId);
    prompt += QStringLiteral("\nlocal_end_word_id: ");
    prompt += wordToken(target.endWordId);
    prompt += QStringLiteral("\nlocal_similarity: ");
    prompt += QString::number(target.confidence, 'f', 2);
    prompt += QStringLiteral("\n\nTASK:\n请判断 L");
    prompt += pythonIndexText;
    prompt += QStringLiteral(
        " 对应哪个连续 ASR_WORDS 区间。只使用输入中已有的 line_id 和 word_id。"
        "音频中明确没有时 status = not_present，无法可靠判断时 status = uncertain。只输出 JSON。");
    return prompt;
}

QByteArray OpenAICompatibleProvider::buildPayload(
    const QString &model,
    const QString &userPrompt)
{
    QJsonObject systemMessage;
    systemMessage.insert(QStringLiteral("role"), QStringLiteral("system"));
    systemMessage.insert(QStringLiteral("content"), systemPrompt());
    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), userPrompt);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model);
    payload.insert(QStringLiteral("messages"), QJsonArray{systemMessage, userMessage});
    payload.insert(QStringLiteral("response_format"), responseFormatSchema());
    payload.insert(QStringLiteral("enable_thinking"), false);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

std::variant<std::optional<QJsonObject>, AppError> OpenAICompatibleProvider::mappingFromResponse(
    const QByteArray &body,
    int pythonIndex)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return aiInvalidResponse(parseError.errorString());
    }

    const QJsonArray choices = document.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return aiInvalidResponse(QStringLiteral("choices 为空"));
    }
    const QJsonObject message = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    const QJsonValue contentValue = message.value(QStringLiteral("content"));

    QJsonObject parsed;
    if (contentValue.isObject()) {
        parsed = contentValue.toObject();
    } else if (contentValue.isString()) {
        QJsonParseError contentError;
        const QJsonDocument contentDocument =
            QJsonDocument::fromJson(contentValue.toString().toUtf8(), &contentError);
        if (contentError.error != QJsonParseError::NoError || !contentDocument.isObject()) {
            return aiInvalidResponse(contentError.errorString());
        }
        parsed = contentDocument.object();
    } else {
        return aiInvalidResponse(QStringLiteral("message.content 无效"));
    }

    const QJsonArray mappings = parsed.value(QStringLiteral("mappings")).toArray();
    const QString expectedLine = QStringLiteral("L%1").arg(pythonIndex);
    for (const QJsonValue &item : mappings) {
        const QJsonObject mapping = item.toObject();
        if (mapping.value(QStringLiteral("line_id")).toString() == expectedLine) {
            return std::optional<QJsonObject>(mapping);
        }
    }
    return std::optional<QJsonObject>{};
}

bool OpenAICompatibleProvider::applyIfValid(
    Subtitle &target,
    int targetIndex,
    const QJsonObject &mapping,
    const QVector<Subtitle> &subtitles,
    const QVector<TranscriptWord> &words)
{
    const std::optional<qint64> startId = parseWordId(mapping.value(QStringLiteral("start_word_id")));
    const std::optional<qint64> endId = parseWordId(mapping.value(QStringLiteral("end_word_id")));
    if (!startId || !endId) {
        return false;
    }
    if (*startId < 1 || *endId < *startId || *endId > words.size()) {
        return false;
    }

    const Subtitle *previous = targetIndex > 0 ? &subtitles.at(targetIndex - 1) : nullptr;
    const Subtitle *following = targetIndex + 1 < subtitles.size()
        ? &subtitles.at(targetIndex + 1)
        : nullptr;
    if (previous && previous->endWordId > 0 && *startId <= previous->endWordId) {
        return false;
    }
    if (following && following->startWordId > 0 && *endId >= following->startWordId) {
        return false;
    }

    QString candidateText;
    for (qint64 index = *startId - 1; index < *endId; ++index) {
        candidateText += words.at(static_cast<qsizetype>(index)).text;
    }
    const QString targetText = AlignmentEngine::normalizeText(target.text);
    const QString normalizedCandidate = AlignmentEngine::normalizeText(candidateText);
    const double similarity =
        AlignmentEngine::fuzzRatio(targetText, normalizedCandidate) / 100.0;
    const qint64 targetLength = static_cast<qint64>(Normalizer::toCodepoints(targetText).size());
    const qint64 candidateLength =
        static_cast<qint64>(Normalizer::toCodepoints(normalizedCandidate).size());
    const qint64 minimum = std::min(targetLength, candidateLength);
    const qint64 maximum = std::max<qint64>(1, std::max(targetLength, candidateLength));
    const double lengthRatio = static_cast<double>(minimum) / static_cast<double>(maximum);
    const double localConfidence =
        AlignmentEngine::calculateConfidence(similarity, 0.05, lengthRatio);
    const double mappingConfidence = mapping.value(QStringLiteral("confidence")).toDouble(0.0);
    if (localConfidence < kAiApplyConfidenceFloor || mappingConfidence < kAiApplyConfidenceFloor) {
        return false;
    }

    target.startWordId = *startId;
    target.endWordId = *endId;
    target.start = MediaTime::fromMilliseconds(words.at(static_cast<qsizetype>(*startId - 1)).startMs);
    target.end = MediaTime::fromMilliseconds(words.at(static_cast<qsizetype>(*endId - 1)).endMs);
    target.candidateText = candidateText;
    target.confidence = std::min(localConfidence, std::max(0.0, std::min(1.0, mappingConfidence)));
    target.ambiguity = 1.0;
    target.source = QStringLiteral("llm");
    target.status = target.confidence >= kAiApplyConfidenceFloor
        ? QStringLiteral("MATCHED")
        : QStringLiteral("LOW_CONFIDENCE");
    target.skipReason.clear();
    return true;
}

std::variant<std::optional<QJsonObject>, AppError> OpenAICompatibleProvider::reviewOne(
    int targetIndex,
    const QVector<Subtitle> &subtitles,
    const QVector<TranscriptWord> &words,
    const std::atomic<bool> *cancel)
{
    if (aiCancelled(cancel)) {
        return aiCancelledError();
    }

    HttpRequest request;
    request.url = QUrl(endpoint_);
    request.method = QByteArrayLiteral("POST");
    request.headers = {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
    if (useBearer_) {
        request.headers.push_back({QByteArrayLiteral("Authorization"),
                                   QByteArrayLiteral("Bearer ") + apiKey_.toUtf8()});
    }
    request.body = buildPayload(model_, buildUserPrompt(subtitles, words, targetIndex));
    request.connectTimeoutMs = kAiConnectTimeoutMs;
    request.transferTimeoutMs = kAiTransferTimeoutMs;

    std::variant<HttpResponse, AppError> sent = http_->send(request, cancel);
    if (std::holds_alternative<AppError>(sent)) {
        AppError error = std::get<AppError>(std::move(sent));
        if (isAiCancelError(error)) {
            return aiCancelledError();
        }
        return error;
    }
    if (aiCancelled(cancel)) {
        return aiCancelledError();
    }
    const HttpResponse response = std::get<HttpResponse>(std::move(sent));
    if (response.status >= 400) {
        return aiHttpError(response.status);
    }
    return mappingFromResponse(response.body, pythonIndexOf(targetIndex));
}

AiReviewResult OpenAICompatibleProvider::review(
    QVector<Subtitle> &subtitles,
    const QVector<TranscriptWord> &words,
    const std::atomic<bool> *cancel,
    const AiProgress &progress)
{
    errors_.clear();
    QVector<int> targets;
    targets.reserve(subtitles.size());
    for (int index = 0; index < subtitles.size(); ++index) {
        if (AlignmentEngine::needsAiReview(
                subtitles.at(index).confidence, subtitles.at(index).ambiguity)) {
            targets.push_back(index);
        }
    }

    int calls = 0;
    qCInfo(subcueAiLog) << "ai review targets" << targets.size();
    for (int order = 0; order < targets.size(); ++order) {
        if (aiCancelled(cancel)) {
            return aiCancelledError();
        }
        const int targetIndex = targets.at(order);
        if (progress) {
            progress(order, targets.size());
        }
        ++calls;
        std::variant<std::optional<QJsonObject>, AppError> mapping =
            reviewOne(targetIndex, subtitles, words, cancel);
        if (!aiCancelled(cancel) && progress) progress(order + 1, targets.size());
        if (std::holds_alternative<AppError>(mapping)) {
            AppError error = std::get<AppError>(std::move(mapping));
            if (isAiCancelError(error)) {
                return aiCancelledError();
            }
            errors_.push_back(error);
            subtitles[targetIndex].metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("failed"));
            subtitles[targetIndex].status = QStringLiteral("LOW_CONFIDENCE");
            continue;
        }

        const std::optional<QJsonObject> item = std::get<std::optional<QJsonObject>>(std::move(mapping));
        Subtitle &target = subtitles[targetIndex];
        if (!item) {
            target.metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("invalid"));
            target.skipReason = QStringLiteral("AI 未返回有效复核结果");
            continue;
        }
        const QString status = item->value(QStringLiteral("status")).toString();
        target.metadata.insert(QStringLiteral("aiReviewStatus"), status);
        if (status == QLatin1String("matched")) {
            if (!applyIfValid(target, targetIndex, *item, subtitles, words)) {
                target.metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("rejectedLocally"));
                target.skipReason = QStringLiteral("AI 映射未通过本地音频校验");
            }
        } else if (status == QLatin1String("not_present")) {
            markNotPresent(target, QStringLiteral("AI 判定音频中不存在该片段"));
        } else {
            if (target.isTimed() && target.startWordId >= 0 && target.endWordId >= target.startWordId) {
                target.status = QStringLiteral("LOW_CONFIDENCE");
            } else {
                markNotPresent(target, QStringLiteral("音频证据不足"));
            }
        }
    }
    return calls;
}

} // namespace subcue
