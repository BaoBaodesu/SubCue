#include "ai/ai_review_json.h"
#include "ai/ai_review_settings.h"
#include "ai/ai_types.h"
#include "ai/qwen_omni_provider.h"
#include "asr/http_client.h"
#include "settings/credential_store.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

#include <atomic>

using namespace subcue;

namespace {

class FakeHttpClient final : public IHttpClient {
public:
    HttpResponse response;
    AppError error{ErrorDomain::Network, 0, QString()};
    bool fail = false;
    HttpRequest lastRequest;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel) override
    {
        lastRequest = request;
        if (aiCancelled(cancel)) return aiCancelledError();
        if (fail) return error;
        return response;
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &, QFile *, qint64, const std::atomic<bool> *,
        const std::function<void(qint64, qint64)> &) override
    {
        return AppError(ErrorDomain::Network, 1, QStringLiteral("unused"));
    }
};

QByteArray toolCallBody(const QJsonObject &arguments,
                        const QString &toolName = QStringLiteral("submit_subtitle_review"),
                        const QString &model = QStringLiteral("qwen3.8-omni-flash"))
{
    QJsonObject function;
    function.insert(QStringLiteral("name"), toolName);
    function.insert(QStringLiteral("arguments"),
        QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
    QJsonObject toolCall;
    toolCall.insert(QStringLiteral("function"), function);
    QJsonObject message;
    message.insert(QStringLiteral("tool_calls"), QJsonArray{toolCall});
    QJsonObject choice;
    choice.insert(QStringLiteral("message"), message);
    QJsonObject usage;
    usage.insert(QStringLiteral("prompt_tokens"), 12);
    usage.insert(QStringLiteral("completion_tokens"), 8);
    usage.insert(QStringLiteral("total_tokens"), 20);
    QJsonObject root;
    root.insert(QStringLiteral("model"), model);
    root.insert(QStringLiteral("choices"), QJsonArray{choice});
    root.insert(QStringLiteral("usage"), usage);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

class OmniProviderTests final : public QObject {
    Q_OBJECT

private slots:
    void payloadOmitsApiKeyAndUsesConfiguredModel();
    void testConnectionReturnsModelLatencyAndUsage();
    void invalidApiKeyMapsToChineseError();
    void modelPermissionMapsToChineseError();
    void rateLimitMapsToChineseError();
    void httpTimeoutMapsToChineseError();
    void timeoutKeepsLocalMessage();
    void parsesToolCallAndPlainJson();
    void resolveApiKeyPrefersUiThenSavedThenEnv();
    void wordMappingToolIsPresent();
};

void OmniProviderTests::payloadOmitsApiKeyAndUsesConfiguredModel()
{
    OmniReviewSettings settings;
    settings.model = QStringLiteral("qwen3.8-omni-flash");
    OmniChatRequest request;
    request.userText = QStringLiteral("ping");
    const QByteArray payload = QwenOmniProvider::buildPayload(settings, request);
    QVERIFY(!payload.contains("sk-"));
    QVERIFY(!payload.contains("api_key"));
    QVERIFY(payload.contains("qwen3.8-omni-flash"));
    QVERIFY(payload.contains("modalities"));
}

void OmniProviderTests::testConnectionReturnsModelLatencyAndUsage()
{
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = toolCallBody({{QStringLiteral("ok"), true}},
        QStringLiteral("submit_connection_test"));
    OmniReviewSettings settings;
    QwenOmniProvider provider(QStringLiteral("secret-key"), settings, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<OmniConnectionTestResult>(result));
    const auto connected = std::get<OmniConnectionTestResult>(result);
    QCOMPARE(connected.model, QStringLiteral("qwen3.8-omni-flash"));
    QCOMPARE(connected.usage.totalTokens, 20);
    QVERIFY(!QString::fromUtf8(http.lastRequest.body).contains("secret-key"));
    QVERIFY(http.lastRequest.body.contains("submit_connection_test"));
    QVERIFY(http.lastRequest.body.contains("input_audio") || http.lastRequest.body.contains("audio"));
    QVERIFY(http.lastRequest.headers.size() >= 1);
}

void OmniProviderTests::invalidApiKeyMapsToChineseError()
{
    FakeHttpClient http;
    http.response.status = 401;
    http.response.body = QByteArrayLiteral("{\"error\":\"unauthorized\"}");
    QwenOmniProvider provider(QStringLiteral("bad"), {}, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("认证失败：API Key 无效或已过期"));
}

void OmniProviderTests::modelPermissionMapsToChineseError()
{
    FakeHttpClient http;
    http.response.status = 403;
    http.response.body = QByteArrayLiteral("{\"error\":{\"code\":\"AccessDenied\"}}");
    QwenOmniProvider provider(QStringLiteral("key"), {}, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(),
             QStringLiteral("当前 API Key 无 qwen3.8-omni-flash 模型权限"));
}

void OmniProviderTests::rateLimitMapsToChineseError()
{
    FakeHttpClient http;
    http.response.status = 429;
    QwenOmniProvider provider(QStringLiteral("key"), {}, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("请求过于频繁，请稍后再试"));
}

void OmniProviderTests::httpTimeoutMapsToChineseError()
{
    FakeHttpClient http;
    http.response.status = 408;
    QwenOmniProvider provider(QStringLiteral("key"), {}, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("AI 请求超时"));
}

void OmniProviderTests::timeoutKeepsLocalMessage()
{
    FakeHttpClient http;
    http.fail = true;
    http.error = AppError(ErrorDomain::Network, 2, QStringLiteral("网络请求失败"));
    QwenOmniProvider provider(QStringLiteral("key"), {}, &http);
    OmniTestResult result = provider.testConnection();
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("网络请求失败"));
}

void OmniProviderTests::parsesToolCallAndPlainJson()
{
    QJsonObject item;
    item.insert(QStringLiteral("segment_id"), QStringLiteral("s1"));
    item.insert(QStringLiteral("issue_type"), QStringLiteral("MISSING_TEXT"));
    item.insert(QStringLiteral("confidence"), 0.93);
    item.insert(QStringLiteral("original_text"), QStringLiteral("你好"));
    item.insert(QStringLiteral("suggested_text"), QStringLiteral("你好世界"));
    item.insert(QStringLiteral("reason"), QStringLiteral("漏字"));
    item.insert(QStringLiteral("decision"), QStringLiteral("REPLACE_TEXT"));
    QJsonObject payload;
    payload.insert(QStringLiteral("results"), QJsonArray{item});
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = toolCallBody(payload);
    QwenOmniProvider provider(QStringLiteral("key"), {}, &http);
    OmniChatRequest request;
    request.userText = QStringLiteral("review");
    request.tools = QwenOmniProvider::subtitleToolSchema();
    OmniCompleteResult completed = provider.complete(request);
    QVERIFY(std::holds_alternative<OmniChatResponse>(completed));
    QVERIFY(std::get<OmniChatResponse>(completed).fromToolCall);
    SubtitleOmniResult parsed = OmniReviewJson::parseSubtitleSuggestions(
        std::get<OmniChatResponse>(completed).arguments);
    QVERIFY(std::holds_alternative<QVector<SubtitleOmniSuggestion>>(parsed));
    QCOMPARE(std::get<QVector<SubtitleOmniSuggestion>>(parsed).at(0).decision,
             SubtitleOmniDecision::ReplaceText);

    QJsonObject message;
    message.insert(QStringLiteral("content"),
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    const auto object = OmniReviewJson::structuredObject(message);
    QVERIFY(object.has_value());
}

void OmniProviderTests::resolveApiKeyPrefersUiThenSavedThenEnv()
{
    QCOMPARE(OmniReviewSettingsStore::resolveApiKey(QStringLiteral(" ui-key ")),
             QStringLiteral("ui-key"));
    class MemoryStore final : public ICredentialStore {
    public:
        mutable QString secret;
        QString load(const QString &, QString *) const override { return secret; }
        bool save(const QString &, const QString &value, QString *) const override
        {
            secret = value;
            return true;
        }
        bool remove(const QString &, QString *) const override { secret.clear(); return true; }
        bool exists(const QString &) const override { return !secret.isEmpty(); }
    };
    MemoryStore store;
    store.secret = QStringLiteral("saved-key");
    const QByteArray previous = qgetenv(kOmniReviewEnvKey);
    qputenv(kOmniReviewEnvKey, "env-key");
    QCOMPARE(OmniReviewSettingsStore::resolveApiKey({}, &store), QStringLiteral("saved-key"));
    QCOMPARE(OmniReviewSettingsStore::resolveApiKeySource({}, &store), AiKeySource::Saved);
    QCOMPARE(OmniReviewSettingsStore::resolveApiKeySource({}, &store, true), AiKeySource::Environment);
    QCOMPARE(OmniReviewSettingsStore::resolveApiKeyLabel({}, &store, true),
             QStringLiteral("当前来源：环境变量 DASHSCOPE_API_KEY"));
    store.secret.clear();
    QCOMPARE(OmniReviewSettingsStore::resolveApiKey({}, &store), QStringLiteral("env-key"));
    if (previous.isNull()) qunsetenv(kOmniReviewEnvKey);
    else qputenv(kOmniReviewEnvKey, previous);
}

void OmniProviderTests::wordMappingToolIsPresent()
{
    const QJsonArray tools = QwenOmniProvider::wordMappingToolSchema();
    QCOMPARE(tools.size(), 1);
    QCOMPARE(tools.at(0).toObject().value(QStringLiteral("function")).toObject()
                 .value(QStringLiteral("name")).toString(),
             QStringLiteral("submit_word_mapping"));
}

QTEST_MAIN(OmniProviderTests)
#include "test_qwen_omni_provider.moc"
