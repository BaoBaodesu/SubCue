#include "asr/asr_provider_factory.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "asr/dashscope_asr_service.h"
#include "asr/http_client.h"
#include "asr/local_python_asr_service.h"
#include "alignment/alignment_pipeline.h"
#include "roughcut/script_document.h"
#include "settings/settings_manager.h"
#include "inference/inference_manager.h"

#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <optional>
#include <utility>
#include <variant>

using namespace subcue;

namespace {

QByteArray jsonWordsBody()
{
    return R"({"output":{"sentence":{"sentence_id":1,"sentence_end":true,"words":[
        {"text":"Hello","begin_time":100,"end_time":500},
        {"text":"字幕","begin_time":500,"end_time":900}
    ]}}})";
}

class FakeHttpClient final : public IHttpClient {
public:
    HttpResponse response;
    AppError error{ErrorDomain::Network, 0, QString()};
    bool fail = false;
    int sendCount = 0;
    HttpRequest lastRequest;
    QByteArray downloadPayload;
    qint64 cancelAfter = -1;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel) override
    {
        ++sendCount;
        lastRequest = request;
        if (asrCancelled(cancel)) {
            return asrCancelledError();
        }
        if (fail) {
            return error;
        }
        return response;
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &request,
        QFile *output,
        qint64 resumeFrom,
        const std::atomic<bool> *cancel,
        const std::function<void(qint64, qint64)> &progress) override
    {
        lastRequest = request;
        if (asrCancelled(cancel)) {
            return asrCancelledError();
        }
        if (fail) {
            return error;
        }
        if (resumeFrom < 0 || resumeFrom > downloadPayload.size()) {
            return AppError(ErrorDomain::Network, static_cast<int>(AsrErrorCode::DownloadFailure),
                QStringLiteral("Range 无效"));
        }
        if (cancelAfter >= 0 && resumeFrom < cancelAfter) {
            const QByteArray partial = downloadPayload.mid(
                static_cast<int>(resumeFrom),
                static_cast<int>(cancelAfter - resumeFrom));
            output->write(partial);
            if (progress) {
                progress(resumeFrom + partial.size(), downloadPayload.size());
            }
            return asrCancelledError();
        }
        const QByteArray slice = downloadPayload.mid(static_cast<int>(resumeFrom));
        output->write(slice);
        if (progress) {
            progress(resumeFrom + slice.size(), downloadPayload.size());
        }
        return resumeFrom + slice.size();
    }
};

QString mediaPath(const QString &name)
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

} // namespace

class AsrTests final : public QObject {
    Q_OBJECT

private slots:
    void chunkPlanMatchesPython();
    void overlapDuplicateMatchesPython();
    void dashscopeParsesJsonAndSse();
    void dashscopePayloadOmitsApiKey();
    void dashscopeMergesChunksAndSkipsOverlap();
    void dashscopeHttpErrorAndCancel();
    void factoryKeepsDashScopeDefault();
    void localModelRejectsIncompleteWeight();
    void inferenceManagerContainsWorkerFailure();
    void extractorWritesFlacAndPcm();
    void realQwenFixtureAlignsWhenRequested();
    void qtNetworkClientDownloadsAndPosts();
};

