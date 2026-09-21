#include "ai/qwen_omni_provider.h"

#include <cmath>

#include "ai/ai_review_json.h"
#include "ai/ai_types.h"
#include "ai/review_audio_encoder.h"
#include "common/logging.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <utility>

namespace subcue {
namespace {

QJsonObject functionTool(const QString &name, const QString &description, const QJsonObject &parameters)
{
    QJsonObject function;
    function.insert(QStringLiteral("name"), name);
    function.insert(QStringLiteral("description"), description);
    function.insert(QStringLiteral("parameters"), parameters);
    return {
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), function},
    };
}

QJsonObject stringEnum(const QStringList &values)
{
    return {
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"), QJsonArray::fromStringList(values)},
    };
}

QJsonArray userContent(const OmniChatRequest &request)
{
    QJsonArray content;
    if (!request.audio.data.isEmpty()) {
        const QString mime = request.audio.format == QLatin1String("wav")
            ? QStringLiteral("audio/wav")
            : (request.audio.format == QLatin1String("mp3")
                ? QStringLiteral("audio/mpeg")
                : QStringLiteral("audio/aac"));
        content.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("input_audio")},
            {QStringLiteral("input_audio"), QJsonObject{
                {QStringLiteral("data"),
                 QStringLiteral("data:%1;base64,%2")
                     .arg(mime, QString::fromLatin1(request.audio.data.toBase64()))},
                {QStringLiteral("format"), request.audio.format},
            }},
        });
    }
    content.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), request.userText},
    });
    return content;
}

} // namespace

QwenOmniProvider::QwenOmniProvider(
    QString apiKey,
    OmniReviewSettings settings,
    IHttpClient *http)
    : apiKey_(std::move(apiKey)),
      settings_(std::move(settings)),
      endpoint_(chatCompletionsUrl(settings_.baseUrl).toString(QUrl::FullyEncoded))
{
    if (http) {
        http_ = http;
    } else {
        ownedHttp_ = std::make_unique<QtNetworkHttpClient>();
        http_ = ownedHttp_.get();
    }
}

QString QwenOmniProvider::providerId() const
{
    return settings_.provider.isEmpty()
        ? QString::fromLatin1(kOmniReviewProviderId)
        : settings_.provider;
}

QString QwenOmniProvider::normalizeBaseUrl(QString url)
{
    QUrl parsed(url.trimmed());
    QString path = parsed.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (!path.endsWith(QLatin1String("/v1"))) path += QStringLiteral("/v1");
    parsed.setPath(path);
    parsed.setQuery(QString());
    parsed.setFragment(QString());
    return parsed.toString(QUrl::FullyEncoded);
}

QUrl QwenOmniProvider::chatCompletionsUrl(const QString &baseUrl)
{
    return QUrl(normalizeBaseUrl(baseUrl) + QStringLiteral("/chat/completions"));
}

QJsonArray QwenOmniProvider::subtitleToolSchema()
{
    QJsonObject itemProperties{
        {QStringLiteral("segment_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("issue_type"), stringEnum({
            QStringLiteral("MISSING_TEXT"), QStringLiteral("WRONG_TEXT"), QStringLiteral("DUPLICATE"),
            QStringLiteral("BAD_SPLIT"), QStringLiteral("BAD_MERGE"), QStringLiteral("PUNCTUATION"),
            QStringLiteral("ASR_UNCERTAIN"), QStringLiteral("SCRIPT_MISMATCH")})},
        {QStringLiteral("confidence"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("original_text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("suggested_text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("decision"), stringEnum({
            QStringLiteral("KEEP"), QStringLiteral("REVIEW"), QStringLiteral("REPLACE_TEXT")})},
    };
    QJsonObject item{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), itemProperties},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("segment_id"), QStringLiteral("issue_type"), QStringLiteral("confidence"),
            QStringLiteral("original_text"), QStringLiteral("suggested_text"),
            QStringLiteral("reason"), QStringLiteral("decision")}},
    };
    QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("results"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), item},
            }},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("results")}},
    };
    return {functionTool(QStringLiteral("submit_subtitle_review"),
        QStringLiteral("Submit subtitle semantic review results"), parameters)};
}

