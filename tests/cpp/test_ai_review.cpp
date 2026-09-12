#include "ai/ai_provider_factory.h"
#include "ai/openai_compatible_provider.h"
#include "alignment/alignment_engine.h"
#include "asr/http_client.h"
#include "settings/settings_manager.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <atomic>
#include <utility>
#include <variant>

using namespace subcue;

namespace {

class FakeHttpClient final : public IHttpClient {
public:
    HttpResponse response;
    AppError error{ErrorDomain::Network, 0, QString()};
    bool fail = false;
    int failTimes = 0;
    int sendCount = 0;
    HttpRequest lastRequest;
    QVector<HttpRequest> requests;
    QVector<HttpResponse> responses;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel) override
    {
        lastRequest = request;
        requests.push_back(request);
        ++sendCount;
        if (aiCancelled(cancel)) {
            return aiCancelledError();
        }
        if (fail || failTimes > 0) {
            if (failTimes > 0) {
                --failTimes;
            }
            return error;
        }
        if (!responses.isEmpty()) {
            const int index = qMin(sendCount - 1, static_cast<int>(responses.size() - 1));
            return responses.at(index);
        }
        return response;
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &,
        QFile *,
        qint64,
        const std::atomic<bool> *,
        const std::function<void(qint64, qint64)> &) override
    {
        return AppError(ErrorDomain::Network, static_cast<int>(AiErrorCode::InvalidRequest),
            QStringLiteral("AI 测试不使用 download"));
    }
};

QByteArray chatContentBody(const QJsonObject &mapping)
{
    const QJsonObject payload{{QStringLiteral("mappings"), QJsonArray{mapping}}};
    const QJsonObject message{
        {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))},
    };
    const QJsonObject body{
        {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message}}}},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QJsonObject matchedMapping(int pythonIndex, const QString &start, const QString &end, double confidence = 0.93)
{
    return {
        {QStringLiteral("line_id"), QStringLiteral("L%1").arg(pythonIndex)},
        {QStringLiteral("start_word_id"), start},
        {QStringLiteral("end_word_id"), end},
        {QStringLiteral("confidence"), confidence},
        {QStringLiteral("status"), QStringLiteral("matched")},
    };
}

QVector<Subtitle> numberedSubtitles(int count, double confidence = 0.9)
{
    QVector<Subtitle> subtitles;
    subtitles.reserve(count);
    for (int index = 1; index <= count; ++index) {
        Subtitle subtitle;
        subtitle.text = QStringLiteral("第%1条").arg(index);
        subtitle.confidence = confidence;
        subtitles.push_back(subtitle);
    }
    return subtitles;
}

QVector<TranscriptWord> numberedWords(int count)
{
    QVector<TranscriptWord> words;
    words.reserve(count);
    for (int index = 1; index <= count; ++index) {
        words.push_back(TranscriptWord{
            index,
            QStringLiteral("词%1").arg(index),
            static_cast<qint64>(index) * 100,
            static_cast<qint64>(index) * 100 + 80,
        });
    }
    return words;
}

QJsonObject requestJson(const HttpRequest &request)
{
    return QJsonDocument::fromJson(request.body).object();
}

QByteArray headerValue(const HttpRequest &request, const QByteArray &name)
{
    for (const HttpHeader &header : request.headers) {
        if (header.name.compare(name, Qt::CaseInsensitive) == 0) {
            return header.value;
        }
    }
    return {};
}

} // namespace

class AiReviewTests final : public QObject {
    Q_OBJECT

private slots:
    void onlySendsAndAppliesLocalWindow();
    void payloadOmitsApiKeyAndUsesSchema();
    void skipsHighConfidence();
    void applyRejectsOverlapAndLowConfidence();
    void notPresentAndUncertainZeroTimes();
    void httpErrorSetsLowConfidenceAndContinues();
    void cancelStopsBeforeCall();
    void factoryReadsModelAndRegion();
    void modelDiscoveryParsesAndSortsModels();
    void modelDiscoveryRejectsInvalidResponses();
    void qtNetworkClientPostsChatCompletion();
};

