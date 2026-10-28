#include "inference/inference_manager.h"
#include "tts/local_tts_provider.h"
#include "tts/cloud_tts_provider.h"
#include "tts/audio_quality.h"
#include "project/project_store.h"
#include <thread>
#include <QtCore/QThread>
#include "asr/asr_provider_factory.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "asr/dashscope_asr_service.h"
#include "asr/http_client.h"
#include "asr/local_python_asr_service.h"
#include "alignment/alignment_pipeline.h"
#include "roughcut/script_document.h"
#include "settings/model_locator.h"
#include "settings/settings_manager.h"
#include "inference/inference_manager.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>

#include <optional>
#include <utility>
#include <variant>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef far
#undef near
#endif

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
    void modelLocatorPrefersSettingsThenEnv();
    void inferenceQueueCanBeCancelled();
    void inferenceBatchLeaseKeepsGpuUntilReleased();
    void inferenceWaitersAreFifoAndCanceledWaiterLeaves();
    void localTtsProviderRunsAndContainsMockFailures();
    void inferenceCancellationAndTimeoutStopChildProcessTree();
    void inferenceCancellationStopsCudaWorkerWhenRequested();
    void inferenceCancellationStopsRealTtsStagesWhenRequested();
    void qwenTextModeComparisonWhenRequested();
    void qwenDecodingProbeWhenRequested();
    void cloudTtsPricingAndPaidRequestProtection();
    void modelLocatorMigratesLegacyDirectories();
    void localModelsAtConfiguredRootAreReady();
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
    for (const auto &host : {"https://evil.example", "https://dashscope.aliyuncs.com.evil.example", "https://user@dashscope.aliyuncs.com", "https://dashscope.aliyuncs.com:444", "https://dashscope.aliyuncs.com?key=sentinel"}) {
        QVERIFY(DashScopeAsrService::endpointForApiHost(QString::fromLatin1(host), QStringLiteral("beijing")).isEmpty());
    }
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
        QStringLiteral("语音识别请求失败（HTTP 401）"));

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

void AsrTests::inferenceQueueCanBeCancelled()
{
    auto lease = InferenceManager::instance().acquire();
    std::atomic<bool> cancel = false;
    InferenceProcessResult result;
    std::thread waiting([&] { result = InferenceManager::instance().run(QStringLiteral("unused"), {}, {}, &cancel, 1000); });
    QThread::msleep(100);
    cancel = true;
    waiting.join();
    QVERIFY(result.cancelled);
    QVERIFY(!result.started);
}

void AsrTests::inferenceBatchLeaseKeepsGpuUntilReleased()
{
    auto &manager = InferenceManager::instance();
    auto batch = manager.acquire(nullptr, 2000, QStringLiteral("配音批次"), QStringLiteral("batch"));
    QVERIFY(batch);
    std::atomic<bool> cancel = false;
    std::atomic<bool> acquired = false;
    std::jthread asr([&] {
        auto lease = manager.acquire(&cancel, 5000, QStringLiteral("ASR"), QStringLiteral("asr"));
        if (lease) acquired = true;
    });
    const auto tasksReady = [&] { return manager.tasks().size() == 2; };
    QTRY_VERIFY(tasksReady());
    {
        auto nested = manager.acquire(nullptr, 2000, QStringLiteral("批次内部"));
        QVERIFY(nested);
        QCOMPARE(manager.tasks().first().toMap().value(QStringLiteral("id")).toString(), QStringLiteral("batch"));
    }
    QVERIFY(!acquired.load());
    batch.reset();
    asr.join();
    QVERIFY(acquired.load());
    QVERIFY(manager.tasks().isEmpty());
}

void AsrTests::inferenceWaitersAreFifoAndCanceledWaiterLeaves()
{
    auto &manager = InferenceManager::instance();
    auto owner = manager.acquire(nullptr, 2000, QStringLiteral("当前批次"), QStringLiteral("owner"));
    QVERIFY(owner);
    std::atomic<bool> cancel = false;
    std::atomic<bool> canceledAcquired = false;
    std::jthread canceled([&] {
        auto lease = manager.acquire(&cancel, 5000, QStringLiteral("将取消"), QStringLiteral("canceled"));
        canceledAcquired = lease.has_value();
    });
    QTRY_COMPARE(manager.tasks().size(), 2);
    QVector<QString> order;
    std::mutex orderMutex;
    std::jthread first([&] {
        auto lease = manager.acquire(nullptr, 5000, QStringLiteral("第一批"), QStringLiteral("first"));
        if (lease) { std::lock_guard lock(orderMutex); order.append(QStringLiteral("first")); }
    });
    QTRY_COMPARE(manager.tasks().size(), 3);
    std::jthread second([&] {
        auto lease = manager.acquire(nullptr, 5000, QStringLiteral("第二批"), QStringLiteral("second"));
        if (lease) { std::lock_guard lock(orderMutex); order.append(QStringLiteral("second")); }
    });
    QTRY_COMPARE(manager.tasks().size(), 4);
    cancel = true;
    canceled.join();
    QVERIFY(!canceledAcquired.load());
    QCOMPARE(manager.tasks().size(), 3);
    owner.reset();
    first.join(); second.join();
    QCOMPARE(order, QVector<QString>({QStringLiteral("first"), QStringLiteral("second")}));
    QVERIFY(manager.tasks().isEmpty());
}