QJsonArray QwenOmniProvider::roughCutToolSchema()
{
    QJsonObject itemProperties{
        {QStringLiteral("candidate_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("decision"), stringEnum({
            QStringLiteral("KEEP"), QStringLiteral("REVIEW"), QStringLiteral("CUT")})},
        {QStringLiteral("reason_type"), stringEnum({
            QStringLiteral("RETAKE"), QStringLiteral("FALSE_START"), QStringLiteral("DUPLICATE"),
            QStringLiteral("STUMBLE"), QStringLiteral("WRONG_TAKE"), QStringLiteral("INTERRUPTION"),
            QStringLiteral("NOISE_TAKE"), QStringLiteral("UNCERTAIN")})},
        {QStringLiteral("confidence"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
        {QStringLiteral("replacement_candidate_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("reason"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
    };
    QJsonObject item{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), itemProperties},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("candidate_id"), QStringLiteral("decision"), QStringLiteral("reason_type"),
            QStringLiteral("confidence"), QStringLiteral("reason")}},
    };
    QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("results"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), item},
            }},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("results")}},
    };
    return {functionTool(QStringLiteral("submit_roughcut_review"),
        QStringLiteral("Submit rough-cut candidate review results"), parameters)};
}

QJsonArray QwenOmniProvider::wordMappingToolSchema()
{
    QJsonObject itemProperties{
        {QStringLiteral("line_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("status"), stringEnum({
            QStringLiteral("matched"), QStringLiteral("not_present"), QStringLiteral("uncertain")})},
        {QStringLiteral("start_word_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("end_word_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("confidence"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
    };
    QJsonObject item{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), itemProperties},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("line_id"), QStringLiteral("status"), QStringLiteral("confidence")}},
    };
    QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("mappings"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), item},
            }},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("mappings")}},
    };
    return {functionTool(QStringLiteral("submit_word_mapping"),
        QStringLiteral("Submit word-level subtitle timing mappings"), parameters)};
}

QJsonArray QwenOmniProvider::connectionTestToolSchema()
{
    QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("ok")}},
    };
    return {functionTool(QStringLiteral("submit_connection_test"),
        QStringLiteral("Confirm the probe audio and structured response"), parameters)};
}

OmniAudioClip QwenOmniProvider::builtInProbeClip()
{
    QVector<float> pcm(1'600);
    for (int index = 0; index < pcm.size(); ++index) {
        pcm[index] = 0.04f * std::sin(2.0 * 3.14159265358979323846 * 440.0 * index / 16'000.0);
    }
    OmniAudioClip clip;
    clip.data = ReviewAudioEncoder::encodeWav16kMono(pcm);
    clip.format = QStringLiteral("wav");
    clip.sampleRate = 16'000;
    clip.windowEndMs = 100;
    return clip;
}

QByteArray QwenOmniProvider::buildPayload(
    const OmniReviewSettings &settings,
    const OmniChatRequest &request)
{
    QJsonArray messages;
    if (!request.systemPrompt.isEmpty()) {
        messages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("content"), request.systemPrompt},
        });
    }
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), userContent(request)},
    });
    QJsonObject payload{
        {QStringLiteral("model"), settings.model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("modalities"), QJsonArray{QStringLiteral("text")}},
        {QStringLiteral("stream"), false},
    };
    const bool thinking = settings.reasoningEffort != OmniReasoningEffort::None;
    payload.insert(QStringLiteral("enable_thinking"), thinking);
    if (thinking) {
        payload.insert(QStringLiteral("reasoning_effort"), omniReasoningName(settings.reasoningEffort));
    }
    if (!request.tools.isEmpty()) {
        payload.insert(QStringLiteral("tools"), request.tools);
        if (!request.toolChoiceName.isEmpty()) {
            payload.insert(QStringLiteral("tool_choice"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), QJsonObject{
                    {QStringLiteral("name"), request.toolChoiceName},
                }},
            });
        }
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

AppError QwenOmniProvider::mapHttpError(int status, const QByteArray &body)
{
    QString code;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject error = document.object().value(QStringLiteral("error")).toObject();
        code = error.value(QStringLiteral("code")).toString();
        if (code.isEmpty()) code = document.object().value(QStringLiteral("code")).toString();
    }
    if (status == 401 || code.contains(QLatin1String("InvalidApiKey"), Qt::CaseInsensitive)
        || code.contains(QLatin1String("Unauthorized"), Qt::CaseInsensitive)) {
        return AppError(ErrorDomain::Ai, status, QStringLiteral("认证失败：API Key 无效或已过期"));
    }
    if (status == 403 || code.contains(QLatin1String("AccessDenied"), Qt::CaseInsensitive)
        || code.contains(QLatin1String("Unpurchased"), Qt::CaseInsensitive)) {
        return AppError(ErrorDomain::Ai, status,
            QStringLiteral("当前 API Key 无 qwen3.8-omni-flash 模型权限"));
    }
    if (status == 429 || code.contains(QLatin1String("Throttl"), Qt::CaseInsensitive)
        || code.contains(QLatin1String("RateQuota"), Qt::CaseInsensitive)) {
        return AppError(ErrorDomain::Ai, status, QStringLiteral("请求过于频繁，请稍后再试"));
    }
    if (status == 408 || status == 504) {
        return AppError(ErrorDomain::Ai, status, QStringLiteral("AI 请求超时"));
    }
    return AppError(ErrorDomain::Ai, status,
        QStringLiteral("AI 请求失败（HTTP %1）").arg(status));
}