void AsrTests::chunkPlanMatchesPython()
{
    const QVector<AudioChunkWindow> shortClip = AudioChunkPlanner::plan(10.0);
    QCOMPARE(shortClip.size(), 1);
    QCOMPARE(shortClip.at(0).startMs, 0);
    QCOMPARE(shortClip.at(0).endMs, 10'000);

    const QVector<AudioChunkWindow> exact = AudioChunkPlanner::plan(270.0);
    QCOMPARE(exact.size(), 1);
    QCOMPARE(exact.at(0).startMs, 0);
    QCOMPARE(exact.at(0).endMs, 270'000);

    const QVector<AudioChunkWindow> overflow = AudioChunkPlanner::plan(270.001);
    QCOMPARE(overflow.size(), 2);
    QCOMPARE(overflow.at(0).startMs, 0);
    QCOMPARE(overflow.at(0).endMs, 270'001);
    QCOMPARE(overflow.at(1).startMs, 269'000);
    QCOMPARE(overflow.at(1).endMs, 270'001);

    const QVector<AudioChunkWindow> three = AudioChunkPlanner::plan(540.001);
    const QVector<AudioChunkWindow> local = AudioChunkPlanner::plan(240.0, 20.0);
    QCOMPARE(local.size(), 12);
    QCOMPARE(local.at(0).startMs, 0);
    QCOMPARE(local.at(0).endMs, 21'000);
    QCOMPARE(local.at(1).startMs, 19'000);
    QCOMPARE(local.last().endMs, 240'000);
    const QVector<AudioChunkWindow> longMedia = AudioChunkPlanner::plan(1'200.0, 20.0);
    QCOMPARE(longMedia.size(), 60);
    QCOMPARE(longMedia.at(20).startMs, 399'000);
    QCOMPARE(longMedia.at(20).endMs, 421'000);
    QCOMPARE(longMedia.last().endMs, 1'200'000);
    QCOMPARE(three.size(), 3);
    QCOMPARE(three.at(0).endMs, 271'000);
    QCOMPARE(three.at(1).startMs, 269'000);
    QCOMPARE(three.at(2).startMs, 539'000);
    QCOMPARE(three.at(2).endMs, 540'001);
}

void AsrTests::overlapDuplicateMatchesPython()
{
    QVector<TranscriptWord> existing{
        {1, QStringLiteral("Hello"), 100, 500},
        {2, QStringLiteral("字幕"), 500, 900},
    };
    TranscriptWord duplicate{3, QStringLiteral("Hello"), 120, 480};
    QVERIFY(DashScopeAsrService::isOverlapDuplicate(duplicate, existing));

    TranscriptWord far{4, QStringLiteral("Hello"), 4'000, 4'400};
    QVERIFY(!DashScopeAsrService::isOverlapDuplicate(far, existing));

    TranscriptWord punctuation{5, QStringLiteral("..."), 200, 300};
    QVERIFY(DashScopeAsrService::isOverlapDuplicate(punctuation, existing));
}

void AsrTests::dashscopeParsesJsonAndSse()
{
    const QVector<QJsonObject> jsonEvents =
        DashScopeAsrService::parseEventPayloads(jsonWordsBody(), QByteArrayLiteral("application/json"));
    QCOMPARE(jsonEvents.size(), 1);
    std::variant<QVector<TranscriptWord>, AppError> words = DashScopeAsrService::wordsFromEvents(jsonEvents);
    QVERIFY(std::holds_alternative<QVector<TranscriptWord>>(words));
    const QVector<TranscriptWord> parsed = std::get<QVector<TranscriptWord>>(words);
    QCOMPARE(parsed.size(), 2);
    QCOMPARE(parsed.at(0).text, QStringLiteral("Hello"));
    QCOMPARE(parsed.at(0).startMs, 100);
    QCOMPARE(parsed.at(1).text, QStringLiteral("字幕"));
    QCOMPARE(parsed.at(1).endMs, 900);

    const QByteArray sse =
        "event:result\n"
        "data:{\"output\":{\"sentence\":{\"sentence_id\":1,\"sentence_end\":false,\"words\":[{\"text\":\"old\",\"begin_time\":1,\"end_time\":2}]}}}\n"
        "data:{\"output\":{\"sentence\":{\"sentence_id\":1,\"sentence_end\":true,\"words\":[{\"text\":\"Hello\",\"begin_time\":100,\"end_time\":500}]}}}\n";
    const QVector<QJsonObject> sseEvents =
        DashScopeAsrService::parseEventPayloads(sse, QByteArrayLiteral("text/event-stream"));
    QCOMPARE(sseEvents.size(), 2);
    std::variant<QVector<TranscriptWord>, AppError> sseWords = DashScopeAsrService::wordsFromEvents(sseEvents);
    QVERIFY(std::holds_alternative<QVector<TranscriptWord>>(sseWords));
    QCOMPARE(std::get<QVector<TranscriptWord>>(sseWords).size(), 1);
    QCOMPARE(std::get<QVector<TranscriptWord>>(sseWords).at(0).text, QStringLiteral("Hello"));

    std::variant<QVector<TranscriptWord>, AppError> empty =
        DashScopeAsrService::wordsFromEvents({});
    QVERIFY(std::holds_alternative<AppError>(empty));
    QCOMPARE(std::get<AppError>(empty).userMessage(), QStringLiteral("语音识别未返回词级时间戳。"));
}

void AsrTests::dashscopePayloadOmitsApiKey()
{
    FakeHttpClient http;
    http.response.status = 200;
    http.response.contentType = QByteArrayLiteral("application/json");
    http.response.body = jsonWordsBody();
    DashScopeAsrService service(
        QStringLiteral("secret-key"),
        QStringLiteral("fun-asr-flash-2026-06-15"),
        QStringLiteral("singapore"),
        &http);
    QCOMPARE(service.endpoint(),
        QStringLiteral("https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"));
    QCOMPARE(DashScopeAsrService::endpointForApiHost(
        QStringLiteral("https://workspace.cn-beijing.maas.aliyuncs.com"), QStringLiteral("beijing")),
        QStringLiteral("https://workspace.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"));
    QCOMPARE(DashScopeAsrService::endpointForApiHost(
        QStringLiteral("https://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1"), QStringLiteral("beijing")),
        QStringLiteral("https://workspace.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"));

    PreparedAudioChunk chunk;
    chunk.window = {0.0, 1.0, 0, 1000};
    chunk.flac = QByteArrayLiteral("fake-flac");
    AsrResult result = service.transcribePreparedChunks({chunk});
    QVERIFY(std::holds_alternative<Transcript>(result));

    const QJsonObject payload = QJsonDocument::fromJson(http.lastRequest.body).object();
    QCOMPARE(payload.value(QStringLiteral("parameters")).toObject()
                 .value(QStringLiteral("format")).toString(),
        QStringLiteral("flac"));
    QCOMPARE(payload.value(QStringLiteral("parameters")).toObject()
                 .value(QStringLiteral("sample_rate")).toString(),
        QStringLiteral("16000"));
    QVERIFY(!QString::fromUtf8(http.lastRequest.body).contains(QStringLiteral("secret-key")));
    bool sawBearer = false;
    for (const HttpHeader &header : http.lastRequest.headers) {
        if (header.name == QByteArrayLiteral("Authorization")) {
            QCOMPARE(header.value, QByteArrayLiteral("Bearer secret-key"));
            sawBearer = true;
        }
    }
    QVERIFY(sawBearer);
}

void AsrTests::dashscopeMergesChunksAndSkipsOverlap()
{
    FakeHttpClient http;
    http.response.status = 200;
    http.response.contentType = QByteArrayLiteral("application/json");
    http.response.body = jsonWordsBody();
    DashScopeAsrService service(
        QStringLiteral("secret"), QStringLiteral("fun-asr-flash-2026-06-15"),
        QStringLiteral("beijing"), &http);

    PreparedAudioChunk first;
    first.window = {0.0, 271.0, 0, 271'000};
    first.flac = QByteArrayLiteral("a");
    PreparedAudioChunk second;
    second.window = {269.0, 271.0, 269'000, 271'000};
    second.flac = QByteArrayLiteral("b");
    QList<int> completed;
    AsrResult result = service.transcribePreparedChunks({first, second}, nullptr, [&](int current, int total) {
        QCOMPARE(total, 2);
        QCOMPARE(current, http.sendCount);
        completed.append(current);
    });
    QCOMPARE(completed, QList<int>({0, 1, 1, 2}));
    QVERIFY(std::holds_alternative<Transcript>(result));
    const Transcript transcript = std::get<Transcript>(result);
    QCOMPARE(transcript.words.size(), 4);
    QCOMPARE(transcript.words.at(0).startMs, 100);
    QCOMPARE(transcript.words.at(2).startMs, 269'100);
    QCOMPARE(transcript.words.at(0).id, 1);
    QCOMPARE(transcript.words.at(3).id, 4);

    PreparedAudioChunk overlap = first;
    overlap.window = {0.0, 1.0, 0, 1000};
    AsrResult skipped = service.transcribePreparedChunks({first, overlap});
    QVERIFY(std::holds_alternative<Transcript>(skipped));
    QCOMPARE(std::get<Transcript>(skipped).words.size(), 2);
}

void AsrTests::dashscopeHttpErrorAndCancel()
{
    FakeHttpClient http;
    http.response.status = 401;
    http.response.body = QByteArrayLiteral(R"({"code":"InvalidApiKey","message":"API key is invalid"})");
    DashScopeAsrService service(
        QStringLiteral("secret"), QStringLiteral("fun-asr-flash-2026-06-15"),
        QStringLiteral("beijing"), &http);
    PreparedAudioChunk chunk;
    chunk.flac = QByteArrayLiteral("x");
    AsrResult failed = service.transcribePreparedChunks({chunk});
    QVERIFY(std::holds_alternative<AppError>(failed));
    QCOMPARE(std::get<AppError>(failed).userMessage(),
        QStringLiteral("语音识别请求失败（HTTP 401）：InvalidApiKey: API key is invalid"));

    std::atomic<bool> cancel{true};
    AsrResult cancelled = service.transcribePreparedChunks({chunk}, &cancel);
    QVERIFY(std::holds_alternative<AppError>(cancelled));
    QCOMPARE(std::get<AppError>(cancelled).userMessage(), QStringLiteral("任务已取消"));
}

void AsrTests::factoryKeepsDashScopeDefault()
{
    const QJsonObject defaults = SettingsManager::defaults();
    QCOMPARE(defaults.value(QStringLiteral("asrProvider")).toString(), QStringLiteral("funasr"));
    QCOMPARE(defaults.value(QStringLiteral("asrModel")).toString(),
        QStringLiteral("Fun-ASR-Nano-2512"));
    QVERIFY(!defaults.contains(QStringLiteral("whisperModel")));
    QVERIFY(!defaults.contains(QStringLiteral("whisperModelsDirectory")));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString settingsPath = directory.filePath(QStringLiteral("settings.json"));
    QFile file(settingsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"version":1,"asrModel":"fun-asr-flash-2026-06-15","region":"beijing"})");
    file.close();
    const QJsonObject loaded = SettingsManager(settingsPath).load();
    QCOMPARE(loaded.value(QStringLiteral("asrProvider")).toString(), QStringLiteral("dashscope"));
    QCOMPARE(loaded.value(QStringLiteral("asrModel")).toString(),
        QStringLiteral("fun-asr-flash-2026-06-15"));
    QCOMPARE(AsrProviderFactory::providerIdFromSettings(loaded), QStringLiteral("dashscope"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"({"version":2,"asrProvider":"whisper","whisperModel":"small"})");
    file.close();
    const QJsonObject migrated = SettingsManager(settingsPath).load();
    QCOMPARE(migrated.value(QStringLiteral("asrProvider")).toString(), QStringLiteral("dashscope"));
    QVERIFY(!migrated.contains(QStringLiteral("whisperModel")));

    FakeHttpClient http;
    AsrProviderFactory factory(&http);
    std::unique_ptr<IAsrService> dashscope = factory.create(loaded, QStringLiteral("secret"));
    QCOMPARE(dashscope->providerId(), QStringLiteral("dashscope"));

    QJsonObject whisperSettings = loaded;
    whisperSettings.insert(QStringLiteral("asrProvider"), QStringLiteral("whisper"));
    std::unique_ptr<IAsrService> retired = factory.create(whisperSettings, QStringLiteral("secret"));
    QCOMPARE(retired->providerId(), QStringLiteral("dashscope"));
    QCOMPARE(loaded.value(QStringLiteral("asrModel")).toString(),
        QStringLiteral("fun-asr-flash-2026-06-15"));
}

void AsrTests::localModelRejectsIncompleteWeight()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile config(directory.filePath(QStringLiteral("config.json")));
    QFile incomplete(directory.filePath(QStringLiteral("model.pt.incomplete")));
    QVERIFY(config.open(QIODevice::WriteOnly));
    QVERIFY(incomplete.open(QIODevice::WriteOnly));
    config.write("{}");
    incomplete.write("partial");
    config.close();
    incomplete.close();
    QVERIFY(!LocalPythonAsrService::modelReady(directory.path()));

    QFile weight(directory.filePath(QStringLiteral("model.pt")));
    QVERIFY(weight.open(QIODevice::WriteOnly));
    weight.write("complete");
    weight.close();
    QVERIFY(LocalPythonAsrService::modelReady(directory.path()));
}

void AsrTests::inferenceManagerContainsWorkerFailure()
{
#ifdef Q_OS_WIN
    const InferenceProcessResult verbose = InferenceManager::instance().run(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
         QStringLiteral("[Console]::Error.Write('x' * 65536)")},
        {}, nullptr, 10'000);
    QVERIFY(verbose.started);
    QVERIFY(!verbose.timedOut);
    QCOMPARE(verbose.exitCode, 0);
    QCOMPARE(verbose.standardError.size(), 65'536);

    const InferenceProcessResult result = InferenceManager::instance().run(
        QStringLiteral("cmd.exe"),
        {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("exit 7")});
    QVERIFY(result.started);
    QCOMPARE(result.exitCode, 7);
#else
    QSKIP("Worker crash containment smoke test is Windows-only");
#endif
}

void AsrTests::extractorWritesFlacAndPcm()
{
    const QString path = mediaPath(QStringLiteral("audio.wav"));
    QVERIFY(QFileInfo::exists(path));
    const AudioChunkWindow window{0.0, 2.0, 0, 2000};
    MediaResult<QVector<float>> pcm = AudioChunkExtractor::decodePcm16kMono(path, window);
    QVERIFY2(std::holds_alternative<QVector<float>>(pcm),
        qPrintable(std::holds_alternative<AppError>(pcm)
            ? std::get<AppError>(pcm).userMessage()
            : QString()));
    QVERIFY(!std::get<QVector<float>>(pcm).isEmpty());

    MediaResult<QByteArray> flac = AudioChunkExtractor::encodeFlac(std::get<QVector<float>>(pcm));
    QVERIFY2(std::holds_alternative<QByteArray>(flac),
        qPrintable(std::holds_alternative<AppError>(flac)
            ? std::get<AppError>(flac).userMessage() + QLatin1Char(' ')
                + std::get<AppError>(flac).technicalDetails()
            : QString()));
    QVERIFY(std::get<QByteArray>(flac).startsWith("fLaC"));
    const QByteArray &encoded = std::get<QByteArray>(flac);
    QVERIFY(encoded.size() >= 26);
    quint64 declaredSamples = 0;
    for (int index = 18; index < 26; ++index) {
        declaredSamples = (declaredSamples << 8)
            | static_cast<unsigned char>(encoded.at(index));
    }
    QCOMPARE(declaredSamples & ((1ULL << 36) - 1),
        static_cast<quint64>(std::get<QVector<float>>(pcm).size()));
}

void AsrTests::qtNetworkClientDownloadsAndPosts()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const quint16 port = server.serverPort();
    QByteArray payload = QByteArrayLiteral("0123456789abcdef");
    QByteArray lastRequest;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &payload, &lastRequest]() {
            lastRequest += socket->readAll();
            if (!lastRequest.contains("\r\n\r\n")) {
                return;
            }
            const int headerEnd = lastRequest.indexOf("\r\n\r\n");
            const QByteArray headers = lastRequest.left(headerEnd);
            int rangeStart = 0;
            const int rangeAt = headers.indexOf("Range: bytes=");
            if (rangeAt >= 0) {
                rangeStart = headers.mid(rangeAt + 13).split('-').value(0).toInt();
            }
            QByteArray body;
            int status = 200;
            QByteArray statusText = QByteArrayLiteral("OK");
            QByteArray contentType = QByteArrayLiteral("application/octet-stream");
            if (headers.startsWith("POST ")) {
                contentType = QByteArrayLiteral("application/json");
                body = jsonWordsBody();
            } else {
                if (rangeStart > 0) {
                    status = 206;
                    statusText = QByteArrayLiteral("Partial Content");
                }
                body = payload.mid(rangeStart);
            }
            QByteArray response;
            response += "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText + "\r\n";
            response += "Content-Type: " + contentType + "\r\n";
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            response += "Connection: close\r\n\r\n";
            response += body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    QtNetworkHttpClient client;
    HttpRequest get;
    get.url = QUrl(QStringLiteral("http://127.0.0.1:%1/ggml-test.bin").arg(port));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("out.bin"));
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    std::variant<qint64, AppError> downloaded = client.download(get, &out, 0);
    out.close();
    QVERIFY2(std::holds_alternative<qint64>(downloaded),
        qPrintable(std::holds_alternative<AppError>(downloaded)
            ? std::get<AppError>(downloaded).userMessage()
            : QString()));
    QFile verify(path);
    QVERIFY(verify.open(QIODevice::ReadOnly));
    QCOMPARE(verify.readAll(), payload);

    lastRequest.clear();
    HttpRequest post;
    post.url = QUrl(QStringLiteral("http://127.0.0.1:%1/asr").arg(port));
    post.method = QByteArrayLiteral("POST");
    post.headers = {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
    post.body = QByteArrayLiteral("{\"model\":\"x\"}");
    std::variant<HttpResponse, AppError> posted = client.send(post);
    QVERIFY(std::holds_alternative<HttpResponse>(posted));
    QCOMPARE(std::get<HttpResponse>(posted).status, 200);
    QVERIFY(std::get<HttpResponse>(posted).body.contains("Hello"));
}

void AsrTests::realQwenFixtureAlignsWhenRequested()
{
    if (qEnvironmentVariableIsEmpty("SUBCUE_RUN_REAL_QWEN_FIXTURE")) {
        QSKIP("Real Qwen fixture requires an installed CUDA model");
    }
    const QString testDir = QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).absoluteFilePath(
        QStringLiteral(".."));
    const QString documentPath = QDir(testDir).filePath(QStringLiteral("测试文案1.docx"));
    const QString media = QDir(testDir).filePath(QStringLiteral("测试音频1.mp4"));
    if (!QFileInfo::exists(documentPath) || !QFileInfo::exists(media)) {
        QSKIP("Real Qwen fixture media is not present");
    }
    const ScriptDocumentResult parsed = ScriptDocumentImporter::loadDocx(documentPath);
    QVERIFY2(std::holds_alternative<ScriptDocument>(parsed),
        qPrintable(std::holds_alternative<AppError>(parsed)
            ? std::get<AppError>(parsed).technicalDetails() : QString()));
    QStringList lines;
    for (const ScriptLine &line : std::get<ScriptDocument>(parsed).lines) {
        if (!line.text.trimmed().isEmpty()) lines.append(line.text.trimmed());
    }
    QJsonObject settings{{QStringLiteral("asrProvider"), QStringLiteral("qwen3")},
        {QStringLiteral("aiAssistEnabled"), false}};
    AlignmentPipeline pipeline(settings, {});
    const AlignmentRunResult result = pipeline.run(media, lines);
    QVERIFY2(std::holds_alternative<AlignmentTaskOutput>(result),
        qPrintable(std::holds_alternative<AppError>(result)
            ? std::get<AppError>(result).userMessage() + QLatin1Char(' ')
                + std::get<AppError>(result).technicalDetails() : QString()));
    const AlignmentTaskOutput &output = std::get<AlignmentTaskOutput>(result);
    QCOMPARE(output.result.subtitles.size(), lines.size());
    QVERIFY(output.result.exportableSubtitles().size() > 100);
}

QTEST_GUILESS_MAIN(AsrTests)
#include "test_asr.moc"