void AsrTests::cloudTtsPricingAndPaidRequestProtection()
{
    class Http final : public IHttpClient {
    public:
        int sends = 0, downloads = 0, status = 200, downloadFailures = 0;
        bool unknown = false, malformed = false, expired = false, networkError = false;
        QByteArray audio; HttpRequest captured, capturedDownload;
        std::variant<HttpResponse, AppError> send(const HttpRequest &request, const std::atomic<bool> *) override {
            ++sends; captured = request;
            if (networkError) return AppError(ErrorDomain::Network, 1, QStringLiteral("模拟 TLS 或超时"));
            return HttpResponse{status, QByteArrayLiteral("application/json"), malformed ? QByteArrayLiteral("{") : QJsonDocument(QJsonObject{
                {QStringLiteral("request_id"), QStringLiteral("test-request")}, {QStringLiteral("usage"), unknown ? QJsonObject{} : QJsonObject{{QStringLiteral("input_tokens"), 100}, {QStringLiteral("output_tokens"), 1000}}},
                {QStringLiteral("output"), QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("stop")}, {QStringLiteral("audio"), QJsonObject{{QStringLiteral("url"), QStringLiteral("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/mock.wav")},
                    {QStringLiteral("expires_at"), expired ? 1 : 4102444800LL}}}}}}).toJson()};
        }
        std::variant<qint64, AppError> download(const HttpRequest &request, QFile *output, qint64, const std::atomic<bool> *, const std::function<void(qint64,qint64)> &) override {
            ++downloads; capturedDownload = request; if (!request.headers.empty()) return AppError(ErrorDomain::Network, 1, QStringLiteral("下载不应携带 API Key"));
            if (downloadFailures-- > 0) return AppError(ErrorDomain::Network, 1, QStringLiteral("模拟下载失败"));
            return output->write(audio);
        }
    } http;
    QFile fixture(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(fixture.open(QIODevice::ReadOnly)); http.audio = fixture.readAll();
    const auto price = TtsPricing::flashPrice();
    QCOMPARE(*TtsPricing::amount({{QStringLiteral("state"), QStringLiteral("known")}, {QStringLiteral("inputTokens"), 100}, {QStringLiteral("outputTokens"), 1000}}, price), 0.01215);
    QVERIFY(!TtsPricing::amount({{QStringLiteral("state"), QStringLiteral("unknown")}}, price));
    TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.input.projectId = QUuid::createUuid().toString(); job.input.segmentId = QUuid::createUuid().toString(); job.input.voiceVersionId = QUuid::createUuid().toString();
    job.input.projectGeneration = 1; job.input.provider = QStringLiteral("qwen-cloud"); job.input.model = QStringLiteral("qwen-audio-3.1-tts-flash"); job.input.modelVersion = QStringLiteral("http-wav24k-20261014");
    job.input.language = QStringLiteral("Chinese"); job.input.region = QStringLiteral("beijing"); job.input.providerVoiceId = QStringLiteral("test-voice"); job.input.spokenText = QStringLiteral("测试中文旁白。");
    job.billing = {{QStringLiteral("price"), price}};
    const auto nextJob = [&] { auto next = job; next.id = QUuid::createUuid().toString(QUuid::WithoutBraces); return next; };
    QTemporaryDir directory; QVector<TtsResult> candidates; QJsonObject usage;
    const auto run = [&](double budget, QVector<TtsJob> jobs, int timeoutMs = -1) { return CloudTtsProvider(QStringLiteral("mock-space"), QStringLiteral("test-key-not-real"), budget, &http).generate(jobs, directory.path(), nullptr,
        [&](const TtsResult &result) { candidates.append(result); }, timeoutMs, {}, [&](const QString &, const QJsonObject &record, const QJsonObject &) { usage = record; }); };
    http.downloadFailures = 1; auto report = run(0.1, {job}); QVERIFY2(report.success(), qPrintable(report.error)); QCOMPARE(http.sends, 1); QCOMPARE(http.downloads, 2); QCOMPARE(candidates.size(), 1);
    QVERIFY(!QJsonDocument::fromJson(http.captured.body).object().value(QStringLiteral("input")).toObject().contains(QStringLiteral("rate")));
    QVERIFY(!http.captured.followRedirects); QCOMPARE(http.captured.url.host(), QStringLiteral("mock-space.cn-beijing.maas.aliyuncs.com")); QVERIFY(!http.captured.body.contains("reference")); QCOMPARE(usage.value(QStringLiteral("yuan")).toDouble(), 0.01215);
    QCOMPARE(usage.value(QStringLiteral("inputTokens")).toInteger(), 100);
    QCOMPARE(usage.value(QStringLiteral("outputTokens")).toInteger(), 1000);
    QVERIFY(!usage.contains(QStringLiteral("remainingEstimatedYuan")));
    report = run(0.001, {nextJob(), nextJob()}); QVERIFY(!report.success()); QCOMPARE(http.sends, 2);
    QCOMPARE(report.results.size(), 1); QCOMPARE(usage.value(QStringLiteral("batchKnownYuan")).toDouble(), 0.01215);
    QVERIFY(report.error.contains(QStringLiteral("实际已知费用达到软预算")));
    http.unknown = true; report = run(0.1, {nextJob(), nextJob()}); QCOMPARE(http.sends, 3); QVERIFY(!report.success()); QCOMPARE(usage.value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    http.unknown = false; http.networkError = true; report = run(0.1, {nextJob(), nextJob()}); QCOMPARE(http.sends, 4); QCOMPARE(report.failures.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("outcome_unknown"));
    http.networkError = false; http.status = 429; report = run(0.1, {nextJob(), nextJob()}); QCOMPARE(http.sends, 5); QVERIFY(!report.success()); QCOMPARE(report.failures.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("http_error"));
    http.status = 200; http.expired = true; report = run(0.1, {nextJob()}); QCOMPARE(http.sends, 6); QCOMPARE(usage.value(QStringLiteral("state")).toString(), QStringLiteral("known")); QVERIFY(!report.success());
    http.expired = false; http.malformed = true; report = run(0.1, {nextJob()}); QCOMPARE(http.sends, 7); QCOMPARE(report.failures.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("outcome_unknown"));
    http.malformed = false;
    report = run(0.1, {nextJob(), nextJob()}); QVERIFY2(report.success(), qPrintable(report.error)); QCOMPARE(http.sends, 9);
    QCOMPARE(report.results.size(), 2); QCOMPARE(usage.value(QStringLiteral("batchKnownYuan")).toDouble(), 0.0243);
    http.malformed = false; http.expired = false; http.downloadFailures = 2;
    const auto sends = http.sends, downloads = http.downloads;
    report = run(0.1, {nextJob()}); QVERIFY(!report.success()); QCOMPARE(http.sends, sends + 1); QCOMPARE(http.downloads, downloads + 2);
    QCOMPARE(report.failures.first().toObject().value(QStringLiteral("stage")).toString(), QStringLiteral("download"));
    QCOMPARE(report.failures.first().toObject().value(QStringLiteral("networkCode")).toInt(), 1);
    QVERIFY(report.error.contains(QStringLiteral("模拟下载失败"))); QCOMPARE(usage.value(QStringLiteral("state")).toString(), QStringLiteral("known"));
    http.audio = QByteArrayLiteral("invalid audio fixture");
    report = run(0.1, {nextJob()}); QVERIFY(!report.success()); QCOMPARE(http.sends, sends + 2); QCOMPARE(http.downloads, downloads + 3);
    QCOMPARE(report.failures.first().toObject().value(QStringLiteral("stage")).toString(), QStringLiteral("decode"));
    QCOMPARE(report.failures.first().toObject().value(QStringLiteral("requestId")).toString(), QStringLiteral("test-request"));
    QCOMPARE(usage.value(QStringLiteral("state")).toString(), QStringLiteral("known"));
    QVERIFY(!report.error.contains(QStringLiteral("test-key-not-real"))); QVERIFY(!report.error.contains(QStringLiteral("https://")));
    if (!qEnvironmentVariable("SUBCUE_FLASH_WAV_FIXTURE").isEmpty()) {
        QFile raw(qEnvironmentVariable("SUBCUE_FLASH_WAV_FIXTURE")); QVERIFY(raw.open(QIODevice::ReadOnly)); http.audio = raw.readAll();
        report = run(0.1, {nextJob()}); QVERIFY2(report.success(), qPrintable(report.error));
        QFile repaired(report.results.first().audioPath); QVERIFY(repaired.open(QIODevice::ReadOnly));
        QCOMPARE(repaired.readAll().mid(44), http.audio.mid(44));
        const auto quality = AudioQuality::inspect(report.results.first().audioPath); QVERIFY(quality); QCOMPARE(quality->sampleRate, 24000);
        QCOMPARE(quality->sampleCount, qint64((http.audio.size() - 44) / 2));
        QVERIFY(QFile::copy(report.results.first().audioPath, QFileInfo(raw.fileName()).absoluteDir().filePath(QStringLiteral("repaired.wav"))));
    }
    QVERIFY(fixture.seek(0)); http.audio = fixture.readAll();
    for (const double speed : {0.5, 0.85, 1.0, 2.0}) {
        job.input.effectiveParameters = {{QStringLiteral("speedFactor"), speed}};
        report = run(0.1, {nextJob()}); QVERIFY2(report.success(), qPrintable(report.error));
        QCOMPARE(QJsonDocument::fromJson(http.captured.body).object().value(QStringLiteral("input")).toObject().value(QStringLiteral("rate")).toDouble(), speed);
    }
    const auto finalSends = http.sends;
    for (const auto &invalid : {QJsonValue(0.49), QJsonValue(2.01), QJsonValue(true), QJsonValue(QStringLiteral("0.85")), QJsonValue(QJsonValue::Null)}) {
        job.input.effectiveParameters = {{QStringLiteral("speedFactor"), invalid}};
        report = run(0.1, {nextJob()}); QVERIFY(!report.success()); QCOMPARE(http.sends, finalSends);
    }
    job.input.effectiveParameters = {};
    job.input.effectiveParameters.insert(QStringLiteral("seed"), 65536); report = run(0.1, {job}); QCOMPARE(http.sends, finalSends); QVERIFY(!report.success());
    job.input.effectiveParameters = {};
    for (const int timeoutMs : {120000, -1}) {
        report = run(0.1, {nextJob()}, timeoutMs); QVERIFY2(report.success(), qPrintable(report.error));
        QCOMPARE(http.captured.transferTimeoutMs, timeoutMs > 0 ? timeoutMs : 360000);
        QCOMPARE(http.capturedDownload.transferTimeoutMs, timeoutMs > 0 ? timeoutMs : 360000);
        QCOMPARE(http.captured.connectTimeoutMs, 15000); QCOMPARE(http.capturedDownload.connectTimeoutMs, 15000);
    }
}

void AsrTests::inferenceCancellationAndTimeoutStopChildProcessTree()
{
#ifdef Q_OS_WIN
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("独立 Python 不可用");
    for (const bool timeout : {false, true}) {
        std::atomic<bool> cancel{false}; HANDLE child = nullptr;
        const auto result = InferenceManager::instance().run(python, {QStringLiteral("-B"), QStringLiteral("-c"),
            QStringLiteral("import subprocess,sys,time; child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); print(child.pid,flush=True); time.sleep(60)")}, {}, &cancel, 2000,
            [&](const QByteArray &line) { child = OpenProcess(SYNCHRONIZE, FALSE, line.toULong()); if (!timeout) cancel = true; }, QStringLiteral("进程树取消验收"));
        QVERIFY(result.started); QVERIFY(child); const auto stopped = WaitForSingleObject(child, 0); CloseHandle(child);
        QCOMPARE(stopped, DWORD(WAIT_OBJECT_0)); QCOMPARE(result.cancelled, !timeout); QCOMPARE(result.timedOut, timeout);
        QVERIFY(InferenceManager::instance().tasks().isEmpty());
    }
#else
    QSKIP("Windows 进程树专项测试");
#endif
}

void AsrTests::inferenceCancellationStopsCudaWorkerWhenRequested()
{
#ifdef Q_OS_WIN
    const auto python = qEnvironmentVariable("SUBCUE_CUDA_CANCEL_PYTHON");
    if (python.isEmpty()) QSKIP("真实 CUDA 取消验收须显式提供独立 Python；普通回归不占用 GPU");
    QVERIFY(QFileInfo(python).isFile());
    std::atomic<bool> cancel{false}; HANDLE child = nullptr; QJsonObject observed;
    // 有界矩阵和固定批次，仅验证取消机制，不加载模型或将测试音当成真实推理。
    const auto result = InferenceManager::instance().run(python, {QStringLiteral("-B"), QStringLiteral("-c"), QString::fromUtf8(R"PY(
import json, os, torch
assert torch.cuda.is_available(), 'CUDA unavailable'
a = torch.ones((8192, 8192), device='cuda')
b = torch.empty_like(a)
torch.cuda.synchronize()
for _ in range(8):
    torch.mm(a, a, out=b)
done = torch.cuda.Event()
done.record()
print(json.dumps({'pid': os.getpid(), 'pending': not done.query(), 'allocatedBytes': torch.cuda.memory_allocated()}), flush=True)
while True:
    done.synchronize()
    for _ in range(8):
        torch.mm(a, a, out=b)
    done.record()
)PY")}, {}, &cancel, 60000, [&](const QByteArray &line) {
        observed = QJsonDocument::fromJson(line).object();
        if (!observed.contains(QStringLiteral("pid"))) return;
        child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(observed.value(QStringLiteral("pid")).toInteger()));
        cancel = true;
    }, QStringLiteral("CUDA 运算期间取消验收"));
    const auto stopped = child ? WaitForSingleObject(child, 0) : DWORD(WAIT_FAILED);
    if (child) CloseHandle(child);
    QVERIFY2(result.started, result.standardError.constData()); QVERIFY(child);
    QVERIFY2(observed.value(QStringLiteral("pending")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(observed).toJson())));
    QCOMPARE(stopped, DWORD(WAIT_OBJECT_0)); QVERIFY(result.cancelled); QVERIFY(!result.timedOut);
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    const auto recovered = InferenceManager::instance().run(python, {QStringLiteral("-B"), QStringLiteral("-c"),
        QStringLiteral("import torch; print(torch.ones(4, device='cuda').sum().item(), flush=True)")}, {}, nullptr, 60000);
    QVERIFY2(recovered.started && recovered.exitCode == 0, recovered.standardError.constData());
    QCOMPARE(recovered.standardOutput.trimmed(), QByteArray("4.0"));
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    qInfo().noquote() << "CUDA cancel evidence:" << QJsonDocument(observed).toJson(QJsonDocument::Compact);
#else
    QSKIP("Windows CUDA Worker 取消专项");
