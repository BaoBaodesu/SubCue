#include "app_controller.h"
#include "application_context.h" // 依赖 ApplicationContext 析构声明
#include "asr/http_client.h"
#include "ai/ai_review_types.h"
#include "playback/audio_device.h"
#include "playback/audio_output.h"
#include "preview/preview_renderer.h"
#include "subtitle/subtitle.h"
#include "timeline_scene_item.h"
#include "video_preview_item.h"

#ifdef Q_OS_WIN
#include "platform/windows/d3d11_preview_adapter.h"
#include "platform/windows/wasapi_audio_device.h"
#endif

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <memory>

using namespace subcue;

namespace {

class RecordingHttpClient final : public IHttpClient {
public:
    int sendCount = 0;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &, const std::atomic<bool> *) override
    {
        ++sendCount;
        return AppError(ErrorDomain::Network, 1, QStringLiteral("unexpected AI HTTP"));
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &, QFile *, qint64, const std::atomic<bool> *,
        const std::function<void(qint64, qint64)> &) override
    {
        ++sendCount;
        return AppError(ErrorDomain::Network, 1, QStringLiteral("unexpected AI download"));
    }
};

} // namespace

class AppControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void audioOutputTakeFramesAdvancesClockBuffer();
    void virtualAudioDeviceConsumesElapsedAudio();
    void createAudioDeviceVirtualKind();
    void appControllerAcceptsSupportedPlaybackRates();
    void softwarePreviewStoresPresentedFrame();
    void previewFactoryCreatesIsolatedBackend();
    void d3d11AdapterFallsBackWithoutWindow();
    void videoPreviewItemPresentsWithoutWindow();
    void appControllerLoadsMediaAndScript();
    void appControllerImportsDocxFixture();
    void realFixtureCompletesAutomaticAlignmentWhenRequested();
    void unicodeMediaOpensFromDialogAndDrop();
    void projectFilesImportReopenAndRemove();
    void invalidImportPreservesCurrentMedia();
    void appControllerAppliesSubtitlesAndOverlayText();
    void currentSubtitlePrefersDocumentOrderOnOverlap();
    void timelinePlayheadDoesNotDirtyStaticGeometry();
    void appControllerSettingsRoundTrip();
    void unifiedAiKeySaveClearAndRollback();
    void saveSettingsDoesNotSendAiHttp();
    void appControllerPreflightReportsAllMissingItems();
    void appControllerExportsSrt();
    void appControllerPlayPauseSeek();
    void wasapiProbeDoesNotCrash();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
    [[nodiscard]] std::unique_ptr<ApplicationContext> makeContext(const QTemporaryDir &dir) const;
};

QString AppControllerTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

std::unique_ptr<ApplicationContext> AppControllerTests::makeContext(const QTemporaryDir &dir) const
{
    return std::make_unique<ApplicationContext>(
        dir.filePath(QStringLiteral("settings.json")),
        dir.filePath(QStringLiteral("credentials.dat")),
        AudioDeviceKind::Virtual);
}

