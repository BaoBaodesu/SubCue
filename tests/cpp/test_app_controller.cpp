#include "cloud_settings_controller.h"
#include "app_controller.h"
#include "application_context.h" // 依赖 ApplicationContext 析构声明
#include "asr/http_client.h"
#include "ai/ai_review_types.h"
#include "playback/audio_device.h"
#include "playback/audio_output.h"
#include "preview/preview_renderer.h"
#include "project/project_serializer.h"
#include "subtitle/subtitle.h"
#include "subtitle/subtitle_model.h"
#include "timeline_scene_item.h"
#include "video_preview_item.h"

#ifdef Q_OS_WIN
#include "platform/windows/d3d11_preview_adapter.h"
#include "platform/windows/wasapi_audio_device.h"
#endif

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QScopeGuard>
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
    void cleanup();
    void credentialCleanupPreservesOtherScope();
    void audioOutputTakeFramesAdvancesClockBuffer();
    void virtualAudioDeviceConsumesElapsedAudio();
    void createAudioDeviceVirtualKind();
    void appControllerAcceptsSupportedPlaybackRates();
    void softwarePreviewStoresPresentedFrame();
    void previewFactoryCreatesIsolatedBackend();
    void d3d11AdapterFallsBackWithoutWindow();
    void videoPreviewItemPresentsWithoutWindow();
    void appControllerLoadsMediaAndScript();
    void importOrderPreservesProjectAndSavePath();
    void appControllerImportsDocxFixture();
    void realFixtureCompletesAutomaticAlignmentWhenRequested();
    void unicodeMediaOpensFromDialogAndDrop();
    void projectFilesImportReopenAndRemove();
    void invalidImportPreservesCurrentMedia();
    void appControllerAppliesSubtitlesAndOverlayText();
    void currentSubtitleShowsAllOverlapsInDocumentOrder();
    void timelinePlayheadDoesNotDirtyStaticGeometry();
    void atomicCueEditsOverlapAndStableFiltering();
    void timelineVisibilityScalesWithViewport();
    void appControllerSettingsRoundTrip();
    void settingsRoundTripIncludesModelsRoot();
    void unifiedAiKeySaveClearAndRollback();
    void cloudDeletionWaitsForOwner();
    void saveSettingsDoesNotSendAiHttp();
    void settingsOnlyStyleChangesDirtyProject();
    void defaultsAndVoiceCredentialsStayScoped();
    void cloudConnectionsPersistAndRollback();
    void cloudSettingsDraftAndDiscovery();
    void cloudSwitchConflictAndCapability();
    void connectionTestsPreserveRequestIdentity();
    void appControllerPreflightReportsAllMissingItems();
    void appControllerExportsSrt();
    void draftProjectRoundTripAndFailedOpenPreservesEdits();
    void appControllerPlayPauseSeek();
    void wasapiProbeDoesNotCrash();
    void navigatePendingAndMismatchCues();
    void multilinePreviewFlagAndExportPreservesModified();
    void subtitleTimecodeShortcutsKeepDocumentInSync();
    void relinkMediaRejectsDifferentContent();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
    [[nodiscard]] std::unique_ptr<ApplicationContext> makeContext(const QTemporaryDir &dir);
    QStringList credentialPaths_;
};

QString AppControllerTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

void AppControllerTests::cleanup()
{
    // 只释放本用例临时路径下的隔离凭据，正式账号命名空间不受影响。
    for (const auto &path : credentialPaths_) {
        DpapiCredentialStore keys(path); QString error;
        for (const auto &prefix : {"SubCue/Cloud/", "SubCue/ASR/", "SubCue/AIReview/", "SubCue/AI/", "SubCue/VoiceStudio/"}) {
            const auto refs = keys.list(QString::fromLatin1(prefix), &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            for (const auto &ref : refs) QVERIFY2(keys.remove(ref, &error), qPrintable(error));
            QVERIFY2(keys.list(QString::fromLatin1(prefix), &error).isEmpty() && error.isEmpty(), qPrintable(error));
        }
    }
    credentialPaths_.clear();
}

void AppControllerTests::credentialCleanupPreservesOtherScope()
{
    QTemporaryDir directory, otherDirectory; QString error;
    const auto ref = QStringLiteral("SubCue/Cloud/cleanup-test/version");
    DpapiCredentialStore other(otherDirectory.filePath(QStringLiteral("credentials.dat")));
    QVERIFY2(other.save(ref, QStringLiteral("other-test-only"), &error), qPrintable(error));
    const auto removeOther = qScopeGuard([&] { (void)other.remove(ref); });
    {
        auto context = makeContext(directory);
        QVERIFY2(context->credentials.save(ref, QStringLiteral("owned-test-only"), &error), qPrintable(error));
    }
    cleanup();
    QVERIFY(credentialPaths_.isEmpty());
    QVERIFY(!DpapiCredentialStore(directory.filePath(QStringLiteral("credentials.dat"))).exists(ref));
    QVERIFY(other.exists(ref));
}

std::unique_ptr<ApplicationContext> AppControllerTests::makeContext(const QTemporaryDir &dir)
{
    credentialPaths_.append(dir.filePath(QStringLiteral("credentials.dat")));
    return std::make_unique<ApplicationContext>(
        dir.filePath(QStringLiteral("settings.json")),
        dir.filePath(QStringLiteral("credentials.dat")),
        AudioDeviceKind::Virtual);
}

void AppControllerTests::draftProjectRoundTripAndFailedOpenPreservesEdits()
{
    QTemporaryDir dir;
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.setScriptText(QStringLiteral("手改文稿\n第二行"));
    QVERIFY(controller.modified());
    const auto path = dir.filePath(QStringLiteral("草稿.subcue"));
    QSignalSpy saved(&controller, &AppController::projectSaveFinished);
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 8000);
    QVERIFY(saved.at(0).at(0).toBool());
    QVERIFY(!controller.modified());
    controller.newProject();
    QSignalSpy opened(&controller, &AppController::projectOpenFinished);
    controller.openProject(QUrl::fromLocalFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 8000);
    QVERIFY(opened.at(0).at(0).toBool());
    QCOMPARE(controller.scriptText(), QStringLiteral("手改文稿\n第二行"));
    QVERIFY(!controller.hasMedia());
    QVERIFY(!controller.modified());
    controller.setScriptText(QStringLiteral("保留当前编辑"));
    controller.openProject(QUrl::fromLocalFile(dir.filePath(QStringLiteral("不存在.subcue"))));
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 8000);
    QVERIFY(!opened.at(1).at(0).toBool());
    QCOMPARE(controller.scriptText(), QStringLiteral("保留当前编辑"));
    QVERIFY(controller.modified());
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