#endif
}

void AsrTests::inferenceCancellationStopsRealTtsStagesWhenRequested()
{
#ifdef Q_OS_WIN
    const auto requestPath = qEnvironmentVariable("SUBCUE_TTS_CANCEL_REQUEST");
    if (requestPath.isEmpty()) QSKIP("真实 TTS 阶段取消须显式提供诊断请求，普通回归不加载模型");
    const auto python = qEnvironmentVariable("SUBCUE_TTS_CANCEL_PYTHON");
    const auto worker = qEnvironmentVariable("SUBCUE_TTS_CANCEL_WORKER");
    QVERIFY(QFileInfo(python).isFile()); QVERIFY(QFileInfo(worker).isFile());
    QFile input(requestPath); QVERIFY(input.open(QIODevice::ReadOnly)); QVERIFY(input.size() < 2 * 1024 * 1024);
    const auto request = QJsonDocument::fromJson(input.readAll()).object();
    const auto provider = request.value(QStringLiteral("provider")).toString();
    QVERIFY(provider == QStringLiteral("gpt-local") || provider == QStringLiteral("qwen-local"));
    QCOMPARE(request.value(QStringLiteral("jobs")).toArray().size(), 1);
    QJsonArray evidence;
    for (const auto &phase : provider == QStringLiteral("qwen-local") ? QStringList{QStringLiteral("talker")}
        : QStringList{QStringLiteral("reference"), QStringLiteral("semantic")}) {
        QTemporaryDir output; QVERIFY(output.isValid());
        auto payload = request; payload.insert(QStringLiteral("outputDirectory"), output.path());
        std::atomic<bool> cancel{false}; HANDLE child = nullptr; QJsonObject observed; int results = 0;
        const auto process = InferenceManager::instance().run(python, {QStringLiteral("-B"), worker, phase},
            QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n', &cancel, 180000,
            [&](const QByteArray &line) {
                const auto event = QJsonDocument::fromJson(line).object();
                if (event.value(QStringLiteral("type")).toString() == QStringLiteral("result")) ++results;
                if (event.value(QStringLiteral("type")).toString() != QStringLiteral("model_phase") || child) return;
                observed = event;
                child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(event.value(QStringLiteral("pid")).toInteger()));
                cancel = true;
            }, QStringLiteral("真实 %1 %2 阶段取消验收").arg(provider, phase));
        const auto stopped = child ? WaitForSingleObject(child, 0) : DWORD(WAIT_FAILED);
        if (child) CloseHandle(child);
        QFile log(QDir(QFileInfo(requestPath).absolutePath()).filePath(phase + QStringLiteral("-stderr.txt")));
        QVERIFY(log.open(QIODevice::WriteOnly)); log.write(process.standardError); log.close();
        QVERIFY2(process.started, process.standardError.constData());
        QVERIFY2(child, process.standardError.constData()); QCOMPARE(stopped, DWORD(WAIT_OBJECT_0));
        QVERIFY(process.cancelled); QVERIFY(!process.timedOut); QCOMPARE(results, 0);
        QCOMPARE(observed.value(QStringLiteral("phase")).toString(), phase);
        QVERIFY(observed.value(QStringLiteral("allocatedBytes")).toInteger() > 0);
        QVERIFY(QDir(output.path()).entryList({QStringLiteral("*.wav"), QStringLiteral(".tts-*.part")}, QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(InferenceManager::instance().tasks().isEmpty());
        evidence.append(observed);
    }
    // 取消后通过同一 GPU 门禁重新加载生产 Worker，完成固定请求。
    const auto production = QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).absoluteFilePath(QStringLiteral("../../tools/tts_python_worker.py"));
    QVERIFY(QDir().mkpath(request.value(QStringLiteral("outputDirectory")).toString()));
    QJsonArray recovered;
    const auto recovery = InferenceManager::instance().run(python, {QStringLiteral("-B"), production},
        QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n', nullptr, 180000,
        [&](const QByteArray &line) { recovered.append(QJsonDocument::fromJson(line).object()); }, QStringLiteral("真实 %1 取消后恢复").arg(provider));
    QFile log(QDir(QFileInfo(requestPath).absolutePath()).filePath(QStringLiteral("recovery-stderr.txt")));
    QVERIFY(log.open(QIODevice::WriteOnly)); log.write(recovery.standardError); log.close();
    QVERIFY2(recovery.started && recovery.exitCode == 0 && !recovery.timedOut, recovery.standardError.constData());
    int results = 0;
    for (const auto &value : recovered) if (value.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("result")) {
        ++results;
        const auto quality = AudioQuality::inspect(value.toObject().value(QStringLiteral("audioPath")).toString());
        QVERIFY(quality); QVERIFY(quality->peak > 0); QCOMPARE(quality->channels, 1);
    }
    QCOMPARE(results, 1); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QFile audit(QDir(QFileInfo(requestPath).absolutePath()).filePath(QStringLiteral("stages-and-recovery.json")));
    QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("cancellations"), evidence}, {QStringLiteral("recovery"), recovered},
        {QStringLiteral("humanAccepted"), false}, {QStringLiteral("mock"), false}}).toJson());