void AiReviewTests::onlySendsAndAppliesLocalWindow()
{
    QVector<Subtitle> subtitles = numberedSubtitles(5);
    subtitles[2].confidence = 0.5;
    subtitles[2].startWordId = 30;
    subtitles[2].endWordId = 32;
    QVector<TranscriptWord> words = numberedWords(80);
    words[27].text = QStringLiteral("第");
    words[28].text = QStringLiteral("3");
    words[29].text = QStringLiteral("条");

    FakeHttpClient http;
    http.response.status = 200;
    http.response.contentType = QByteArrayLiteral("application/json");
    http.response.body = chatContentBody(matchedMapping(3, QStringLiteral("W28"), QStringLiteral("W30")));

    OpenAICompatibleProvider provider(QStringLiteral("secret"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    AiReviewResult result = provider.review(subtitles, words);
    QVERIFY(std::holds_alternative<int>(result));
    QCOMPARE(std::get<int>(result), 1);
    QCOMPARE(http.sendCount, 1);
    QCOMPARE(subtitles.at(2).source, QStringLiteral("llm"));
    QCOMPARE(subtitles.at(2).startWordId, 28);
    QCOMPARE(subtitles.at(2).endWordId, 30);
    QCOMPARE(subtitles.at(2).status, QStringLiteral("MATCHED"));
    QCOMPARE(subtitles.at(2).candidateText, QStringLiteral("第3条"));
    QCOMPARE(subtitles.at(2).start.milliseconds(), 2800);
    QCOMPARE(subtitles.at(2).end.milliseconds(), 3080);

    const QJsonObject payload = requestJson(http.lastRequest);
    const QString prompt = payload.value(QStringLiteral("messages")).toArray().at(1)
        .toObject().value(QStringLiteral("content")).toString();
    QVERIFY(prompt.contains(QStringLiteral("L1: 第1条")));
    QVERIFY(prompt.contains(QStringLiteral("L5: 第5条")));
    QVERIFY(!prompt.contains(QStringLiteral("W4: 词4\n")));
    QVERIFY(!prompt.contains(QStringLiteral("W56: 词56")));
    QCOMPARE(payload.value(QStringLiteral("response_format")).toObject()
        .value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
}

void AiReviewTests::payloadOmitsApiKeyAndUsesSchema()
{
    QVector<Subtitle> subtitles = numberedSubtitles(1, 0.4);
    QVector<TranscriptWord> words = numberedWords(10);
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = chatContentBody(matchedMapping(1, QStringLiteral("W1"), QStringLiteral("W1"), 0.2));

    OpenAICompatibleProvider provider(QStringLiteral("secret-key-value"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    QVERIFY(std::holds_alternative<int>(provider.review(subtitles, words)));

    QCOMPARE(headerValue(http.lastRequest, QByteArrayLiteral("Authorization")),
        QByteArrayLiteral("Bearer secret-key-value"));
    QVERIFY(!http.lastRequest.body.contains("secret-key-value"));
    const QJsonObject payload = requestJson(http.lastRequest);
    QVERIFY(!payload.contains(QStringLiteral("api_key")));
    QVERIFY(!payload.contains(QStringLiteral("Authorization")));
    QCOMPARE(payload.value(QStringLiteral("model")).toString(), QStringLiteral("qwen3.8-flash"));
    QCOMPARE(payload.value(QStringLiteral("enable_thinking")).toBool(true), false);
    QCOMPARE(payload.value(QStringLiteral("messages")).toArray().at(0)
        .toObject().value(QStringLiteral("content")).toString(),
        OpenAICompatibleProvider::systemPrompt());
    const QJsonObject format = payload.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(format.value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
    QCOMPARE(format.value(QStringLiteral("json_schema")).toObject()
        .value(QStringLiteral("name")).toString(), QStringLiteral("subtitle_alignment"));
    QCOMPARE(format.value(QStringLiteral("json_schema")).toObject()
        .value(QStringLiteral("strict")).toBool(), true);
    QCOMPARE(http.lastRequest.connectTimeoutMs, kAiConnectTimeoutMs);
    QCOMPARE(http.lastRequest.transferTimeoutMs, kAiTransferTimeoutMs);
}

void AiReviewTests::skipsHighConfidence()
{
    QVector<Subtitle> subtitles = numberedSubtitles(3, 0.9);
    subtitles[1].confidence = 0.74;
    QVector<TranscriptWord> words = numberedWords(20);
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = chatContentBody(matchedMapping(2, QStringLiteral("W2"), QStringLiteral("W2")));

    OpenAICompatibleProvider provider(QStringLiteral("secret"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    AiReviewResult result = provider.review(subtitles, words);
    QVERIFY(std::holds_alternative<int>(result));
    QCOMPARE(std::get<int>(result), 1);
    QCOMPARE(http.sendCount, 1);
    QVERIFY(AlignmentEngine::needsAiReview(0.74));
    QVERIFY(!AlignmentEngine::needsAiReview(0.9));
}

void AiReviewTests::applyRejectsOverlapAndLowConfidence()
{
    QVector<Subtitle> subtitles = numberedSubtitles(3, 0.4);
    subtitles[0].endWordId = 30;
    subtitles[2].startWordId = 31;
    QVector<TranscriptWord> words = numberedWords(40);
    words[27].text = QStringLiteral("第");
    words[28].text = QStringLiteral("2");
    words[29].text = QStringLiteral("条");

    QVERIFY(!OpenAICompatibleProvider::applyIfValid(
        subtitles[1], 1, matchedMapping(2, QStringLiteral("W28"), QStringLiteral("W30")),
        subtitles, words));

    subtitles[0].endWordId = 10;
    subtitles[2].startWordId = 40;
    QVERIFY(!OpenAICompatibleProvider::applyIfValid(
        subtitles[1], 1, matchedMapping(2, QStringLiteral("W28"), QStringLiteral("W30"), 0.5),
        subtitles, words));

    QVERIFY(OpenAICompatibleProvider::applyIfValid(
        subtitles[1], 1, matchedMapping(2, QStringLiteral("W28"), QStringLiteral("W30")),
        subtitles, words));
    QCOMPARE(subtitles.at(1).source, QStringLiteral("llm"));
    QCOMPARE(subtitles.at(1).startWordId, 28);
}

void AiReviewTests::notPresentAndUncertainZeroTimes()
{
    QVector<Subtitle> subtitles = numberedSubtitles(2, 0.4);
    QVector<TranscriptWord> words = numberedWords(10);
    FakeHttpClient http;
    http.responses = {
        HttpResponse{200, QByteArrayLiteral("application/json"), chatContentBody({
            {QStringLiteral("line_id"), QStringLiteral("L1")},
            {QStringLiteral("start_word_id"), QJsonValue::Null},
            {QStringLiteral("end_word_id"), QJsonValue::Null},
            {QStringLiteral("confidence"), 0.9},
            {QStringLiteral("status"), QStringLiteral("not_present")},
        })},
        HttpResponse{200, QByteArrayLiteral("application/json"), chatContentBody({
            {QStringLiteral("line_id"), QStringLiteral("L2")},
            {QStringLiteral("start_word_id"), QJsonValue::Null},
            {QStringLiteral("end_word_id"), QJsonValue::Null},
            {QStringLiteral("confidence"), 0.2},
            {QStringLiteral("status"), QStringLiteral("uncertain")},
        })},
    };

    OpenAICompatibleProvider provider(QStringLiteral("secret"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    AiReviewResult result = provider.review(subtitles, words);
    QVERIFY(std::holds_alternative<int>(result));
    QCOMPARE(std::get<int>(result), 2);
    QCOMPARE(subtitles.at(0).status, QStringLiteral("SKIPPED_NO_AUDIO"));
    QCOMPARE(subtitles.at(0).start.microseconds(), 0);
    QCOMPARE(subtitles.at(0).end.microseconds(), 0);
    QCOMPARE(subtitles.at(0).skipReason, QStringLiteral("AI 判定音频中不存在该片段"));
    QCOMPARE(subtitles.at(1).status, QStringLiteral("SKIPPED_NO_AUDIO"));
    QCOMPARE(subtitles.at(1).skipReason, QStringLiteral("音频证据不足"));
}

void AiReviewTests::httpErrorSetsLowConfidenceAndContinues()
{
    QVector<Subtitle> subtitles = numberedSubtitles(2, 0.4);
    QVector<TranscriptWord> words = numberedWords(10);
    words[0].text = QStringLiteral("第");
    words[1].text = QStringLiteral("2");
    words[2].text = QStringLiteral("条");

    FakeHttpClient http;
    http.responses = {
        HttpResponse{500, QByteArrayLiteral("application/json"), QByteArrayLiteral("{}")},
        HttpResponse{200, QByteArrayLiteral("application/json"),
            chatContentBody(matchedMapping(2, QStringLiteral("W1"), QStringLiteral("W3")))},
    };

    OpenAICompatibleProvider provider(QStringLiteral("secret"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    AiReviewResult result = provider.review(subtitles, words);
    QVERIFY(std::holds_alternative<int>(result));
    QCOMPARE(std::get<int>(result), 2);
    QCOMPARE(provider.errors().size(), 1);
    QCOMPARE(provider.errors().at(0).userMessage(), QStringLiteral("AI 复核请求失败（HTTP 500）"));
    QCOMPARE(subtitles.at(0).status, QStringLiteral("LOW_CONFIDENCE"));
    QCOMPARE(subtitles.at(1).source, QStringLiteral("llm"));
    QCOMPARE(subtitles.at(1).startWordId, 1);
}

void AiReviewTests::cancelStopsBeforeCall()
{
    QVector<Subtitle> subtitles = numberedSubtitles(2, 0.4);
    QVector<TranscriptWord> words = numberedWords(10);
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = chatContentBody(matchedMapping(1, QStringLiteral("W1"), QStringLiteral("W1")));

    std::atomic<bool> cancel{true};
    OpenAICompatibleProvider provider(QStringLiteral("secret"), QStringLiteral("qwen3.8-flash"),
        QStringLiteral("beijing"), &http);
    AiReviewResult result = provider.review(subtitles, words, &cancel);
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("任务已取消"));
    QCOMPARE(http.sendCount, 0);
    QCOMPARE(subtitles.at(0).source, QStringLiteral("local"));
}

void AiReviewTests::factoryReadsModelAndRegion()
{
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = chatContentBody({
        {QStringLiteral("line_id"), QStringLiteral("L1")},
        {QStringLiteral("status"), QStringLiteral("uncertain")},
        {QStringLiteral("start_word_id"), QJsonValue::Null},
        {QStringLiteral("end_word_id"), QJsonValue::Null},
        {QStringLiteral("confidence"), 0.1},
    });

    QJsonObject settings = SettingsManager::defaults();
    settings.insert(QStringLiteral("aiProviderId"), QStringLiteral("custom"));
    settings.insert(QStringLiteral("aiProviders"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("custom")},
        {QStringLiteral("name"), QStringLiteral("Custom")},
        {QStringLiteral("baseUrl"), QStringLiteral("https://example.com/v1")},
        {QStringLiteral("authMode"), QStringLiteral("bearer")},
        {QStringLiteral("selectedModel"), QStringLiteral("qwen-plus")},
    }});
    AiProviderFactory factory(&http);
    std::unique_ptr<IAiProvider> provider = factory.create(settings, QStringLiteral("secret"));
    QCOMPARE(provider->providerId(), QStringLiteral("openai-compatible"));

    QVector<Subtitle> subtitles = numberedSubtitles(1, 0.4);
    QVERIFY(std::holds_alternative<int>(provider->review(subtitles, numberedWords(5))));
    QCOMPARE(http.lastRequest.url.toString(),
        QStringLiteral("https://example.com/v1/chat/completions"));
    QCOMPARE(requestJson(http.lastRequest).value(QStringLiteral("model")).toString(),
        QStringLiteral("qwen-plus"));

    OpenAICompatibleProvider beijing(QStringLiteral("k"), QStringLiteral("m"));
    QCOMPARE(beijing.endpoint(),
        QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"));
}

void AiReviewTests::modelDiscoveryParsesAndSortsModels()
{
    FakeHttpClient http;
    http.response.status = 200;
    http.response.body = R"({"data":[{"id":"model-z"},{"id":"model-a"}]})";
    OpenAICompatibleProvider provider(QString(), QString(),
        QUrl(QStringLiteral("http://127.0.0.1:8080/")), false, &http);

    const ModelListResult listed = provider.listModels();
    QVERIFY(std::holds_alternative<QVector<ModelDescriptor>>(listed));
    const QVector<ModelDescriptor> models = std::get<QVector<ModelDescriptor>>(listed);
    QCOMPARE(models.size(), 2);
    QCOMPARE(models.at(0).id, QStringLiteral("model-a"));
    QCOMPARE(models.at(1).id, QStringLiteral("model-z"));
    QCOMPARE(http.lastRequest.url.toString(), QStringLiteral("http://127.0.0.1:8080/v1/models"));
    QVERIFY(headerValue(http.lastRequest, QByteArrayLiteral("Authorization")).isEmpty());
    QCOMPARE(OpenAICompatibleProvider::qwenModelsEndpoint(
        QUrl(QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"))).toString(),
        QStringLiteral("https://dashscope.aliyuncs.com/api/v1/models?providers=qwen&capabilities=TG&page_size=100"));
    QCOMPARE(OpenAICompatibleProvider::qwenModelsEndpoint(
        QUrl(QStringLiteral("https://llm-example.cn-beijing.maas.aliyuncs.com/compatible-mode/v1"))).toString(),
        QStringLiteral("https://llm-example.cn-beijing.maas.aliyuncs.com/api/v1/models?providers=qwen&capabilities=TG&page_size=100"));

    http.response.body = R"({"output":{"models":[{"model":"qwen-plus","name":"通义千问 Plus"}]}})";
    OpenAICompatibleProvider qwen(QStringLiteral("sk-ws-test"), QString(),
        QUrl(QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1")), true,
        OpenAICompatibleProvider::qwenModelsEndpoint(
            QUrl(QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1"))), &http);
    const ModelListResult qwenListed = qwen.listModels();
    QVERIFY(std::holds_alternative<QVector<ModelDescriptor>>(qwenListed));
    QCOMPARE(std::get<QVector<ModelDescriptor>>(qwenListed).at(0).id,
        QStringLiteral("qwen-plus"));
    QCOMPARE(headerValue(http.lastRequest, QByteArrayLiteral("Authorization")),
        QByteArrayLiteral("Bearer sk-ws-test"));
}

void AiReviewTests::modelDiscoveryRejectsInvalidResponses()
{
    FakeHttpClient http;
    OpenAICompatibleProvider provider(QStringLiteral("secret"), QString(),
        QUrl(QStringLiteral("https://example.com/v1")), true, &http);

    http.response = {401, QByteArrayLiteral("application/json"), QByteArrayLiteral("{}")};
    QVERIFY(std::holds_alternative<AppError>(provider.listModels()));
    QCOMPARE(headerValue(http.lastRequest, QByteArrayLiteral("Authorization")),
        QByteArrayLiteral("Bearer secret"));

    http.response = {200, QByteArrayLiteral("application/json"), QByteArrayLiteral("{\"data\":[]}")};
    QVERIFY(std::holds_alternative<AppError>(provider.listModels()));

    std::atomic<bool> cancel{true};
    const int callsBeforeCancel = http.sendCount;
    QVERIFY(std::holds_alternative<AppError>(provider.listModels(&cancel)));
    QCOMPARE(http.sendCount, callsBeforeCancel);
}

void AiReviewTests::qtNetworkClientPostsChatCompletion()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const quint16 port = server.serverPort();
    QByteArray lastRequest;
    const QByteArray replyBody = chatContentBody(
        matchedMapping(1, QStringLiteral("W1"), QStringLiteral("W1")));

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &lastRequest, replyBody]() {
            lastRequest += socket->readAll();
            if (!lastRequest.contains("\r\n\r\n")) {
                return;
            }
            QByteArray response;
            response += "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: application/json\r\n";
            response += "Content-Length: " + QByteArray::number(replyBody.size()) + "\r\n";
            response += "Connection: close\r\n\r\n";
            response += replyBody;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    QtNetworkHttpClient client;
    HttpRequest request;
    request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(port));
    request.method = QByteArrayLiteral("POST");
    request.headers = {
        {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer secret")},
        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
    };
    request.body = OpenAICompatibleProvider::buildPayload(
        QStringLiteral("qwen3.8-flash"),
        OpenAICompatibleProvider::buildUserPrompt(numberedSubtitles(1, 0.4), numberedWords(3), 0));
    std::variant<HttpResponse, AppError> posted = client.send(request);
    QVERIFY(std::holds_alternative<HttpResponse>(posted));
    QCOMPARE(std::get<HttpResponse>(posted).status, 200);
    QVERIFY(lastRequest.contains("Authorization: Bearer secret"));
    QVERIFY(!request.body.contains("secret"));
    std::variant<std::optional<QJsonObject>, AppError> mapping =
        OpenAICompatibleProvider::mappingFromResponse(std::get<HttpResponse>(posted).body, 1);
    QVERIFY(std::holds_alternative<std::optional<QJsonObject>>(mapping));
    QVERIFY(std::get<std::optional<QJsonObject>>(mapping).has_value());
}

QTEST_GUILESS_MAIN(AiReviewTests)
#include "test_ai_review.moc"