void AppControllerTests::importOrderPreservesProjectAndSavePath()
{
    for (const bool mediaFirst : {false, true}) {
        QTemporaryDir dir;
        auto context = makeContext(dir);
        AppController controller(context.get());
        const auto path = dir.filePath(QStringLiteral("导入顺序.subcue"));
        QSignalSpy saved(&controller, &AppController::projectSaveFinished);
        QVERIFY(controller.saveProject(QUrl::fromLocalFile(path)));
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(saved.last().first().toBool());
        if (mediaFirst) controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
        controller.setScriptText(QStringLiteral("第一行\n第二行"));
        if (!mediaFirst) {
            Subtitle cue; cue.id = QStringLiteral("before-media"); cue.text = QStringLiteral("已导入的字幕");
            cue.start = MediaTime::fromMilliseconds(100); cue.end = MediaTime::fromMilliseconds(500);
            controller.applySubtitles({cue});
            controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
            QCOMPARE(controller.subtitleModel()->property("count").toInt(), 1);
        }
        QCOMPARE(controller.projectPath(), path);
        QCOMPARE(controller.scriptText(), QStringLiteral("第一行\n第二行"));
        QVERIFY(controller.saveCurrentProject());
        QTRY_COMPARE(saved.count(), 2);
        QVERIFY(saved.last().first().toBool());
        QVERIFY(!controller.modified());
        const auto stored = ProjectSerializer::load(path);
        QVERIFY(stored);
        QCOMPARE(stored->state.value(QStringLiteral("scriptText")).toString(), controller.scriptText());
        QCOMPARE(stored->mediaPath, controller.mediaPath());
        controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
        QCOMPARE(controller.projectPath(), path);
        QVERIFY(controller.saveCurrentProject());
        QTRY_COMPARE(saved.count(), 3);
        QVERIFY(saved.last().first().toBool());
    }
}

void AppControllerTests::appControllerImportsDocxFixture()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    if (!QFileInfo::exists(appDir.filePath(QStringLiteral("SubCueInference.exe")))
        && !QFileInfo::exists(appDir.filePath(QStringLiteral("inference/SubCueInference.exe")))) {
        QSKIP("CPU builds omit the inference worker used to parse DOCX scripts");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QString path = mediaPath(QStringLiteral("script_sample.docx"));
    QVERIFY(controller.canImportFiles({QUrl::fromLocalFile(path)}));
    controller.importScriptPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 15000);
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
    QVERIFY(controller.validateImportFiles({video, audio}));
    QVERIFY(controller.projectFiles().isEmpty());
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
    QVERIFY(!controller.validateImportFiles({QUrl::fromLocalFile(corrupt.fileName())}));
    QCOMPARE(controller.projectFiles().size(), 1);
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