void AppControllerTests::audioOutputTakeFramesAdvancesClockBuffer()
{
    AudioOutput output;
    QVERIFY(output.configure(48'000, 2));
    QVector<float> samples(4'800, 0.25f);
    QVERIFY(output.write(samples, MediaTime::fromMilliseconds(0), 0));
    QCOMPARE(output.bufferedSamples(), 2'400);

    const QVector<float> taken = output.takeFrames(1'200);
    QCOMPARE(taken.size(), 2'400);
    QCOMPARE(output.bufferedSamples(), 1'200);
    QCOMPARE(output.consumedSamples(), 1'200);
    QCOMPARE(output.consumeFrames(1'200), 1'200);
    QCOMPARE(output.bufferedSamples(), 0);
}

void AppControllerTests::virtualAudioDeviceConsumesElapsedAudio()
{
    VirtualAudioDevice device;
    AudioOutput output;
    QVERIFY(output.configure(48'000, 2));
    QVector<float> samples(48'000, 0.0f);
    QVERIFY(output.write(samples, MediaTime::fromMilliseconds(0), 0));
    QVERIFY(device.start(48'000, 2));
    device.setSampleProvider([&](float *destination, qint64 maximumFrames) -> qint64 {
        const QVector<float> taken = output.takeFrames(maximumFrames);
        std::copy(taken.cbegin(), taken.cend(), destination);
        return static_cast<qint64>(taken.size() / 2);
    });
    QTest::qWait(20);
    const qint64 consumed = device.renderFrame();
    QVERIFY(consumed > 0);
    QVERIFY(output.consumedSamples() > 0);
    device.pause();
    const qint64 afterPause = output.consumedSamples();
    QTest::qWait(20);
    QCOMPARE(device.renderFrame(), 0);
    QCOMPARE(output.consumedSamples(), afterPause);
    device.stop();
}

void AppControllerTests::createAudioDeviceVirtualKind()
{
    auto device = createAudioDevice(AudioDeviceKind::Virtual);
    QVERIFY(device);
    QCOMPARE(device->backendId(), QStringLiteral("virtual"));
    QVERIFY(!device->isHardware());
}

void AppControllerTests::appControllerAcceptsSupportedPlaybackRates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QList<double> rates{0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    for (double rate : rates) {
        controller.setPlaybackRate(rate);
        QCOMPARE(controller.playbackRate(), rate);
    }
    controller.setPlaybackRate(1.1);
    QCOMPARE(controller.playbackRate(), 3.0);
}

void AppControllerTests::softwarePreviewStoresPresentedFrame()
{
    SoftwarePreviewRenderer renderer;
    QCOMPARE(renderer.backendId(), QStringLiteral("software"));
    QVERIFY(!renderer.hasFrame());
    QImage image(16, 9, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    renderer.present(image, MediaTime::fromMilliseconds(40), 3);
    QVERIFY(renderer.hasFrame());
    QCOMPARE(renderer.lastPts().milliseconds(), 40);
    QCOMPARE(renderer.lastGeneration(), 3ULL);
    QCOMPARE(renderer.lastImage().pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(renderer.syncNode(nullptr, nullptr), nullptr);
}

void AppControllerTests::previewFactoryCreatesIsolatedBackend()
{
    auto renderer = PreviewRendererFactory::create(nullptr);
    QVERIFY(renderer);
#ifdef Q_OS_WIN
    QVERIFY(renderer->backendId() == QStringLiteral("d3d11-fallback")
            || renderer->backendId() == QStringLiteral("d3d11"));
#else
    QCOMPARE(renderer->backendId(), QStringLiteral("software"));
#endif
}

void AppControllerTests::d3d11AdapterFallsBackWithoutWindow()
{
#ifdef Q_OS_WIN
    D3d11PreviewAdapter adapter;
    QCOMPARE(adapter.backendId(), QStringLiteral("d3d11-fallback"));
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    adapter.present(image, MediaTime::fromMilliseconds(0), 1);
    QVERIFY(adapter.hasFrame());
    QCOMPARE(adapter.syncNode(nullptr, nullptr), nullptr);
    QCOMPARE(adapter.backendId(), QStringLiteral("d3d11-fallback"));
#else
    QSKIP("D3D11 adapter is Windows-only");
#endif
}

void AppControllerTests::videoPreviewItemPresentsWithoutWindow()
{
    VideoPreviewItem item;
    QVERIFY(!item.hasFrame());
    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(Qt::green);
    item.present(image, MediaTime::fromMilliseconds(16), 2);
    QVERIFY(item.hasFrame());
    QCOMPARE(item.lastImage().pixelColor(0, 0), QColor(Qt::green));
}

void AppControllerTests::appControllerLoadsMediaAndScript()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), false);
    context->settings.insert(QStringLiteral("outputAss"), false);
    AppController controller(context.get());
    QVERIFY(!controller.hasMedia());

    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(controller.hasMedia());
    QVERIFY(controller.hasVideo());
    QVERIFY(controller.durationMs() >= 900);
    QCOMPARE(controller.audioBackendId(), QStringLiteral("virtual"));

    const QString scriptPath = dir.filePath(QStringLiteral("script.txt"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("第一行\n第二行\n");
    script.close();
    controller.importScriptPath(scriptPath);
    QVERIFY(controller.scriptText().contains(QStringLiteral("第一行")));

    controller.startAlignment();
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.statusText().contains(QStringLiteral("前检查")));
}

void AppControllerTests::appControllerImportsDocxFixture()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QString path = mediaPath(QStringLiteral("script_sample.docx"));
    QVERIFY(controller.canImportFiles({QUrl::fromLocalFile(path)}));
    controller.importScriptPath(path);
    QVERIFY2(controller.scriptText().startsWith(QStringLiteral("Hello 各位观众朋友们好")),
        qPrintable(controller.statusText()));
    QVERIFY(controller.scriptText().split(QLatin1Char('\n')).size() >= 2);
}

void AppControllerTests::realFixtureCompletesAutomaticAlignmentWhenRequested()
{
    if (qEnvironmentVariableIsEmpty("SUBCUE_RUN_REAL_QWEN_FIXTURE")) {
        QSKIP("Real Qwen fixture requires an installed CUDA model");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    AppController controller(context.get());
    const QDir testDir(mediaPath(QStringLiteral("..")));
    const QString media = testDir.filePath(QStringLiteral("测试音频1.mp4"));
    const QString documentPath = testDir.filePath(QStringLiteral("测试文案1.docx"));
    if (!QFileInfo::exists(media) || !QFileInfo::exists(documentPath)) {
        QSKIP("Real Qwen fixture media is not present");
    }
    controller.loadMediaPath(media);
    controller.importScriptPath(documentPath);
    QVERIFY2(controller.hasMedia(), qPrintable(controller.statusText()));
    QVERIFY(controller.scriptText().startsWith(QStringLiteral("Hello 各位观众朋友们好")));
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 180'000);
    QVERIFY2(controller.statusText().startsWith(QStringLiteral("打轴完成")),
        qPrintable(controller.statusText()));
    QCOMPARE(controller.document()->count(), 138);
    QVERIFY(controller.canExport());
}

void AppControllerTests::unicodeMediaOpensFromDialogAndDrop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("中文 素材 #100% 测试.mp4"));
    QVERIFY(QFile::copy(mediaPath(QStringLiteral("cfr_av.mp4")), path));
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMedia(QUrl::fromLocalFile(path));
    QVERIFY2(controller.hasVideo(), qPrintable(controller.statusText()));
    QVERIFY(controller.playback().waitForDisplayedFrame(4'000));
    controller.handleDroppedUrl(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    QCOMPARE(controller.mediaPath(), path);
    QVERIFY(controller.playback().waitForDisplayedFrame(4'000));
}

void AppControllerTests::projectFilesImportReopenAndRemove()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QUrl video = QUrl::fromLocalFile(mediaPath(QStringLiteral("cfr_av.mp4")));
    const QUrl audio = QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")));
    QVERIFY(controller.canImportFiles({video, audio}));
    controller.importFiles({video, audio, video});
    QCOMPARE(controller.projectFiles().size(), 2);
    QVERIFY(controller.hasVideo());
    controller.openProjectFile(1);
    QVERIFY(!controller.hasVideo());
    QCOMPARE(controller.mediaPath(), audio.toLocalFile());
    controller.openProjectFile(0);
    QVERIFY(controller.hasVideo());
    controller.removeProjectFile(1);
    QCOMPARE(controller.projectFiles().size(), 1);
    QVERIFY(QFile::exists(audio.toLocalFile()));
    controller.openProjectFile(-1);
    controller.removeProjectFile(99);
    QCOMPARE(controller.projectFiles().size(), 1);
}

void AppControllerTests::invalidImportPreservesCurrentMedia()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(!controller.canImportFiles({QUrl(QStringLiteral("https://example.com/video.mp4"))}));
    QVERIFY(!controller.canImportFiles({QUrl::fromLocalFile(dir.path())}));
    QFile corrupt(dir.filePath(QStringLiteral("损坏.mp4")));
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("invalid media");
    corrupt.close();
    controller.importFiles({QUrl::fromLocalFile(corrupt.fileName())});
    QCOMPARE(controller.mediaPath(), mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(controller.playback().isOpen());
    QCOMPARE(controller.projectFiles().size(), 1);
    QVERIFY(controller.statusText().contains(QStringLiteral("未能导入")));
}

void AppControllerTests::appControllerAppliesSubtitlesAndOverlayText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());

    Subtitle first;
    first.text = QStringLiteral("Hello");
    first.start = MediaTime::fromMilliseconds(0);
    first.end = MediaTime::fromMilliseconds(1'000);
    first.status = QStringLiteral("MATCHED");
    Subtitle second;
    second.text = QStringLiteral("World");
    second.start = MediaTime::fromMilliseconds(1'200);
    second.end = MediaTime::fromMilliseconds(2'000);
    second.status = QStringLiteral("MATCHED");
    second.skipReason = QStringLiteral("");
    controller.applySubtitles({first, second});
    QCOMPARE(controller.document()->count(), 2);
    controller.seek(100);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("Hello"));
    controller.seek(1'500);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("World"));
    QCOMPARE(controller.formatTime(3'661'234), QStringLiteral("01:01:01.234"));
}

void AppControllerTests::currentSubtitlePrefersDocumentOrderOnOverlap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());

    Subtitle first;
    first.text = QStringLiteral("First");
    first.start = MediaTime::fromMilliseconds(1'000);
    first.end = MediaTime::fromMilliseconds(2'000);
    first.status = QStringLiteral("MATCHED");
    Subtitle second;
    second.text = QStringLiteral("Covering");
    second.start = MediaTime::fromMilliseconds(0);
    second.end = MediaTime::fromMilliseconds(3'000);
    second.status = QStringLiteral("MATCHED");
    controller.applySubtitles({first, second});
    controller.seek(100);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("Covering"));
    controller.seek(1'500);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("First"));
    controller.seek(2'000);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("Covering"));
    controller.seek(3'000);
    QCOMPARE(controller.currentSubtitleText(), QString());
}

void AppControllerTests::timelinePlayheadDoesNotDirtyStaticGeometry()
{
    TimelineSceneItem item;
    item.setDurationUs(10'000'000);
    item.setPixelsPerMs(0.2);
    item.consumePaintDirtyForTest();
    item.setPlayheadUs(1'000'000);
    QCOMPARE(int(item.pendingPaintDirty()), int(TimelineSceneItem::PaintPlayhead));
    QVERIFY((item.pendingPaintDirty() & TimelineSceneItem::PaintGeometry) == 0);
    item.consumePaintDirtyForTest();
    item.setScrollOffset(40);
    QVERIFY(item.pendingPaintDirty() & TimelineSceneItem::PaintGeometry);
}

void AppControllerTests::appControllerSettingsRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    QCOMPARE(controller.credentialStatus(), QStringLiteral("尚未配置"));
    QSignalSpy credentialSpy(&controller, &AppController::credentialStatusReady);
    QElapsedTimer responseTimer;
    responseTimer.start();
    QCOMPARE(controller.requestCredentialStatus(), QStringLiteral("正在查询…"));
    QVERIFY(responseTimer.elapsed() < 200);
    QTRY_COMPARE(credentialSpy.size(), 1);
    QCOMPARE(credentialSpy.first().at(1).toString(), QStringLiteral("尚未配置"));
    controller.saveSettings(
        {{QStringLiteral("fontFamily"), QStringLiteral("SimHei")},
         {QStringLiteral("fontSize1080p"), 64},
         {QStringLiteral("region"), QStringLiteral("singapore")}},
        QStringLiteral("test-key-not-for-log"));
    QCOMPARE(controller.subtitleFontFamily(), QStringLiteral("SimHei"));
    QCOMPARE(controller.subtitleFontSize(), 64);
    QCOMPARE(controller.setting(QStringLiteral("region")).toString(), QStringLiteral("singapore"));
    QCOMPARE(controller.credentialStatus(), QStringLiteral("已安全保存"));
    QVERIFY(!controller.statusText().contains(QStringLiteral("test-key-not-for-log")));
}

void AppControllerTests::unifiedAiKeySaveClearAndRollback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    QCOMPARE(controller.aiKeySource(), QStringLiteral("尚未配置"));
    QVERIFY(context->credentials.save(QStringLiteral("SubCue/AI/custom"), QStringLiteral("legacy-key")));
    context->settings.insert(QStringLiteral("legacyAiCredentialIds"),
        QJsonArray{QStringLiteral("custom")});
    QVERIFY(controller.saveSettings({}, {}, QStringLiteral("omni-secret-not-for-log")));
    QVERIFY(!context->credentials.exists(QStringLiteral("SubCue/AI/custom")));
    QCOMPARE(controller.credentialStatus(QStringLiteral("SubCue/AIReview/qwen_omni")),
             QStringLiteral("已安全保存"));
    QCOMPARE(controller.aiKeySource(), QStringLiteral("当前来源：已保存密钥"));
    QVERIFY(!controller.statusText().contains(QStringLiteral("omni-secret-not-for-log")));
    QVERIFY(!controller.setting(QStringLiteral("omniReviewEnabled")).isValid()
            || !controller.setting(QStringLiteral("omniReviewEnabled")).toBool());
    QVERIFY(!controller.setting(QStringLiteral("aiAssistEnabled")).isValid()
            || !controller.setting(QStringLiteral("aiAssistEnabled")).toBool());

    const QString settingsPath = dir.filePath(QStringLiteral("settings.json"));
    QVERIFY(QFile::remove(settingsPath));
    QVERIFY(QDir().mkdir(settingsPath));
    QVERIFY(!controller.saveSettings({}, {}, QStringLiteral("replacement-key")));
    QCOMPARE(context->credentials.load(QStringLiteral("SubCue/AIReview/qwen_omni")),
             QStringLiteral("omni-secret-not-for-log"));
    QVERIFY(QDir(settingsPath).removeRecursively());

    QVERIFY(controller.saveSettings({{QStringLiteral("clearAiApiKey"), true}}, {}, {}));
    QVERIFY(!context->credentials.exists(QStringLiteral("SubCue/AIReview/qwen_omni")));
    const QByteArray previous = qgetenv("DASHSCOPE_API_KEY");
    qputenv("DASHSCOPE_API_KEY", "env-after-clear");
    QCOMPARE(controller.aiKeySource({}, true),
             QStringLiteral("已清除应用保存的密钥，仍可使用环境变量 DASHSCOPE_API_KEY"));
    if (previous.isNull()) qunsetenv("DASHSCOPE_API_KEY");
    else qputenv("DASHSCOPE_API_KEY", previous);
}

void AppControllerTests::saveSettingsDoesNotSendAiHttp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    RecordingHttpClient http;
    context->aiHttp = &http;
    AppController controller(context.get());
    QCOMPARE(http.sendCount, 0);
    QVERIFY(controller.saveSettings({
        {QStringLiteral("fontFamily"), QStringLiteral("SimHei")},
        {QStringLiteral("clearAiApiKey"), false},
    }, {}, QStringLiteral("saved-ai-key")));
    QCOMPARE(http.sendCount, 0);
}

void AppControllerTests::appControllerPreflightReportsAllMissingItems()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), false);
    context->settings.insert(QStringLiteral("outputAss"), false);
    AppController controller(context.get());
    QSignalSpy spy(&controller, &AppController::alignmentPreflightFailed);

    controller.startAlignment();

    QCOMPARE(spy.size(), 1);
    const QVariantList issues = spy.takeFirst().at(0).toList();
    QStringList codes;
    for (const QVariant &issue : issues) {
        codes.append(issue.toMap().value(QStringLiteral("code")).toString());
    }
    QCOMPARE(codes, QStringList({
        QStringLiteral("media_missing"),
        QStringLiteral("script_missing"),
        QStringLiteral("output_format_missing"),
    }));
    QVERIFY(!controller.busy());
}

void AppControllerTests::appControllerExportsSrt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputAss"), false);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    AppController controller(context.get());

    Subtitle cue;
    cue.text = QStringLiteral("导出");
    cue.start = MediaTime::fromMilliseconds(0);
    cue.end = MediaTime::fromMilliseconds(1'000);
    cue.status = QStringLiteral("MANUAL");
    controller.applySubtitles({cue});
    controller.exportSubtitles();
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("subtitles_字幕文件/subtitles.srt"))));
    QVERIFY(controller.statusText().contains(QStringLiteral("导出完成")));
}

void AppControllerTests::appControllerPlayPauseSeek()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    QVERIFY(controller.hasMedia());
    QVERIFY(!controller.hasVideo());
    QVERIFY(!controller.playing());
    controller.playForward();
    QVERIFY(controller.playing());
    QTRY_VERIFY_WITH_TIMEOUT(controller.positionMs() > 0, 2'000);
    controller.stop();
    QVERIFY(!controller.playing());
    controller.seek(200);
    QCOMPARE(controller.positionMs(), 200);
    controller.seekUs(350'000);
    QCOMPARE(controller.positionUs(), 350'000);
}

void AppControllerTests::wasapiProbeDoesNotCrash()
{
#ifdef Q_OS_WIN
    WasapiAudioDevice device;
    QCOMPARE(device.backendId(), QStringLiteral("wasapi"));
    AppError error(ErrorDomain::Media, 0, QString());
    const bool available = device.probe(&error);
    if (available) {
        AppError startError(ErrorDomain::Media, 0, QString());
        const bool started = device.start(48'000, 2, &startError);
        if (started) {
            QVERIFY(device.isHardware());
            device.pause();
            device.resume();
            device.stop();
        }
    }
    QVERIFY(true);
#else
    QSKIP("WASAPI is Windows-only");
#endif
}

QTEST_MAIN(AppControllerTests)
#include "test_app_controller.moc"