OmniCompleteResult QwenOmniProvider::complete(
    const OmniChatRequest &request,
    const std::atomic<bool> *cancel)
{
    if (aiCancelled(cancel)) return aiCancelledError();
    if (apiKey_.trimmed().isEmpty()) {
        return AppError(ErrorDomain::Ai, static_cast<int>(AiErrorCode::InvalidRequest),
            QStringLiteral("未配置 AI API Key"));
    }
    HttpRequest http;
    http.url = QUrl(endpoint_);
    http.method = QByteArrayLiteral("POST");
    http.headers = {
        {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + apiKey_.toUtf8()},
        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
    };
    http.body = buildPayload(settings_, request);
    http.connectTimeoutMs = kAiConnectTimeoutMs;
    http.transferTimeoutMs = settings_.timeoutMs;
    QElapsedTimer timer;
    timer.start();
    std::variant<HttpResponse, AppError> sent = http_->send(http, cancel);
    const qint64 latencyMs = timer.elapsed();
    if (std::holds_alternative<AppError>(sent)) {
        AppError error = std::get<AppError>(std::move(sent));
        qCWarning(subcueAiLog) << "omni_review provider=" << providerId()
            << "model=" << settings_.model
            << "task_type=" << static_cast<int>(request.task)
            << "request_duration=" << latencyMs
            << "error=" << error.userMessage();
        return error;
    }
    const HttpResponse response = std::get<HttpResponse>(std::move(sent));
    if (response.status >= 400) {
        AppError error = mapHttpError(response.status, response.body);
        qCWarning(subcueAiLog) << "omni_review provider=" << providerId()
            << "model=" << settings_.model
            << "task_type=" << static_cast<int>(request.task)
            << "request_duration=" << latencyMs
            << "error=" << error.userMessage();
        return error;
    }
    const std::optional<QJsonObject> root = OmniReviewJson::objectFromSseOrJson(response.body);
    if (!root) return OmniReviewJson::invalidResponse(QStringLiteral("响应不是 JSON"));
    const QJsonArray choices = root->value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) return OmniReviewJson::invalidResponse(QStringLiteral("缺少 choices"));
    const QJsonObject message = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    const std::optional<QJsonObject> arguments = OmniReviewJson::structuredObject(message);
    OmniChatResponse result;
    result.latencyMs = latencyMs;
    result.usage = OmniReviewJson::usageFromObject(root->value(QStringLiteral("usage")).toObject());
    result.model = root->value(QStringLiteral("model")).toString(settings_.model);
    result.rawContent = message.value(QStringLiteral("content")).toString();
    result.fromToolCall = message.contains(QStringLiteral("tool_calls"));
    if (arguments) result.arguments = *arguments;
    qCInfo(subcueAiLog) << "omni_review provider=" << providerId()
        << "model=" << result.model
        << "task_type=" << static_cast<int>(request.task)
        << "request_duration=" << latencyMs
        << "token_usage=" << result.usage.totalTokens
        << "from_tool_call=" << result.fromToolCall;
    if (!arguments && !request.tools.isEmpty()) {
        return OmniReviewJson::invalidResponse(QStringLiteral("无法解析结构化结果"));
    }
    return result;
}

OmniTestResult QwenOmniProvider::testConnection(const std::atomic<bool> *cancel)
{
    OmniChatRequest request;
    request.task = OmniTaskType::ConnectionTest;
    request.systemPrompt = QStringLiteral("Call submit_connection_test with ok=true.");
    request.userText = QStringLiteral(
        "This is a built-in probe clip. Confirm you received the audio and call submit_connection_test.");
    request.audio = builtInProbeClip();
    request.tools = connectionTestToolSchema();
    request.toolChoiceName = QStringLiteral("submit_connection_test");
    OmniCompleteResult completed = complete(request, cancel);
    if (std::holds_alternative<AppError>(completed)) {
        return std::get<AppError>(std::move(completed));
    }
    const OmniChatResponse response = std::get<OmniChatResponse>(std::move(completed));
    OmniConnectionTestResult result;
    result.model = response.model.isEmpty() ? settings_.model : response.model;
    result.latencyMs = response.latencyMs;
    result.usage = response.usage;
    result.verifiedAtUtc = QDateTime::currentDateTimeUtc();
    return result;
}

} // namespace subcue