void AppControllerTests::currentSubtitleShowsAllOverlapsInDocumentOrder()
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
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("First\nCovering"));
    controller.seek(2'000);
    QCOMPARE(controller.currentSubtitleText(), QStringLiteral("Covering"));
    controller.seek(3'000);
    QCOMPARE(controller.currentSubtitleText(), QString());
}

void AppControllerTests::atomicCueEditsOverlapAndStableFiltering()
{
    QTemporaryDir dir;
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY(!controller.busy());
    Subtitle first;
    first.id = QStringLiteral("a"); first.text = QStringLiteral("第一条");
    first.start = MediaTime::fromMicroseconds(100'001); first.end = MediaTime::fromMicroseconds(400'001);
    first.status = QStringLiteral("MANUAL");
    Subtitle second = first;
    second.id = QStringLiteral("b"); second.text = QStringLiteral("第二条");
    second.start = MediaTime::fromMicroseconds(400'001); second.end = MediaTime::fromMicroseconds(1'100'001);
    controller.applySubtitles({first, second});
    QVERIFY(controller.applyCueEdit(QStringLiteral("a"), QStringLiteral("手改"), -1, -1));
    QCOMPARE(qobject_cast<SubtitleModel *>(controller.subtitleModel())->get(0).value(QStringLiteral("startUs")).toLongLong(), qint64{100'001});
    controller.undo();
    QCOMPARE(qobject_cast<SubtitleModel *>(controller.subtitleModel())->get(0).value(QStringLiteral("text")).toString(), first.text);
    QVERIFY(controller.applyCueEdit(QStringLiteral("a"), QStringLiteral("重叠"), 300'001, 700'001));
    QCOMPARE(controller.overlappingCueCount(), 2);
    QCOMPARE(controller.overlapCount(), 1);
    QVERIFY(!controller.canExport());
    QCOMPARE(qobject_cast<SubtitleModel *>(controller.subtitleModel())->get(1).value(QStringLiteral("startUs")).toLongLong(), qint64{400'001});
    qobject_cast<SubtitleFilterModel *>(controller.filteredSubtitleModel())->setSearchText(QStringLiteral("第二"));
    QCOMPARE(qobject_cast<SubtitleFilterModel *>(controller.filteredSubtitleModel())->get(0).value(QStringLiteral("id")).toString(), QStringLiteral("b"));
    QVERIFY(controller.applyCueEdit(QStringLiteral("b"), QStringLiteral("第二条修正"), -1, -1));
    QCOMPARE(qobject_cast<SubtitleModel *>(controller.subtitleModel())->get(0).value(QStringLiteral("text")).toString(), QStringLiteral("重叠"));
    QVERIFY(!controller.applyCueEdit(QStringLiteral("失效"), QStringLiteral("错误"), -1, -1));
    QSignalSpy saved(&controller, &AppController::projectSaveFinished);
    const QString path = dir.filePath(QStringLiteral("overlap.subcue"));
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY(saved.first().first().toBool());
    controller.openProject(QUrl::fromLocalFile(path));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.overlapCount(), 1);
    QCOMPARE(controller.overlappingCueCount(), 2);
    QSignalSpy exported(&controller, &AppController::exportFinished);
    controller.exportSubtitles(dir.path());
    QCOMPARE(exported.count(), 1);
    QVERIFY(!exported.first().first().toBool());
}

void AppControllerTests::timelineVisibilityScalesWithViewport()
{
    TimelineSceneItem item;
    item.setWidth(1000); item.setHeight(300); item.setDurationUs(1'100'000'000);
    item.setView(1.0, 50'000);
    for (const int count : {1000, 10000}) {
        QList<Subtitle> cues;
        for (int row = 0; row < count; ++row) {
            Subtitle cue;
            cue.id = QString::number(row); cue.text = QStringLiteral("性能样本"); cue.status = QStringLiteral("MATCHED");
            cue.start = MediaTime::fromMicroseconds(row * qint64{100'000});
            cue.end = MediaTime::fromMicroseconds(row * qint64{100'000} + 50'000);
            cues.append(cue);
        }
        item.setSubtitles(cues);
        QVERIFY(item.visibleCueCount() >= 10 && item.visibleCueCount() <= 12);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int sample = 0; sample < 2000; ++sample) QVERIFY(item.visibleCueCount() <= 12);
        qInfo() << "viewport_query_cues=" << count << "iterations=2000 elapsed_ns=" << elapsed.nsecsElapsed();
        for (Subtitle &cue : cues) { cue.start = MediaTime::fromMicroseconds(0); cue.end = MediaTime::fromMicroseconds(1'000'000'000); }
        item.setSubtitles(cues);
        QVERIFY(item.visibleCueCount() > 0 && item.visibleCueCount() <= 4);
        item.setSubtitleScrollOffset(item.subtitleContentHeight() - item.subtitleViewportHeight());
        QVERIFY(item.visibleCueCount() > 0 && item.visibleCueCount() <= 4);
    }
    item.consumePaintDirtyForTest();
    item.setSelectedCueId(QStringLiteral("9999"));
    QVERIFY(!(item.pendingPaintDirty() & TimelineSceneItem::PaintGeometry));
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
    QTemporaryDir dir; auto context = makeContext(dir); AppController controller(context.get());
    QVERIFY(!controller.saveSettings({}, QStringLiteral("test-key-not-for-log")));
    QVERIFY(!context->credentials.exists(QStringLiteral("SubCue/ASR/dashscope")));
    QVERIFY(!controller.statusText().contains(QStringLiteral("test-key-not-for-log")));
    QVERIFY(controller.saveSettings({{QStringLiteral("fontFamily"), QStringLiteral("SimHei")}, {QStringLiteral("fontSize1080p"), 64}}));
    QCOMPARE(controller.subtitleFontFamily(), QStringLiteral("SimHei")); QCOMPARE(controller.subtitleFontSize(), 64);
}

void AppControllerTests::settingsRoundTripIncludesModelsRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QString root = dir.filePath(QStringLiteral("models-root"));
    QVERIFY(controller.saveSettings({{QStringLiteral("modelsRoot"), root}}));
    QCOMPARE(controller.setting(QStringLiteral("modelsRoot")).toString(), root);
    QCOMPARE(controller.setting(QStringLiteral("qwen3AsrModelsDirectory")).toString(),
        QDir(root).filePath(QStringLiteral("qwen3-asr-0.6b")));
}

void AppControllerTests::unifiedAiKeySaveClearAndRollback()
{
    QTemporaryDir dir; auto context = makeContext(dir); AppController controller(context.get());
    CloudSettingsController cloud(context.get()); cloud.begin();
    const auto account = cloud.addAccount(QStringLiteral("bailian"));
    QVERIFY(cloud.stageKey(account, QStringLiteral("unified-secret"))); QVERIFY(cloud.save());
    QCOMPARE(context->cloudConfig->resolveKey(account), QStringLiteral("unified-secret"));
    QVERIFY(!controller.saveSettings({}, {}, QStringLiteral("legacy-must-not-save")));
    QVERIFY(!context->credentials.exists(QStringLiteral("SubCue/AIReview/qwen_omni")));
    QCOMPARE(context->cloudConfig->resolveKey(account), QStringLiteral("unified-secret"));
    QVERIFY(!controller.saveSettings({{QStringLiteral("clearAiApiKey"), true}}));
    QCOMPARE(context->cloudConfig->resolveKey(account), QStringLiteral("unified-secret"));
    cloud.begin(); cloud.removeKey(account); QVERIFY(cloud.save());
    QVERIFY(context->cloudConfig->resolveKey(account).isEmpty());
}

void AppControllerTests::cloudDeletionWaitsForOwner()
{
    QTemporaryDir directory; auto context = makeContext(directory); CloudSettingsController cloud(context.get()); cloud.begin();
    const auto account = cloud.addAccount(QStringLiteral("bailian")); QVERIFY(cloud.stageKey(account, QStringLiteral("old-key"))); QVERIFY(cloud.save());
    QObject owner, unrelated, intruder; bool stopped = false, unrelatedStopped = false, blocked = false, existedBeforeStop = false;
    QVERIFY(context->trackCloudTask(&owner, account, [&] {
        existedBeforeStop = !context->cloudConfig->resolveKey(account).isEmpty();
        blocked = !context->trackCloudTask(&intruder, account, [] {});
        stopped = true; context->clearCloudTask(&owner);
    }));
    QVERIFY(context->trackCloudTask(&unrelated, QStringLiteral("unrelated"), [&] { unrelatedStopped = true; }));
    QVERIFY(cloud.stageKey(account, QStringLiteral("rotated-key"))); QVERIFY(cloud.save()); QVERIFY(!stopped);
    cloud.removeKey(account); QVERIFY(cloud.save()); QVERIFY(stopped); QVERIFY(blocked); QVERIFY(existedBeforeStop); QVERIFY(!unrelatedStopped);
    QVERIFY(context->cloudConfig->resolveKey(account).isEmpty());
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
    }, {}, {}));
    QCOMPARE(http.sendCount, 0);
}

void AppControllerTests::cloudConnectionsPersistAndRollback()
{
    QTemporaryDir dir; auto context = makeContext(dir); AppController controller(context.get());
    const auto before = context->cloudConfig->load();
    QVERIFY(!controller.saveSettings({{QStringLiteral("ttsConnections"), QVariantList{QVariantMap{{QStringLiteral("workspace"), QStringLiteral("old-workspace")}}}}}));
    QVERIFY(!controller.saveSettings({{QStringLiteral("ttsCloudWorkspace"), QStringLiteral("old-workspace")}}));
    QCOMPARE(context->cloudConfig->load(), before);
    QVERIFY(controller.saveSettings({{QStringLiteral("fontFamily"), QStringLiteral("SimHei")}}));
    QVERIFY(context->settingsManager.load().value(QStringLiteral("ttsConnections")).toArray().isEmpty());
}

void AppControllerTests::defaultsAndVoiceCredentialsStayScoped()
{
    QTemporaryDir dir; auto context = makeContext(dir); AppController controller(context.get());
    const auto original = controller.setting(QStringLiteral("fontFamily"));
    const auto reference = QStringLiteral("SubCue/VoiceStudio/SettingsTest");
    QVERIFY(!controller.saveSettings({{QStringLiteral("voiceApiKey"), QStringLiteral("private-voice-settings-key")}}));
    QVERIFY(controller.saveSettings({{QStringLiteral("defaultsOnly"), true}, {QStringLiteral("fontFamily"), QStringLiteral("SimHei")}}));
    QVERIFY(!controller.modified()); QCOMPARE(controller.setting(QStringLiteral("fontFamily")), original);
    QCOMPARE(controller.defaultSetting(QStringLiteral("fontFamily")).toString(), QStringLiteral("SimHei"));
    QVERIFY(!context->credentials.exists(reference));
    QFile settings(context->settingsManager.path()); QVERIFY(settings.open(QIODevice::ReadOnly));
    QVERIFY(!settings.readAll().contains("private-voice-settings-key")); settings.close();
    QVERIFY(!context->settings.contains(QStringLiteral("voiceApiKey")));
    QVERIFY(!controller.saveSettings({{QStringLiteral("clearVoiceApiKey"), true}}));
    QVERIFY(context->credentials.load(reference).isEmpty()); QVERIFY(!controller.modified());
    QVERIFY(!controller.applySubtitleStyle({{QStringLiteral("fontSize1080p"), 0}}));
    QVERIFY(!controller.modified());
    QVERIFY(controller.applySubtitleStyle({{QStringLiteral("fontFamily"), QStringLiteral("KaiTi")}, {QStringLiteral("fontSize1080p"), 64}}));
    QVERIFY(controller.modified()); QCOMPARE(controller.subtitleFontFamily(), QStringLiteral("KaiTi"));
    QCOMPARE(controller.defaultSetting(QStringLiteral("fontFamily")).toString(), QStringLiteral("SimHei"));
}

void AppControllerTests::settingsOnlyStyleChangesDirtyProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    QVERIFY(!controller.modified());
    QVERIFY(controller.saveSettings({{QStringLiteral("region"), QStringLiteral("singapore")}},
        {}, {}));
    QVERIFY(!controller.modified());
    QVERIFY(controller.saveSettings({{QStringLiteral("fontFamily"), controller.setting(QStringLiteral("fontFamily"))}}));
    QVERIFY(!controller.modified());
    QVERIFY(controller.saveSettings({{QStringLiteral("fontFamily"), QStringLiteral("SimHei")}}));
    QVERIFY(controller.modified());

    QSignalSpy saved(&controller, &AppController::projectSaveFinished);
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(dir.filePath(QStringLiteral("style.subcue")))));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY(saved.first().first().toBool());
    QVERIFY(!controller.modified());
    QVERIFY(controller.saveSettings({{QStringLiteral("fontFamily"), QStringLiteral("SimHei")},
        {QStringLiteral("outputSrt"), false}}));
    QVERIFY(!controller.modified());
    QVERIFY(controller.saveSettings({{QStringLiteral("fontSize1080p"), 64}}));
    QVERIFY(controller.modified());
}

void AppControllerTests::connectionTestsPreserveRequestIdentity()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    RecordingHttpClient http;
    context->aiHttp = &http;
    AppController controller(context.get());
    QSignalSpy ai(&controller, &AppController::aiConnectionTestFinished);
    QSignalSpy asr(&controller, &AppController::asrConnectionTestFinished);
    const int first = controller.nextSettingsRequestId();
    const int second = controller.nextSettingsRequestId();
    QVERIFY(second > first);
    controller.testAiConnection(QStringLiteral("test-key"), first);
    controller.testAsrConnection({{QStringLiteral("asrProvider"), QStringLiteral("dashscope")}}, {}, second);
    QTRY_COMPARE(ai.count(), 1);
    QCOMPARE(asr.count(), 1);
    QCOMPARE(ai.first().first().toMap().value(QStringLiteral("requestId")).toInt(), first);
    QCOMPARE(asr.first().first().toMap().value(QStringLiteral("requestId")).toInt(), second);
    QVERIFY(!ai.first().first().toMap().value(QStringLiteral("success")).toBool());
    QVERIFY(!asr.first().first().toMap().value(QStringLiteral("success")).toBool());

    QVERIFY(!controller.saveSettings({}, {}, QStringLiteral("saved-key")));
    const QByteArray environmentKey = qgetenv("DASHSCOPE_API_KEY");
    qunsetenv("DASHSCOPE_API_KEY");
    controller.testAiConnection({}, second, true);
    if (environmentKey.isNull()) qunsetenv("DASHSCOPE_API_KEY");
    else qputenv("DASHSCOPE_API_KEY", environmentKey);
    QCOMPARE(ai.count(), 2);
    QCOMPARE(ai.last().first().toMap().value(QStringLiteral("requestId")).toInt(), second);
    QCOMPARE(http.sendCount, 0);
    controller.testAiConnection(QStringLiteral("test-key"));
    QTRY_COMPARE(ai.count(), 3);
    QCOMPARE(ai.last().first().toMap().value(QStringLiteral("requestId")).toInt(), 0);
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

void AppControllerTests::navigatePendingAndMismatchCues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());

    Subtitle matched;
    matched.text = QStringLiteral("对齐");
    matched.start = MediaTime::fromMilliseconds(0);
    matched.end = MediaTime::fromMilliseconds(1'000);
    matched.status = QStringLiteral("MATCHED");
    matched.confidence = 0.9;
    matched.metadata.insert(QStringLiteral("audioEvidence"), true);

    Subtitle low;
    low.text = QStringLiteral("待确认");
    low.start = MediaTime::fromMilliseconds(1'200);
    low.end = MediaTime::fromMilliseconds(2'000);
    low.status = QStringLiteral("LOW_CONFIDENCE");
    low.confidence = 0.2;
    low.metadata.insert(QStringLiteral("audioEvidence"), true);

    Subtitle review;
    review.text = QStringLiteral("对不上");
    review.start = MediaTime::fromMilliseconds(2'200);
    review.end = MediaTime::fromMilliseconds(3'000);
    review.status = QStringLiteral("REVIEW");
    review.metadata.insert(QStringLiteral("audioEvidence"), false);

    Subtitle untimed;
    untimed.text = QStringLiteral("未定位");
    untimed.status = QStringLiteral("SKIPPED_NO_AUDIO");

    controller.applySubtitles({matched, low, review, untimed});
    controller.selectCue(0, false);
    controller.navigatePendingCue(1);
    QCOMPARE(controller.selectedCue(), 1);
    controller.navigatePendingCue(1);
    QCOMPARE(controller.selectedCue(), 2);
    controller.navigatePendingCue(1);
    QCOMPARE(controller.selectedCue(), 3);
    controller.navigateMismatchCue(1);
    QCOMPARE(controller.selectedCue(), 1);
    controller.navigateMismatchCue(1);
    QCOMPARE(controller.selectedCue(), 2);
}

void AppControllerTests::multilinePreviewFlagAndExportPreservesModified()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputAss"), false);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    context->settings.insert(QStringLiteral("fontSize1080p"), 52);
    AppController controller(context.get());

    Subtitle shortCue;
    shortCue.text = QStringLiteral("短");
    shortCue.start = MediaTime::fromMilliseconds(0);
    shortCue.end = MediaTime::fromMilliseconds(1'000);
    shortCue.status = QStringLiteral("MATCHED");

    Subtitle longCue;
    longCue.text = QStringLiteral("这是一条需要在节目监视器里折成两行以上的很长很长很长很长很长很长很长很长很长很长的字幕文本内容");
    longCue.start = MediaTime::fromMilliseconds(1'200);
    longCue.end = MediaTime::fromMilliseconds(2'200);
    longCue.status = QStringLiteral("MATCHED");

    controller.applySubtitles({shortCue, longCue});
    QVERIFY(controller.modified());
    QVERIFY(!controller.document()->subtitles().at(0).metadata.value(QStringLiteral("multilinePreview")).toBool());
    QVERIFY(controller.document()->subtitles().at(1).metadata.value(QStringLiteral("multilinePreview")).toBool());

    controller.exportSubtitles();
    QVERIFY(controller.statusText().contains(QStringLiteral("导出完成")));
    QVERIFY(controller.modified());
}

void AppControllerTests::cloudSettingsDraftAndDiscovery()
{
    class DiscoveryHttp final : public IHttpClient {
    public:
        std::atomic<bool> hold{false}, entered{false}, canceled{false};
        HttpRequest request;
        std::variant<HttpResponse, AppError> send(const HttpRequest &value, const std::atomic<bool> *cancel) override {
            request = value; entered = true;
            while (hold && !cancel->load()) QThread::msleep(5);
            if (cancel->load()) { canceled = true; return AppError(ErrorDomain::Network, 1, QStringLiteral("已取消")); }
            return HttpResponse{200, {}, QByteArrayLiteral("{\"data\":[{\"id\":\"test-model\"},{\"id\":\"cloud-ui-sentinel\"}]}")};
        }
        std::variant<qint64, AppError> download(const HttpRequest &, QFile *, qint64,
            const std::atomic<bool> *, const std::function<void(qint64,qint64)> &) override { return qint64{0}; }
    } http;
    QTemporaryDir directory; auto context = makeContext(directory); context->aiHttp = &http;
    AppController controller(context.get());
    auto *cloud = qobject_cast<CloudSettingsController *>(controller.cloudSettings()); QVERIFY(cloud);
    cloud->begin(); QVERIFY(!cloud->dirty());
    const auto id = cloud->addAccount(QStringLiteral("deepseek")); QVERIFY(!id.isEmpty());
    QVERIFY(cloud->stageKey(id, QStringLiteral("cloud-ui-sentinel")));
    const auto model = cloud->addModel(id, QStringLiteral("test-model"), QStringLiteral("compatible-text"));
    cloud->bindModel(QStringLiteral("subtitleWordMapping"), model);
    QVERIFY(!QJsonDocument::fromVariant(cloud->accounts()).toJson().contains("cloud-ui-sentinel"));
    cloud->discoverModels(id); QTRY_VERIFY(http.entered.load()); QTRY_VERIFY(!cloud->busy());
    QCOMPARE(cloud->discovered().size(), 1); QVERIFY(!http.request.followRedirects);
    QCOMPARE(http.request.url.host(), QStringLiteral("api.deepseek.com"));
    QVERIFY2(cloud->save(), qPrintable(cloud->status())); QVERIFY(!cloud->dirty());
    bool saved = false; for (const auto &value : cloud->accounts()) if (value.toMap().value(QStringLiteral("id")).toString() == id) saved = value.toMap().value(QStringLiteral("keyState")).toString() == QStringLiteral("已保存");
    QVERIFY(saved);
    cloud->editAccount(id, {{QStringLiteral("region"), QStringLiteral("singapore")}});
    QVERIFY(!cloud->dirty()); QVERIFY(cloud->status().contains(QStringLiteral("新建账号")));
    cloud->editAccount(id, {{QStringLiteral("name"), QStringLiteral("暂存名称")}}); QVERIFY(cloud->dirty());
    cloud->discard(); QVERIFY(!cloud->dirty());
    http.entered = false; http.hold = true; cloud->discoverModels(id); QTRY_VERIFY(http.entered.load());
    cloud->discard(); QTRY_VERIFY(http.canceled.load()); QVERIFY(cloud->discovered().isEmpty());
    cloud->removeModel(model); QVERIFY(cloud->dirty()); cloud->discard();
    http.entered = false; http.canceled = false;
    { CloudSettingsController transient(context.get()); transient.begin(); transient.discoverModels(id); QTRY_VERIFY(http.entered.load()); }
    QVERIFY(http.canceled.load());
    cloud->removeKey(id); QVERIFY(cloud->save()); QVERIFY(context->cloudConfig->resolveKey(id).isEmpty());
}

void AppControllerTests::cloudSwitchConflictAndCapability()
{
    class ProbeHttp final : public IHttpClient {
    public:
        std::atomic<bool> hold{false}, entered{false}, canceled{false};
        HttpRequest request;
        std::variant<HttpResponse, AppError> send(const HttpRequest &value, const std::atomic<bool> *cancel) override {
            request = value; entered = true;
            while (hold && !cancel->load()) QThread::msleep(2);
            if (cancel->load()) { canceled = true; return AppError(ErrorDomain::Network, 1, QStringLiteral("cancel")); }
            return HttpResponse{200, {}, QByteArrayLiteral(R"({"choices":[{"message":{"tool_calls":[{"function":{"name":"submit_connection_test","arguments":"{\"ok\":true}"}}]}}]})")};
        }
        std::variant<qint64, AppError> download(const HttpRequest &, QFile *, qint64, const std::atomic<bool> *, const std::function<void(qint64,qint64)> &) override { return qint64{0}; }
    } http;
    QTemporaryDir directory; auto context = makeContext(directory); context->aiHttp = &http;
    CloudSettingsController cloud(context.get());
    QVERIFY(cloud.switchTask(QStringLiteral("speechRecognition"), QStringLiteral("local:qwen3")));
    CloudSettingsController other(context.get());
    QVERIFY(other.switchTask(QStringLiteral("speechRecognition"), QStringLiteral("local:funasr")));
    QVERIFY(!cloud.switchTask(QStringLiteral("speechRecognition"), QStringLiteral("local:qwen3")));
    QVERIFY(!cloud.dirty()); // 冲突不覆盖新配置，也不把失败选择留在表单。
    cloud.begin();
    const auto account = cloud.addAccount(QStringLiteral("deepseek"));
    QVERIFY(cloud.stageKey(account, QStringLiteral("probe-secret")));
    const auto model = cloud.addModel(account, QStringLiteral("unknown-text"), QStringLiteral("compatible-text"));
    cloud.bindModel(QStringLiteral("subtitleWordMapping"), model);
    QVERIFY(!cloud.switchTask(QStringLiteral("speechRecognition"), QStringLiteral("local:qwen3"))); QVERIFY(cloud.dirty());
    cloud.testCapability(model); QTRY_VERIFY(!cloud.busy());
    QCOMPARE(cloud.tests().value(model).toMap().value(QStringLiteral("state")).toString(), QStringLiteral("passed"));
    QVERIFY(context->cloudConfig->resolveKey(account).isEmpty()); // 测试不保存账号或密钥。
    QVERIFY(!http.request.followRedirects); QVERIFY(!http.request.body.contains("input_audio"));
    QVERIFY(!http.request.body.contains("enable_thinking"));
    QVERIFY(cloud.save());
    QString key, error; QVERIFY(!context->cloudConfig->resolveTask(QStringLiteral("subtitleWordMapping"), &key, &error).isEmpty());
    QCOMPARE(key, QStringLiteral("probe-secret"));
    CloudSettingsController reopened(context.get());
    QCOMPARE(reopened.tests().value(model).toMap().value(QStringLiteral("state")).toString(), QStringLiteral("passed"));
    QVERIFY(cloud.stageKey(account, QStringLiteral("rotated-probe-secret"))); QVERIFY(cloud.save());
    QVERIFY(context->cloudConfig->resolveTask(QStringLiteral("subtitleWordMapping"), &key, &error).isEmpty());
    http.hold = true; http.entered = false; cloud.testCapability(model); QTRY_VERIFY(http.entered.load());
    other.begin(); other.removeKey(account); QVERIFY(other.save());
    QVERIFY(http.canceled.load()); QVERIFY(!cloud.busy()); QTest::qWait(20);
    QVERIFY(cloud.tests().value(model).toMap().value(QStringLiteral("state")) != QStringLiteral("passed"));
    QVERIFY(!QJsonDocument::fromVariant(cloud.tests()).toJson().contains("probe-secret"));
}

void AppControllerTests::subtitleTimecodeShortcutsKeepDocumentInSync()
{
    // 字幕精修快捷键（[ ]、Q/W、Alt+方向、I/O）此前没有任何断言；这里按用户可见结果校验：
    // 时间码改动必须落到文档、标记为人工确认、可撤销，并且不越过素材范围或 250ms 最短时长。
    QTemporaryDir dir;
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.hasMedia());
    QVERIFY(controller.durationUs() > 900'000);
    const auto startOf = [&controller](int row) { return controller.document()->subtitles().at(row).start.microseconds(); };
    const auto endOf = [&controller](int row) { return controller.document()->subtitles().at(row).end.microseconds(); };

    Subtitle first;
    first.id = QStringLiteral("a"); first.text = QStringLiteral("第一句");
    first.start = MediaTime::fromMicroseconds(0); first.end = MediaTime::fromMicroseconds(300'000);
    first.status = QStringLiteral("MATCHED");
    Subtitle second;
    second.id = QStringLiteral("b"); second.text = QStringLiteral("第二句");
    second.start = MediaTime::fromMicroseconds(310'000); second.end = MediaTime::fromMicroseconds(620'000);
    second.status = QStringLiteral("MATCHED");
    Subtitle third;
    third.id = QStringLiteral("c"); third.text = QStringLiteral("第三句");
    third.start = MediaTime::fromMicroseconds(640'000); third.end = MediaTime::fromMicroseconds(980'000);
    third.status = QStringLiteral("MATCHED");
    controller.applySubtitles({first, second, third});

    // 「]」把当前字幕开始设到播放头；手动改过即视为人工确认。
    controller.selectCue(1, false);
    controller.seekUs(330'000);
    controller.setCurrentStart();
    QCOMPARE(startOf(1), qint64{330'000});
    QCOMPARE(controller.document()->subtitles().at(1).status, QStringLiteral("MANUAL"));
    // Alt+方向微调：25fps 下一帧 40ms，起点左移两帧。
    controller.nudgeCurrentStart(-2);
    QCOMPARE(startOf(1), qint64{250'000});
    // 终点右移两帧，且不得超过素材时长。
    controller.nudgeCurrentEnd(2);
    QCOMPARE(endOf(1), qint64{700'000});
    // 「[」把当前字幕结束设到播放头。
    controller.seekUs(600'000);
    controller.setCurrentEnd();
    QCOMPARE(endOf(1), qint64{600'000});
    // Q：把上一条的结束对齐到播放头。
    controller.seekUs(630'000);
    controller.setPreviousEnd();
    QCOMPARE(endOf(1), qint64{630'000});
    // W：把下一条的开始对齐到播放头。
    controller.seekUs(635'000);
    controller.setFollowingStart();
    QCOMPARE(startOf(2), qint64{635'000});
    QVERIFY(controller.modified());

    // 入点/出点：设置读取播放头，清除回到无入出点。
    controller.seekUs(200'000);
    controller.setInPoint();
    controller.setOutPoint();
    QCOMPARE(controller.inPoint(), qint64{200});
    QCOMPARE(controller.outPoint(), qint64{200});
    controller.clearInPoint();
    controller.clearOutPoint();
    QCOMPARE(controller.inPoint(), qint64{-1});
    QCOMPARE(controller.outPoint(), qint64{-1});

    // 空白文本必须被拒绝，且不改变文档。
    const QString textBefore = controller.document()->subtitles().at(0).text;
    QVERIFY(!controller.applyCueEdit(QStringLiteral("a"), QStringLiteral("   "), -1, -1));
    QVERIFY(controller.statusText().contains(QStringLiteral("不能为空")));
    QCOMPARE(controller.document()->subtitles().at(0).text, textBefore);

    // Delete 删除当前字幕，撤销可以恢复。
    const int countBefore = controller.document()->count();
    controller.selectCue(2, false);
    controller.deleteCue();
    QCOMPARE(controller.document()->count(), countBefore - 1);
    controller.undo();
    QCOMPARE(controller.document()->count(), countBefore);
}

void AppControllerTests::relinkMediaRejectsDifferentContent()
{
    // 「重新定位素材…」只在内容指纹一致时接管新路径；内容不同的文件必须被拒绝并保留原素材。
    QTemporaryDir dir;
    auto context = makeContext(dir);
    AppController controller(context.get());
    const QString media = mediaPath(QStringLiteral("audio.wav"));
    controller.loadMedia(QUrl::fromLocalFile(media));
    QTRY_VERIFY(!controller.busy() && controller.hasMedia());
    QSignalSpy saved(&controller, &AppController::projectSaveFinished);
    const QString projectPath = dir.filePath(QStringLiteral("素材身份.subcue"));
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(projectPath)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.first().first().toBool(), qPrintable(controller.statusText()));

    const QString copy = dir.filePath(QStringLiteral("同内容副本.wav"));
    QVERIFY(QFile::copy(media, copy));
    controller.relinkMedia(QUrl::fromLocalFile(copy));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(QFileInfo(controller.mediaPath()).fileName(), QStringLiteral("同内容副本.wav"));
    QVERIFY(controller.modified());

    const QString different = dir.filePath(QStringLiteral("不同内容.wav"));
    QFile other(different);
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("not-the-same-content");
    other.close();
    const QString kept = controller.mediaPath();
    controller.relinkMedia(QUrl::fromLocalFile(different));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.mediaPath(), kept);
    QVERIFY(controller.statusText().contains(QStringLiteral("不同")));
}

QTEST_MAIN(AppControllerTests)
#include "test_app_controller.moc"