#else
    QSKIP("Windows 真实 TTS 阶段取消专项");
#endif
}

void AsrTests::qwenTextModeComparisonWhenRequested()
{
    const auto root = qEnvironmentVariable("SUBCUE_QWEN_MODE_DIRECTORY");
    if (root.isEmpty()) QSKIP("Qwen 中文模式对照须显式指定诊断目录，普通回归不加载模型");
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe"); QVERIFY(QFileInfo(python).isFile());
    QJsonArray results;
    for (const bool completeText : {false, true}) {
        const auto mode = completeText ? QStringLiteral("true") : QStringLiteral("false");
        QFile input(QDir(root).filePath(mode + QStringLiteral("-request.json"))); QVERIFY(input.open(QIODevice::ReadOnly));
        const auto request = QJsonDocument::fromJson(input.readAll()).object(); QCOMPARE(request.value(QStringLiteral("provider")).toString(), QStringLiteral("qwen-local"));
        QCOMPARE(request.value(QStringLiteral("jobs")).toArray().size(), 1);
        const auto worker = completeText ? QDir(root).filePath(QStringLiteral("mode-worker/tts_python_worker.py"))
            : QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).absoluteFilePath(QStringLiteral("../../tools/tts_python_worker.py"));
        QJsonArray events;
        const auto process = InferenceManager::instance().run(python, {QStringLiteral("-B"), worker}, QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n', nullptr, 180000,
            [&](const QByteArray &line) { events.append(QJsonDocument::fromJson(line).object()); }, QStringLiteral("Qwen 中文完整文本模式对照 %1").arg(mode));
        QFile log(QDir(root).filePath(mode + QStringLiteral("-stderr.txt"))); QVERIFY(log.open(QIODevice::WriteOnly)); log.write(process.standardError); log.close();
        QFile protocol(QDir(root).filePath(mode + QStringLiteral("-events.json"))); QVERIFY(protocol.open(QIODevice::WriteOnly)); protocol.write(QJsonDocument(events).toJson()); protocol.close();
        QVERIFY2(process.started && process.exitCode == 0 && !process.timedOut, process.standardError.constData());
        int completed = 0;
        for (const auto &value : events) if (value.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("result")) {
            const auto event = value.toObject(); ++completed; QVERIFY(!event.value(QStringLiteral("mock")).toBool(true));
            QCOMPARE(event.value(QStringLiteral("inputHash")), request.value(QStringLiteral("jobs")).toArray()[0].toObject().value(QStringLiteral("inputHash")));
            QCOMPARE(event.value(QStringLiteral("diagnostics")).toObject().value(QStringLiteral("inferenceProfile")).toObject().value(QStringLiteral("nonStreamingMode")).toBool(), completeText);
            const auto quality = AudioQuality::inspect(event.value(QStringLiteral("audioPath")).toString()); QVERIFY(quality); QVERIFY(quality->peak > 0);
            results.append(QJsonObject{{QStringLiteral("nonStreamingMode"), completeText}, {QStringLiteral("result"), event}, {QStringLiteral("quality"), quality->toJson()}});
        }
        QCOMPARE(completed, 1); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    }
    QFile audit(QDir(root).filePath(QStringLiteral("comparison.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("results"), results}, {QStringLiteral("humanAccepted"), false}, {QStringLiteral("productionDefaultsChanged"), false}}).toJson());
}

void AsrTests::qwenDecodingProbeWhenRequested()
{
    const auto root = qEnvironmentVariable("SUBCUE_QWEN_DECODING_DIRECTORY");
    if (root.isEmpty()) QSKIP("Qwen 解码对照须显式指定诊断目录，普通回归不加载模型");
    QFile input(QDir(root).filePath(QStringLiteral("request.json"))); QVERIFY(input.open(QIODevice::ReadOnly));
    const auto request = QJsonDocument::fromJson(input.readAll()).object(); QCOMPARE(request.value(QStringLiteral("jobs")).toArray().size(), 1);
    QJsonArray events;
    const auto process = InferenceManager::instance().run(QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe"),
        {QStringLiteral("-B"), QDir(root).filePath(QStringLiteral("worker/tts_python_worker.py"))}, QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n', nullptr, 180000,
        [&](const QByteArray &line) { events.append(QJsonDocument::fromJson(line).object()); }, QStringLiteral("Qwen 主声码采样单项诊断"));
    QFile log(QDir(root).filePath(QStringLiteral("stderr.txt"))); QVERIFY(log.open(QIODevice::WriteOnly)); log.write(process.standardError); log.close();
    QFile protocol(QDir(root).filePath(QStringLiteral("events.json"))); QVERIFY(protocol.open(QIODevice::WriteOnly)); protocol.write(QJsonDocument(events).toJson()); protocol.close();
    QVERIFY2(process.started && process.exitCode == 0 && !process.timedOut, process.standardError.constData()); int completed = 0;
    for (const auto &value : events) if (value.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("result")) {
        const auto result = value.toObject(); ++completed; QVERIFY(!result.value(QStringLiteral("mock")).toBool(true));
        QCOMPARE(result.value(QStringLiteral("inputHash")), request.value(QStringLiteral("jobs")).toArray()[0].toObject().value(QStringLiteral("inputHash")));
        const auto profile = result.value(QStringLiteral("diagnostics")).toObject().value(QStringLiteral("inferenceProfile")).toObject();
        QCOMPARE(profile.value(QStringLiteral("talkerDoSample")), QJsonValue(false)); QCOMPARE(profile.value(QStringLiteral("nonStreamingMode")), QJsonValue(false));
        const auto quality = AudioQuality::inspect(result.value(QStringLiteral("audioPath")).toString()); QVERIFY(quality); QVERIFY(quality->peak > 0);
        QFile audit(QDir(root).filePath(QStringLiteral("audit.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
        audit.write(QJsonDocument(QJsonObject{{QStringLiteral("result"), result}, {QStringLiteral("quality"), quality->toJson()},
            {QStringLiteral("humanAccepted"), false}, {QStringLiteral("productionDefaultsChanged"), false}}).toJson());
    }
    QCOMPARE(completed, 1); QVERIFY(InferenceManager::instance().tasks().isEmpty());
}

void AsrTests::localTtsProviderRunsAndContainsMockFailures()
{
    const QString python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("独立 TTS 环境未配置，不能用其它引擎冒充");
    const QString worker = QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).absoluteFilePath(QStringLiteral("../../tools/tts_python_worker.py"));
    LocalTtsProvider provider(LocalTtsProvider::Kind::Mock, python, worker);
    QTemporaryDir output;
    TtsJob job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.voiceVersionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectGeneration = 1; job.input.spokenText = QStringLiteral("Mock 明确标记测试");
    job.input.provider = QStringLiteral("mock"); job.input.model = QStringLiteral("mock-tone");
    job.input.modelVersion = QStringLiteral("mock-v1"); job.input.language = QStringLiteral("Chinese");
    int callbacks = 0;
    ProjectStore store(output.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(store.create(job.input.projectId));
    ProjectStore::JobOptions options; options.provider = job.input.provider; options.inputHash = job.input.inputHash();
    QVERIFY(store.enqueue(job.id, job.input.snapshot(), options));
    int starts = 0;
    const auto result = provider.generate({job}, output.path(), nullptr, [&](const TtsResult &take) {
        ++callbacks; QVERIFY(take.mock); QCOMPARE(take.inputHash, job.input.inputHash());
        QVERIFY(store.recordJobResult(take.jobId, QJsonObject{{QStringLiteral("mock"), take.mock}}, take.usage));
        QVERIFY(store.finish(take.jobId, QUuid::createUuid().toString(QUuid::WithoutBraces), take.audioPath));
    }, 10000, [&](const QString &id) {
        ++starts; QCOMPARE(id, job.id);
        QVERIFY(store.transitionJob(id, ProjectStore::JobState::Queued, ProjectStore::JobState::Running));
    });
    QVERIFY2(result.success(), qPrintable(result.error));
    QCOMPARE(callbacks, 1); QCOMPARE(result.results.size(), 1);
    QCOMPARE(starts, 1); QCOMPARE(store.history().size(), 1);
    QCOMPARE(store.jobs().first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
    QCOMPARE(result.results.first().sampleCount, qint64(12000));
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation = QStringLiteral("oom");
    const auto failure = provider.generate({job}, output.path(), nullptr, {}, 10000);
    QVERIFY(!failure.success()); QCOMPARE(failure.failures.size(), 1);
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation = QStringLiteral("invalid_audio");
    auto next = job; next.id = QUuid::createUuid().toString(QUuid::WithoutBraces); next.simulation.clear();
    const auto partial = provider.generate({job, next}, output.path(), nullptr, {}, 10000);
    QVERIFY(!partial.success()); QCOMPARE(partial.failures.size(), 1); QCOMPARE(partial.results.size(), 1);
    next.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto callbackFailed = provider.generate({next}, output.path(), nullptr,
        [](const TtsResult &) { throw std::runtime_error("test submission failure"); }, 10000);
    QVERIFY(!callbackFailed.success()); QCOMPARE(callbackFailed.results.size(), 1);
    QVERIFY(QFileInfo::exists(callbackFailed.results.first().audioPath));
    QFile brokenWorker(output.filePath(QStringLiteral("broken_worker.py")));
    QVERIFY(brokenWorker.open(QIODevice::WriteOnly));
    brokenWorker.write("import time\nprint('not json', flush=True)\ntime.sleep(30)\n"); brokenWorker.close();
    LocalTtsProvider broken(LocalTtsProvider::Kind::Mock, python, brokenWorker.fileName());
    QElapsedTimer elapsed; elapsed.start();
    const auto protocolFailure = broken.generate({next}, output.path(), nullptr, {}, 10000);
    QVERIFY(!protocolFailure.success()); QVERIFY(!protocolFailure.canceled);
    QVERIFY(!protocolFailure.error.isEmpty()); QVERIFY(elapsed.elapsed() < 5000);
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation = QStringLiteral("crash");
    const auto crashed = provider.generate({job}, output.path(), nullptr, {}, 10000);
    QVERIFY(!crashed.success()); QVERIFY(!crashed.error.isEmpty());
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation = QStringLiteral("delay");
    std::atomic<bool> cancel = false;
    TtsBatchReport canceled;
    std::jthread running([&] { canceled = provider.generate({job}, output.path(), &cancel); });
    QTRY_VERIFY(!InferenceManager::instance().tasks().isEmpty());
    cancel = true; running.join();
    QVERIFY(canceled.canceled); QVERIFY(!canceled.success());
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QVERIFY(!QFileInfo(QDir(output.path()).filePath(job.id + QStringLiteral(".wav"))).exists());
}

void AsrTests::modelLocatorPrefersSettingsThenEnv()
{
    QTemporaryDir settingsRoot;
    QTemporaryDir envRoot;
    QVERIFY(settingsRoot.isValid());
    QVERIFY(envRoot.isValid());
    const QByteArray previous = qgetenv("SUBCUE_MODELS_ROOT");
    qputenv("SUBCUE_MODELS_ROOT", QFile::encodeName(envRoot.path()));
    const QString fromEnv = ModelLocator::root({});
    if (previous.isEmpty()) qunsetenv("SUBCUE_MODELS_ROOT");
    else qputenv("SUBCUE_MODELS_ROOT", previous);
    QCOMPARE(QDir::cleanPath(fromEnv), QDir::cleanPath(envRoot.path()));
    QJsonObject settings{{QStringLiteral("modelsRoot"), settingsRoot.path()}};
    QCOMPARE(QDir::cleanPath(ModelLocator::root(settings)), QDir::cleanPath(settingsRoot.path()));
    QCOMPARE(ModelLocator::directoryFor(ModelKind::Qwen3Asr, settings),
        QDir(settingsRoot.path()).filePath(QStringLiteral("qwen3-asr-0.6b")));

    qputenv("SUBCUE_MODELS_ROOT", QFile::encodeName(envRoot.path()));
    QJsonObject missing{{QStringLiteral("modelsRoot"),
        QDir(settingsRoot.path()).filePath(QStringLiteral("missing-models"))}};
    QCOMPARE(QDir::cleanPath(ModelLocator::root(missing)), missing.value(QStringLiteral("modelsRoot")).toString());
    QVERIFY(!QDir(ModelLocator::root(missing)).exists());
    if (previous.isEmpty()) qunsetenv("SUBCUE_MODELS_ROOT");
    else qputenv("SUBCUE_MODELS_ROOT", previous);
}

void AsrTests::modelLocatorMigratesLegacyDirectories()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacy = QDir(directory.path()).filePath(QStringLiteral("qwen3-asr-0.6b"));
    QJsonObject loaded{{QStringLiteral("qwen3AsrModelsDirectory"), legacy}};
    QCOMPARE(QDir::cleanPath(ModelLocator::derivedRootFromLegacy(loaded)),
        QDir::cleanPath(directory.path()));

    const QString settingsPath = directory.filePath(QStringLiteral("settings.json"));
    QFile file(settingsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 3},
        {QStringLiteral("qwen3AsrModelsDirectory"), legacy},
    }).toJson(QJsonDocument::Compact));
    file.close();
    const QJsonObject migrated = SettingsManager(settingsPath).load();
    QCOMPARE(QDir::cleanPath(migrated.value(QStringLiteral("modelsRoot")).toString()),
        QDir::cleanPath(directory.path()));
    QCOMPARE(migrated.value(QStringLiteral("qwen3AsrModelsDirectory")).toString(),
        QDir(directory.path()).filePath(QStringLiteral("qwen3-asr-0.6b")));
}

void AsrTests::localModelsAtConfiguredRootAreReady()
{
    const QString root = ModelLocator::compiledDefaultRoot();
    if (!QFileInfo::exists(root)) {
        QSKIP("Configured model root is not present on this machine");
    }
    QVERIFY2(ModelLocator::modelReady(ModelLocator::directoryFor(ModelKind::Qwen3Asr)),
        qPrintable(ModelLocator::directoryFor(ModelKind::Qwen3Asr)));
    QVERIFY2(ModelLocator::modelReady(ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner)),
        qPrintable(ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner)));
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
    QVector<QByteArray> lines;
    const auto bounded = InferenceManager::instance().run(QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
            QStringLiteral("[Console]::Out.Write('y' * 1048577); [Console]::Out.WriteLine(); [Console]::Out.Write('x' * 2000000); [Console]::Out.WriteLine(); [Console]::Out.WriteLine('valid'); [Console]::Error.Write('e' * 2000000)")},
        {}, nullptr, 10'000, [&lines](const QByteArray &line) { lines.append(line); });
    QVERIFY(bounded.started);
    QCOMPARE(bounded.exitCode, 0);
    QVERIFY(bounded.outputTruncated);
    QVERIFY(bounded.standardOutput.size() <= 65'536);
    QVERIFY(bounded.standardError.size() <= 65'536);
    QCOMPARE(lines, QVector<QByteArray>({QByteArray("valid")}));

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
    // 1 秒 44.1 kHz 素材完整重采样后必须保留 16000 个样本，包括尾部滤波延迟。
    QCOMPARE(std::get<QVector<float>>(pcm).size(), qsizetype(16'000));

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
    lastRequest.clear(); get.maxResponseBytes = 4;
    QVERIFY(std::holds_alternative<AppError>(client.send(get)));
    lastRequest.clear(); get.maxResponseBytes = 0; get.maxDownloadBytes = 4;
    QFile limited(directory.filePath(QStringLiteral("limited.bin"))); QVERIFY(limited.open(QIODevice::WriteOnly));
    QVERIFY(std::holds_alternative<AppError>(client.download(get, &limited))); QVERIFY(limited.size() <= 4);
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
