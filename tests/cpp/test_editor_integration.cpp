#include "cloud_settings_controller.h"
#include "ai/ai_provider.h"
#include "ai/ai_review_types.h"
#include "ai/ai_types.h"
#include "alignment/alignment_pipeline.h"
#include "app_controller.h"
#include "application_context.h" // 依赖 ApplicationContext 析构声明
#include "project_session.h"
#include "tts_task_controller.h"
#include "voice_library_controller.h"
#include "voice_studio_controller.h"
#include "audio_preview_controller.h"
#include "voice_audio_controller.h"
#include "tts/voice_audio.h"
#include "tts/voice_document.h"
#include "tts/audio_quality.h"
#include "tts/cloud_tts_provider.h"
#include "tts/voice_library.h"
#include "inference/inference_manager.h"
#include "asr/asr_service.h"
#include "asr/http_client.h"
#include "media/media_probe.h"
#include "playback/playback_engine.h"
#include "rough_cut_controller.h"
#include "roughcut/rough_cut_project.h"
#include "subtitle/subtitle.h"
#include "timeline_scene_item.h"
#include "video_preview_item.h"
#include "workspace_router.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QScreen>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QWheelEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QJSValue>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlComponent>
#include <QtQml/qqml.h>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <QtCore/QDate>
#include <QtCore/QLockFile>
#include <memory>
#include <variant>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#endif

using namespace subcue;

namespace {

class FakeAsrService final : public IAsrService {
public:
    Transcript transcript;
    std::atomic<bool> blockUntilCancel{false};
    Qt::HANDLE workerThreadId = nullptr;
    int calls = 0;

    [[nodiscard]] QString providerId() const override
    {
        return QStringLiteral("fake");
    }

    [[nodiscard]] AsrResult transcribe(const AsrRequest &request) override
    {
        ++calls;
        workerThreadId = QThread::currentThreadId();
        if (request.progress) {
            request.progress(blockUntilCancel ? 0 : 1, 1);
        }
        while (blockUntilCancel && !asrCancelled(request.cancel)) {
            QThread::msleep(15);
        }
        if (asrCancelled(request.cancel)) {
            return asrCancelledError();
        }
        if (transcript.words.isEmpty()) return AppError(ErrorDomain::Asr,
            static_cast<int>(AsrErrorCode::EmptyTranscript), QStringLiteral("没有识别到词"));
        Transcript copy = transcript;
        copy.sortAndReindex();
        return copy;
    }
};

class RecordingHttpClient final : public IHttpClient {
public:
    int sendCount = 0;
    QVector<HttpRequest> requests;
    QVector<HttpResponse> responses;
    std::atomic<bool> blockUntilCancel{false};
    std::atomic<bool> inFlight{false};
    std::atomic<bool> blockDownloadUntilCancel{false};
    QString blockAction;
    QByteArray downloadBody;
    HttpRequest downloadRequest;
    int downloadCount = 0;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request, const std::atomic<bool> *cancel) override
    {
        requests.append(request);
        ++sendCount;
        inFlight = true;
        while (blockUntilCancel && (blockAction.isEmpty() || QJsonDocument::fromJson(request.body).object().value(QStringLiteral("input")).toObject().value(QStringLiteral("action")).toString() == blockAction) && !aiCancelled(cancel)) {
            QThread::msleep(15);
        }
        inFlight = false;
        if (aiCancelled(cancel)) return aiCancelledError();
        if (responses.isEmpty()) {
            return HttpResponse{200, {}, QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"{}\"}}]}")};
        }
        const int index = qMin(sendCount - 1, static_cast<int>(responses.size() - 1));
        return responses.at(index);
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &request, QFile *output, qint64, const std::atomic<bool> *cancel,
        const std::function<void(qint64, qint64)> &) override
    {
        downloadRequest = request; ++downloadCount;
        if (blockDownloadUntilCancel) {
            inFlight = true;
            while (!aiCancelled(cancel)) QThread::msleep(10);
            inFlight = false;
            return aiCancelledError();
        }
        if (!downloadBody.isEmpty()) return output->write(downloadBody);
        return AppError(ErrorDomain::Network, 1, QStringLiteral("unused"));
    }
};

QByteArray jsonChatBody(const QJsonObject &arguments)
{
    const QJsonObject message{
        {QStringLiteral("content"), QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))},
    };
    QJsonObject root{
        {QStringLiteral("model"), QStringLiteral("qwen3.8-omni-flash")},
        {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message}}}},
    };
    root.insert(QStringLiteral("usage"), QJsonObject{
        {QStringLiteral("prompt_tokens"), 80},
        {QStringLiteral("completion_tokens"), 16},
        {QStringLiteral("total_tokens"), 96},
    });
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QSet<QString> frozenEditorShortcuts()
{
    return {
        QStringLiteral("Ctrl+O"),
        QStringLiteral("Ctrl+Z"),
        QStringLiteral("Ctrl+Y"),
        QStringLiteral("Delete"),
        QStringLiteral("Ctrl+I"),
        QStringLiteral("Space"),
        QStringLiteral("J"),
        QStringLiteral("K"),
        QStringLiteral("L"),
        QStringLiteral("Left"),
        QStringLiteral("Right"),
        QStringLiteral("Shift+Left"),
        QStringLiteral("Shift+Right"),
        QStringLiteral("3"),
        QStringLiteral("4"),
        QStringLiteral("Q"),
        QStringLiteral("W"),
        QStringLiteral("T"),
        QStringLiteral("["),
        QStringLiteral("]"),
        QStringLiteral("C"),
        QStringLiteral("Return"),
        QStringLiteral("Enter"),
        QStringLiteral("Shift+Return"),
        QStringLiteral("Shift+Enter"),
        QStringLiteral("Ctrl+Return"),
        QStringLiteral("Ctrl+Enter"),
        QStringLiteral("Up"),
        QStringLiteral("Down"),
        QStringLiteral("Tab"),
        QStringLiteral("Shift+Up"),
        QStringLiteral("Shift+Down"),
        QStringLiteral("Ctrl+Up"),
        QStringLiteral("Ctrl+Down"),
        QStringLiteral("I"),
        QStringLiteral("O"),
        QStringLiteral("Shift+I"),
        QStringLiteral("Shift+O"),
        QStringLiteral("Home"),
        QStringLiteral("End"),
        QStringLiteral("Alt+Left"),
        QStringLiteral("Alt+Right"),
        QStringLiteral("Alt+Shift+Left"),
        QStringLiteral("Alt+Shift+Right"),
        QStringLiteral("Ctrl+Alt+Left"),
        QStringLiteral("Ctrl+Alt+Right"),
        QStringLiteral("Ctrl+Alt+Shift+Left"),
        QStringLiteral("Ctrl+Alt+Shift+Right"),
        QStringLiteral("Alt+I"),
        QStringLiteral("Alt+O"),
        QStringLiteral("S"),
        QStringLiteral("="),
        QStringLiteral("-"),
        QStringLiteral("\\"),
    };
}

[[nodiscard]] QSet<QString> shortcutTokens(const QString &source)
{
    QSet<QString> tokens;
    QRegularExpression sequence(QStringLiteral("(?:sequence|keySequence):\\s*\"([^\"]+)\""));
    QRegularExpressionMatchIterator it = sequence.globalMatch(source);
    while (it.hasNext()) {
        const QString token = it.next().captured(1);
        tokens.insert(token == QLatin1String("\\\\") ? QStringLiteral("\\") : token);
    }
    QRegularExpression sequences(QStringLiteral("sequences:\\s*\\[([^\\]]+)\\]"));
    QRegularExpressionMatchIterator grouped = sequences.globalMatch(source);
    while (grouped.hasNext()) {
        const QString inner = grouped.next().captured(1);
        QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
        QRegularExpressionMatchIterator quotedIt = quoted.globalMatch(inner);
        while (quotedIt.hasNext()) {
            const QString token = quotedIt.next().captured(1);
            tokens.insert(token == QLatin1String("\\\\") ? QStringLiteral("\\") : token);
        }
    }
    return tokens;
}

void registerSubCueQmlItems()
{
    static const bool registered = [] {
        qmlRegisterType<TimelineSceneItem>("SubCue", 1, 0, "TimelineSceneItem");
        qmlRegisterType<VideoPreviewItem>("SubCue", 1, 0, "VideoPreviewItem");
        return true;
    }();
    Q_UNUSED(registered);
}

#ifdef Q_OS_WIN
[[nodiscard]] SIZE_T privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters))) {
        return 0;
    }
    return counters.PrivateUsage;
}
#endif

QString configureUnifiedSynthesis(ApplicationContext *context)
{
    CloudSettingsController cloud(context); cloud.begin();
    const auto account = cloud.addAccount(QStringLiteral("bailian"));
    if (!cloud.stageKey(account, QStringLiteral("fake-TTS-secret-not-ASR"))) return {};
    const auto model = cloud.addModel(account, QStringLiteral("qwen-audio-3.1-tts-flash"), QStringLiteral("qwen-tts"));
    cloud.editModel(model, {{QStringLiteral("workspace"), QStringLiteral("mock-space")}});
    cloud.bindModel(QStringLiteral("speechSynthesis"), model);
    return cloud.save() ? account : QString();
}

bool configureUnifiedReview(ApplicationContext *context)
{
    CloudSettingsController cloud(context); cloud.begin();
    const auto account = cloud.addAccount(QStringLiteral("bailian"));
    if (!cloud.stageKey(account, QStringLiteral("unified-review-test-key"))) return false;
    const auto audio = cloud.addModel(account, QStringLiteral("qwen3.8-omni-flash"), QStringLiteral("qwen-omni"));
    const auto text = cloud.addModel(account, QStringLiteral("qwen-plus"), QStringLiteral("compatible-text"));
    if (audio.isEmpty() || text.isEmpty()) return false;
    cloud.bindModel(QStringLiteral("subtitleAudioReview"), audio);
    cloud.bindModel(QStringLiteral("roughcutAudioReview"), audio);
    cloud.bindModel(QStringLiteral("subtitleWordMapping"), text);
    return cloud.save() && context->credentials.save(QStringLiteral("SubCue/AIReview/qwen_omni"), QStringLiteral("legacy-ignored"));
}

} // namespace

class EditorIntegrationTests final : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void voiceCloudCloneConsentPersistenceAndCancellation();
    void voiceCloudDeletionConsentPersistenceAndCancellation();
    void voiceCloudUpdateRevisionAndCancellation();
    void voiceLibraryOwnerMaintainsProjectCopies();
    void voiceBatchAndDerivedVersionsPreserveManualReview();
    void voiceBatchDestructionCancelsAndRemovesTemporarySamples();
    void voiceBatchMissingAsrPreservesManualPathAndBindings();
    void voiceRealChineseSamplesWhenRequested();
    void voiceRealQwenWhenRequested();
    void voiceAppliedAudioMustBeIntactBeforeReuse();
    void voiceRealProjectRoundTripWhenRequested();
    void voiceInterruptedProjectRecoveryWhenRequested();
    void voiceFlashRealSingleWhenAuthorized();
    void voiceCloudProjectAcceptanceWhenRequested();
    void voiceFlashDownloadedWavCompletesProject();
    void voiceCrossLanguageWhenRequested();
    void voiceEmotionWhenRequested();
    void voiceEmotionReviewAndCancellation();
    void voiceEmotionBatchCancellationAndDestruction();
    void voiceEmotionBatchWhenRequested();
    void voiceQwenMissingEnvironmentPreservesEditing();
    void voiceFlashUsageAndUnknownResultsAreProtected();
    void voiceFlashConnectionDoesNotSynthesizeOrRevealCredentials();
    void voiceLongNarrationStressWhenRequested();
    void voiceRealExternalEngineWhenRequested();
    void voiceDocumentSharesLifecycleAndPreservesEditsDuringSave();
    void voiceUnsavedWorkspaceGeneratesExportsAndMigratesAssets();
    void voiceSaveWaitsWithoutCancellingGeneration();
    void recoveryPreservesUnnamedDocumentAndFieldDrafts();
    void normalExitDiscardsRecovery();
    void openingProjectDiscardsPreviousRecovery();
    void voiceSearchPreservesSelectionAndDocument();
    void voiceEditorCapabilitiesAndBuiltinCatalog();
    void voiceMultiSelectionLockingAndRecovery();
    void voiceCloudDiscoveryScopesAndCancellation();
    void voiceNativeModelDiscoveryAndWorkspace();
    void voiceCapabilityDrivenReferenceAndLanguage();
    void voiceClearsUnsupportedParametersAndPreservesArrangement();
    void voiceDocumentMockCandidatesProtectManualCropAndVersions();
    void voiceDocumentImportDestructionHasNoLateCallback();
    void voiceDocumentSubtitleSyncAndJsonProtectIndependentEdits();
    void voiceAudioCacheExportAndCancellationProtectFrozenArrangement();
    void voiceAudioCacheRejectsChangedSource();
    void voiceAudioDestructionHasNoLatePublication();
    void voiceRoughCutImportPreservesSourceDecisionsAndExistingEdits();
    void voiceLibraryPagingAndRestore();
    void audioProcessingIsBoundedAndDoesNotBlockStorage();
    void voiceReferenceReviewCreatesNewVersion();
    void voiceReviewDestructionCancelsQueuedAudio();
    void referencePreviewTransfersPlaybackAndCloses();
    void voiceCreationDraftIsReviewedAtomicAndCancellable();
    void voicePreviewFiftyCyclesReleaseResources();
    void voiceHardwarePreviewWhenRequested();
    void referencePreviewDestructionRejectsQueuedStart();
    void initTestCase();
    void projectStorageQueueSerializesAndCancelsSaves();
    void sharedProjectRoundTripAndSaveAs();
    void sharedProjectSavePreservesNewEdits();
    void sharedProjectRepeatSaveCreatesNoDuplicateFiles();
    void sharedProjectFailedOpenPreservesCurrent();
    void sharedProjectFailedSaveAsPreservesEdits_data();
    void sharedProjectFailedSaveAsPreservesEdits();
    void sharedProjectCancelQueuedSaveOnShutdown();
    void roughCutUnifiedProjectKeepsMediaIdentity();
    void sharedLegacyLifecycle_data();
    void sharedLegacyLifecycle();
    void ttsTaskOwnerPersistsAndCancelsBeforeProjectChange();
    void ttsTaskRejectsSilentAudioAndContinuesBatch();
    void ttsTaskCountsOnlyCommittedCandidates();
    void qmlSharedProjectWindow();
    void qmlCloseWhileSavingKeepsSavedState();
    void destroyingControllerWaitsForQueuedSave_data();
    void destroyingControllerWaitsForQueuedSave();
    void qmlShortcutsMatchFeatureMatrix();
    void qmlImportDropAndLayout();
    void roughReviewNavigationPreservesFilters();
    void exportResultUsesIncrementingSubtitleFolder();
    void pipelineAlignsWithInjectedAsrOnWorkerThread();
    void alignmentAndAsrReviewDoNotCallAi();
    void wordMappingReviewIsUndoableAndRejectsStaleEvidence();
    void subtitleContentReviewRequiresAccept();
    void omniReviewStartsAtZeroProgress();
    void destroyingControllerCancelsAiReview();
    void qmlUnifiedAiSettingsAndWorkspaceScreenshots();
    void appControllerAlignmentCancelWaitsForWorker();
    void appControllerCreateNextScriptCue();
    void alignmentDoesNotModifyScriptText();
    void replacingSubtitlesClearsUndoHistory();
    void switchingMediaCancelsAlignment();
    void reversePlaybackUsesElapsedTime();
    void wheelTemporarilyDefersFollow();
    void indexedTimelineHandlesUnsortedAndChangedCues();
    void playbackAvSyncP95WithinFortyMs();
    void playbackOneThousandSeeksHasNoStaleFrame();
    void playbackRepeatedOpenCloseDoesNotGrowUnbounded();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
    [[nodiscard]] QString sourcePath(const QString &relative) const;
    [[nodiscard]] std::unique_ptr<ApplicationContext> makeContext(const QTemporaryDir &dir);
    QStringList credentialPaths_;
};

void EditorIntegrationTests::initTestCase()
{
#ifdef Q_OS_WIN
    // offscreen 平台不扫描系统字体；显式注册，否则测试截图里的中文全是方块，无法作为人工验收证据。
    qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")).toUtf8());
    for (const auto &name : {QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc"), QStringLiteral("simhei.ttf")}) {
        const auto path = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/") + name);
        if (QFileInfo::exists(path)) QFontDatabase::addApplicationFont(path);
    }
#endif
    // 原生截图与正式程序使用相同的中文字体和像素字号。
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setPixelSize(13); QGuiApplication::setFont(font);
    qInfo() << "native_screen_dpr=" << QGuiApplication::primaryScreen()->devicePixelRatio()
            << "available=" << QGuiApplication::primaryScreen()->availableGeometry()
            << "scale=" << qEnvironmentVariable("QT_SCALE_FACTOR") << "screens=" << qEnvironmentVariable("QT_SCREEN_SCALE_FACTORS");
    // 截图用例写源码树下的 .test_tmp/，而 QImage::save 不会自建上层目录。
    const QString screenshotDir = sourcePath(QStringLiteral(".test_tmp"));
    QVERIFY2(QDir().mkpath(screenshotDir),
             qPrintable(QStringLiteral("无法创建截图目录: %1").arg(screenshotDir)));
}

void EditorIntegrationTests::roughCutUnifiedProjectKeepsMediaIdentity()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    roughCut.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy(), 8'000);
    QVERIFY(roughCut.mediaAvailable());
    QSignalSpy saved(&session, &ProjectSession::saved);
    const auto path = directory.filePath(QStringLiteral("粗剪素材身份.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path), QStringLiteral("roughcut")));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    // 统一工程只保存媒体资产引用；快照缺少指纹时，重开后的素材会被判离线，导出入口永久不可用。
    const auto reopened = ProjectStorageAdapter(path, ProjectStorageAdapter::Format::UnifiedSqlite)
        .loadModule(QStringLiteral("roughcut"), nullptr);
    QVERIFY(reopened && std::holds_alternative<RoughCutProject>(*reopened));
    QCOMPARE(std::get<RoughCutProject>(*reopened).mediaSha256.size(), 32);

    auto second = makeContext(directory);
    AppController editor2(second.get()); RoughCutController roughCut2(second.get());
    ProjectSession session2(second.get(), &editor2, &roughCut2);
    // 第一个会话仍持有同一路径的会话锁，重开使用同一内容的副本。
    const auto reopenPath = directory.filePath(QStringLiteral("粗剪素材身份-重开.subcue-project"));
    QVERIFY(QFile::copy(path, reopenPath));
    QSignalSpy opened(&session2, &ProjectSession::opened);
    QVERIFY(session2.openProject(QUrl::fromLocalFile(reopenPath), QStringLiteral("roughcut")));
    QTRY_COMPARE(opened.count(), 1);
    QVERIFY2(opened.last().first().toBool(), qPrintable(session2.statusText()));
    QTRY_VERIFY_WITH_TIMEOUT(roughCut2.mediaAvailable(), 8'000);
}

void EditorIntegrationTests::voiceHardwarePreviewWhenRequested()
{
    if (qEnvironmentVariable("SUBCUE_VOICE_REAL_AUDIO_CHECK") != QLatin1String("1")) QSKIP("真实设备试听需要显式启用。");
    QTemporaryDir directory; auto context = makeContext(directory);
    AppError error(ErrorDomain::Media, 0, {}); context->audioDevice = createAudioDevice(AudioDeviceKind::Wasapi, &error);
    QVERIFY2(context->audioDevice && context->audioDevice->isHardware(), qPrintable(error.userMessage()));
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough); session.newProject();
    AudioPreviewController preview(context.get(), &session); const auto path = mediaPath(QStringLiteral("audio.wav"));
    const auto quality = AudioQuality::inspect(path); QVERIFY(quality);
    preview.play(path, QString::fromLatin1(quality->sha256.toHex())); QTRY_VERIFY2(preview.playing(), qPrintable(preview.statusText()));
    QVERIFY(!preview.statusText().contains(QStringLiteral("虚拟"))); preview.pause(); QVERIFY(preview.paused()); preview.seek(1000); preview.resume();
    QTRY_VERIFY_WITH_TIMEOUT(!preview.playing(), 5000); QVERIFY(!context->audioDevice->isStarted());
}

void EditorIntegrationTests::voicePreviewFiftyCyclesReleaseResources()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); session.newProject();
    AudioPreviewController preview(context.get(), &session);
    const auto path = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(path); QVERIFY(quality);
#ifdef Q_OS_WIN
    SIZE_T smallest = std::numeric_limits<SIZE_T>::max(), largest = 0;
#endif
    for (int cycle = 0; cycle < 50; ++cycle) {
        preview.play(path, QString::fromLatin1(quality->sha256.toHex())); QTRY_VERIFY(preview.playing());
        preview.pause(); QVERIFY(preview.paused()); preview.seek(1000); QCOMPARE(preview.positionUs(), 1000);
        preview.resume(); QVERIFY(preview.playing());
        preview.play(path, QString::fromLatin1(quality->sha256.toHex()), 2000); QTRY_VERIFY(preview.playing());
        preview.stop(); QVERIFY(!preview.playing()); QVERIFY(!preview.paused()); QVERIFY(!preview.loading());
        QVERIFY(preview.sourcePath().isEmpty()); QVERIFY(!context->audioDevice->isStarted());
        QCoreApplication::processEvents();
#ifdef Q_OS_WIN
        if (cycle >= 40) { smallest = std::min(smallest, privateBytes()); largest = std::max(largest, privateBytes()); }
#endif
    }
#ifdef Q_OS_WIN
    QVERIFY2(largest - smallest <= 32ull * 1024 * 1024, qPrintable(QStringLiteral("末十轮私有内存波动 %1 字节").arg(qulonglong(largest - smallest))));
    QFile report(sourcePath(QStringLiteral(".test_tmp/voice-lifecycle-memory.json")));
    QVERIFY(report.open(QIODevice::WriteOnly)); report.write(QJsonDocument(QJsonObject{{QStringLiteral("cycles"), 50}, {QStringLiteral("lateMinimumBytes"), double(smallest)}, {QStringLiteral("lateMaximumBytes"), double(largest)}, {QStringLiteral("lateFluctuationBytes"), double(largest - smallest)}}).toJson());
#endif
}

void EditorIntegrationTests::voiceCreationDraftIsReviewedAtomicAndCancellable()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); session.newProject();
    const auto path = directory.filePath(QStringLiteral("creation.sqlite"));
    context->settings.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope")); // 此用例不加载真实本地模型。
    VoiceLibraryController voices(context.get(), &session, path); voices.refresh(); QTRY_VERIFY(!voices.busy());
    QVERIFY(!voices.creationCapabilities().value(QStringLiteral("training")).toBool());
    QVERIFY(!voices.creationCapabilities().value(QStringLiteral("modelImport")).toBool());
    voices.beginCreation(); QVERIFY(voices.creating());
    voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))}, false); QTRY_VERIFY(!voices.busy());
    QCOMPARE(voices.samples().size(), 1); QVERIFY(voices.profiles().isEmpty());
    // 未分析前不能创建，且给出明确原因。
    QVERIFY(!voices.creationDraft().value(QStringLiteral("analyzed")).toBool());
    QVERIFY(!voices.creationDraft().value(QStringLiteral("ready")).toBool());
    voices.createFromSamples(QStringLiteral("未分析")); QVERIFY(!voices.busy()); QVERIFY(voices.profiles().isEmpty());
    QVERIFY(voices.statusIsWarning());
    // 分析：测试环境没有本地 ASR，应完成质检并保留手工填写参考转写的路径。
    voices.analyzeCreationSamples(); QTRY_VERIFY(!voices.busy());
    QVERIFY(voices.creationDraft().value(QStringLiteral("analyzed")).toBool());
    QCOMPARE(voices.creationDraft().value(QStringLiteral("promptIndex")).toInt(), 0);
    QVERIFY(!voices.creationDraft().value(QStringLiteral("ready")).toBool());
    QVERIFY(voices.creationDraft().value(QStringLiteral("blockReason")).toString().contains(QStringLiteral("转写")));
    voices.createFromSamples(QStringLiteral("空转写")); QVERIFY(!voices.busy()); QVERIFY(voices.profiles().isEmpty());
    QVERIFY(voices.statusIsWarning());
    // 只编辑参考 prompt 那条的转写即可创建，不再要求逐条确认。
    voices.editCreationTranscript(0, QStringLiteral("人工核对的参考文字"));
    QVERIFY(voices.creationDraft().value(QStringLiteral("ready")).toBool());
    QVERIFY(!voices.samples().first().toMap().value(QStringLiteral("confirmed")).toBool());
    voices.createFromSamples(QStringLiteral("未确认但通过")); QTRY_VERIFY(!voices.busy());
    QVERIFY2(!voices.creating(), qPrintable(voices.statusText())); QCOMPARE(voices.profiles().size(), 1);
    QCOMPARE(voices.versions().size(), 1);
    const auto createdVersionId = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    VoiceLibrary library(path); QString error;
    QCOMPARE(library.defaultVersion(voices.selectedProfile(), &error), createdVersionId);
    const auto assets = QDir(path + QStringLiteral(".assets")).entryList(QDir::Files);
    const auto createdVersion = library.version(createdVersionId); QVERIFY(createdVersion);
    const auto metadata = createdVersion->value(QStringLiteral("metadata")).toObject();
    QCOMPARE(metadata.value(QStringLiteral("qualityState")).toString(), QStringLiteral("approved"));
    QCOMPARE(metadata.value(QStringLiteral("referenceText")).toString(), QStringLiteral("人工核对的参考文字"));
    QVERIFY(metadata.value(QStringLiteral("promptSelected")).toBool());
    const QJsonObject valid{{QStringLiteral("referencePath"), createdVersion->value(QStringLiteral("referencePath"))}, {QStringLiteral("metadata"), metadata}};
    QVERIFY(!library.createWithVersions(QStringLiteral("故障不留空档案"), {valid, QJsonObject{}}, &error));
    QCOMPARE(library.profiles().size(), 1); QCOMPARE(QDir(path + QStringLiteral(".assets")).entryList(QDir::Files), assets);
    voices.beginCreation(); voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))}, false);
    voices.cancelCreation(); QVERIFY(!voices.creating()); QVERIFY(!voices.busy()); QVERIFY(voices.samples().isEmpty());
    QCOMPARE(library.profiles().size(), 1);
    // 语言回填：空语言或未知语言回退到已选参考语言，而不是静默拒绝确认。
    voices.beginCreation(); QTRY_VERIFY(!voices.busy());
    voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))}, false); QTRY_VERIFY(!voices.busy());
    voices.analyzeCreationSamples(); QTRY_VERIFY(!voices.busy());
    voices.editCreationTranscript(0, QStringLiteral("日语参考转写"));
    voices.confirmCreationSample(0, true, QStringLiteral("Japanese"));
    QCOMPARE(voices.samples().first().toMap().value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("Japanese"));
    QVERIFY(voices.samples().first().toMap().value(QStringLiteral("confirmed")).toBool());
    voices.confirmCreationSample(0, true, QString());
    QCOMPARE(voices.samples().first().toMap().value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("Japanese"));
    voices.confirmCreationSample(0, false, QStringLiteral("Korean"));
    QVERIFY(!voices.samples().first().toMap().value(QStringLiteral("confirmed")).toBool());
    QCOMPARE(voices.samples().first().toMap().value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("Japanese"));
    voices.setCreationPrompt(0); QCOMPARE(voices.creationDraft().value(QStringLiteral("promptIndex")).toInt(), 0);
    voices.cancelCreation();
}

void EditorIntegrationTests::referencePreviewTransfersPlaybackAndCloses()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    AudioPreviewController preview(context.get(), &session);
    WorkspaceRouter router(&editor, &roughCut); router.setAudioPreview(&preview);
    const QString path = mediaPath(QStringLiteral("audio.wav"));
    const auto quality = AudioQuality::inspect(path); QVERIFY(quality);
    editor.loadMediaPath(path); QTRY_VERIFY(editor.durationUs() > 0);
    editor.playForward(); QVERIFY(editor.playing());
    QSignalSpy requested(&preview, &AudioPreviewController::requested), released(&preview, &AudioPreviewController::released);
    preview.play(path, QString::fromLatin1(quality->sha256.toHex()));
    QTRY_VERIFY(preview.playing()); QVERIFY(!editor.playing()); QCOMPARE(requested.count(), 1);
    QCOMPARE(preview.durationUs(), quality->sampleCount * 1000000 / quality->sampleRate);
    preview.setVolume(0.25); QCOMPARE(preview.volume(), 0.25);
    preview.setMuted(true); QVERIFY(preview.muted());
    preview.setVolume(2.0); QCOMPARE(preview.volume(), 1.0);
    preview.setMuted(false); QVERIFY(!preview.muted());
    preview.pause(); QVERIFY(preview.paused());
    const auto pausedPosition = preview.positionUs();
    QTest::qWait(80); QCOMPARE(preview.positionUs(), pausedPosition);
    preview.seek(0); QVERIFY(preview.paused());
    preview.resume(); QVERIFY(preview.playing());
    QVERIFY(preview.statusText().contains(QStringLiteral("可听输出")));
    QTRY_VERIFY(!preview.playing()); QCOMPARE(released.count(), 1); QVERIFY(!editor.playing());
    preview.play(path, QString::fromLatin1(quality->sha256.toHex())); QTRY_VERIFY(preview.playing());
    QVERIFY(router.switchTo(QStringLiteral("roughcut"))); QVERIFY(!preview.playing());
    preview.play(path, QString(64, QLatin1Char('0'))); QTRY_VERIFY(!preview.loading());
    QVERIFY(!preview.playing()); QVERIFY(preview.statusText().contains(QStringLiteral("指纹")));
    preview.play(path, QString::fromLatin1(quality->sha256.toHex())); QTRY_VERIFY(preview.playing());
    context->audioDevice->stop(); QTRY_VERIFY(!preview.playing());
    QVERIFY(preview.statusText().contains(QStringLiteral("设备")));
    preview.play(path, QString::fromLatin1(quality->sha256.toHex())); QTRY_VERIFY(preview.playing());
    session.newProject(); QVERIFY(!preview.playing()); QVERIFY(!preview.loading());
}
void EditorIntegrationTests::referencePreviewDestructionRejectsQueuedStart()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const QString path = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(path); QVERIFY(quality);
    QSemaphore started, release; QVector<QFuture<void>> blockers;
    for (int i = 0; i < 2; ++i) blockers.append(context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }));
    const bool running = started.tryAcquire(2, 2000);
    auto preview = std::make_unique<AudioPreviewController>(context.get(), &session);
    QSignalSpy requested(preview.get(), &AudioPreviewController::requested);
    preview->play(path, QString::fromLatin1(quality->sha256.toHex()));
    std::jthread unblock([&] { QThread::msleep(50); release.release(2); });
    preview.reset(); for (auto &task : blockers) task.waitForFinished(); QCoreApplication::processEvents();
    QVERIFY(running); QCOMPARE(requested.count(), 0); QVERIFY(!context->audioDevice->isStarted());
}

void EditorIntegrationTests::audioProcessingIsBoundedAndDoesNotBlockStorage()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    QSemaphore started, release;
    QVector<QFuture<void>> jobs;
    for (int i = 0; i < 3; ++i) jobs.append(context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }));
    const bool twoStarted = started.tryAcquire(2, 2000);
    const bool thirdStartedEarly = started.tryAcquire(1, 50);
    std::atomic<bool> saved{false};
    auto storage = context->submitProjectStorage([&] { saved = true; });
    storage.waitForFinished();
    const bool independent = saved.load();
    release.release(3);
    for (auto &job : jobs) job.waitForFinished();
    QVERIFY(twoStarted); QVERIFY(!thirdStartedEarly); QVERIFY(independent);
}
void EditorIntegrationTests::voiceReviewDestructionCancelsQueuedAudio()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const QString path = directory.filePath(QStringLiteral("cancel-review.sqlite"));
    VoiceLibrary library(path); QVERIFY(library.initialize());
    const auto profile = library.createProfile(QStringLiteral("取消测试")); QVERIFY(profile);
    const auto version = library.addVersion(*profile, {{QStringLiteral("referenceText"), QStringLiteral("转写")}}, mediaPath(QStringLiteral("audio.wav"))); QVERIFY(version);
    QSemaphore started, release;
    QVector<QFuture<void>> blockers;
    for (int i = 0; i < 2; ++i) blockers.append(context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }));
    const bool running = started.tryAcquire(2, 2000);
    auto voices = std::make_unique<VoiceLibraryController>(context.get(), &session, path);
    QSignalSpy finished(voices.get(), &VoiceLibraryController::operationFinished);
    voices->inspectVersion(*version);
    QTRY_VERIFY(voices->statusText().contains(QStringLiteral("解码")));
    std::jthread unblock([&] { QThread::msleep(50); release.release(2); });
    voices.reset();
    for (auto &future : blockers) future.waitForFinished();
    QCoreApplication::processEvents();
    QVERIFY(running); QCOMPARE(finished.count(), 0); QCOMPARE(library.versions(*profile).size(), 1);
}

void EditorIntegrationTests::voiceReferenceReviewCreatesNewVersion()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const QString path = directory.filePath(QStringLiteral("review.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path);
    QSignalSpy finished(&voices, &VoiceLibraryController::operationFinished);
    voices.createProfile(QStringLiteral("待审旁白")); QTRY_COMPARE(finished.count(), 1);
    voices.addLocalVersion(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), QStringLiteral("手工转写"));
    QTRY_COMPARE(finished.count(), 2); QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    const QString pending = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    voices.inspectVersion(pending); QTRY_COMPARE(finished.count(), 3);
    QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    QVERIFY(!voices.review().isEmpty());
    voices.approveReview(QStringLiteral("审核转写"), true); QTRY_COMPARE(finished.count(), 4);
    QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText())); QCOMPARE(voices.versions().size(), 2);
    VoiceLibrary library(path);
    QCOMPARE(library.version(pending)->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("qualityState")).toString(), QStringLiteral("pending"));
    bool approved = false;
    for (const auto &value : voices.versions()) {
        const auto metadata = value.toMap().value(QStringLiteral("metadata")).toMap();
        if (metadata.value(QStringLiteral("qualityState")).toString() == QStringLiteral("approved")) {
            approved = true; QCOMPARE(metadata.value(QStringLiteral("sourceVersionId")).toString(), pending);
            QCOMPARE(metadata.value(QStringLiteral("referenceText")).toString(), QStringLiteral("审核转写"));
            QCOMPARE(metadata.value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("Chinese"));
        }
    }
    QVERIFY(approved);
    voices.approveReview(QStringLiteral("手工审核的其它语言转写"), true, QStringLiteral("Japanese"), QStringLiteral("happy")); QTRY_COMPARE(finished.count(), 5);
    QVERIFY(finished.last().first().toBool()); QCOMPARE(library.versions(voices.selectedProfile()).size(), 3);
    bool japanese = false;
    for (const auto &value : voices.versions()) japanese = japanese || value.toMap().value(QStringLiteral("metadata")).toMap().value(QStringLiteral("referenceLanguage")).toString() == QStringLiteral("Japanese");
    QVERIFY(japanese); voices.approveReview(QStringLiteral("转写"), true, QStringLiteral("Auto")); QVERIFY(voices.statusText().contains(QStringLiteral("参考语言")));
    QCOMPARE(library.versions(voices.selectedProfile()).size(), 3);
    voices.approveReview(QStringLiteral("转写"), true, QStringLiteral("Chinese"), QStringLiteral("invalid"));
    QVERIFY(voices.statusText().contains(QStringLiteral("情绪"))); QCOMPARE(finished.count(), 5);
    QCOMPARE(library.versions(voices.selectedProfile()).size(), 3);
    QVERIFY(!library.version(pending)->value(QStringLiteral("metadata")).toObject().contains(QStringLiteral("manualEmotion")));
    bool tagged = false;
    for (const auto &value : voices.versions()) tagged = tagged || value.toMap().value(QStringLiteral("metadata")).toMap().value(QStringLiteral("manualEmotion")).toString() == QStringLiteral("happy");
    QVERIFY(tagged);
}

void EditorIntegrationTests::voiceBatchAndDerivedVersionsPreserveManualReview()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const QString path = directory.filePath(QStringLiteral("batch.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path);
    QSignalSpy finished(&voices, &VoiceLibraryController::operationFinished);
    voices.createProfile(QStringLiteral("批量旁白")); QTRY_COMPARE(finished.count(), 1);
    voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
    QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText())); QVERIFY(!voices.samples().isEmpty());
    voices.saveSamples(); QVERIFY(voices.statusText().contains(QStringLiteral("转写"))); QCOMPARE(finished.count(), 2);
    for (int i = 0; i < voices.samples().size(); ++i) voices.editSample(i, QStringLiteral("手工核对的转写"), true);
    voices.saveSamples(); QTRY_COMPARE(finished.count(), 3); QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    VoiceLibrary library(path); const auto count = library.versions(voices.selectedProfile()).size(); QVERIFY(count >= 2);
    voices.saveSamples(); QTRY_COMPARE(finished.count(), 4); QCOMPARE(library.versions(voices.selectedProfile()).size(), count);
    const QString pending = voices.samples().first().toMap().value(QStringLiteral("savedVersionId")).toString(); QVERIFY(!pending.isEmpty());
    voices.inspectVersion(pending); QTRY_COMPARE(finished.count(), 5); QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    const auto before = library.version(pending);
    voices.deriveReview(0, 0.4, true, true); QTRY_COMPARE(finished.count(), 6); QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    QVERIFY(!voices.derived().isEmpty()); voices.approveDerived(QStringLiteral("裁切后重新核对"), true);
    QCOMPARE(finished.count(), 6); QVERIFY(voices.statusText().contains(QStringLiteral("重新核对")));
    voices.approveDerived(QStringLiteral("裁切后重新核对"), true, true, QStringLiteral("Chinese"), QStringLiteral("neutral"));
    QTRY_COMPARE(finished.count(), 7); QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    QCOMPARE(library.version(pending), before); QCOMPARE(library.versions(voices.selectedProfile()).size(), count + 1);
    QVERIFY(!session.modified());
    bool tagged = false;
    for (const auto &value : voices.versions()) tagged = tagged || value.toMap().value(QStringLiteral("metadata")).toMap().value(QStringLiteral("manualEmotion")).toString() == QStringLiteral("neutral");
    QVERIFY(tagged);
}

void EditorIntegrationTests::voiceBatchDestructionCancelsAndRemovesTemporarySamples()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    auto voices = std::make_unique<VoiceLibraryController>(context.get(), &session, directory.filePath(QStringLiteral("cancel-batch.sqlite")));
    voices->createProfile(QStringLiteral("临时素材")); QTRY_VERIFY(!voices->busy());
    voices->prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))});
    QTRY_VERIFY(!voices->busy()); QVERIFY(!voices->samples().isEmpty());
    const QString temporary = QFileInfo(voices->samples().first().toMap().value(QStringLiteral("referencePath")).toString()).absolutePath();
    QVERIFY(QDir(temporary).exists());
    QSemaphore started, release; QVector<QFuture<void>> blockers;
    for (int i = 0; i < 2; ++i) blockers.append(context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }));
    QVERIFY(started.tryAcquire(2, 2000));
    voices->prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))});
    QVERIFY(!QDir(temporary).exists());
    QSignalSpy finished(voices.get(), &VoiceLibraryController::operationFinished);
    std::jthread unblock([&] { QThread::msleep(50); release.release(2); });
    voices.reset(); for (auto &future : blockers) future.waitForFinished(); QCoreApplication::processEvents();
    QCOMPARE(finished.count(), 0);
}

void EditorIntegrationTests::voiceBatchMissingAsrPreservesManualPathAndBindings()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    context->settings.insert(QStringLiteral("asrProvider"), QStringLiteral("qwen3"));
    context->settings.insert(QStringLiteral("modelsRoot"), directory.filePath(QStringLiteral("absent-models")));
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const auto path = directory.filePath(QStringLiteral("asr-failure.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path); QSignalSpy finished(&voices, &VoiceLibraryController::operationFinished);
    voices.createProfile(QStringLiteral("离线档案")); QTRY_COMPARE(finished.count(), 1);
    voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))}); QTRY_COMPARE(finished.count(), 2);
    const auto before = voices.samples(); voices.transcribeSamples(); QTRY_COMPARE(finished.count(), 3);
    QVERIFY(!finished.last().first().toBool()); QVERIFY(voices.statusText().contains(QStringLiteral("模型")));
    QCOMPARE(voices.samples().first().toMap().value(QStringLiteral("transcript")), before.first().toMap().value(QStringLiteral("transcript")));
    voices.editSample(0, QStringLiteral("手工填写仍可用"), true); voices.saveSamples(); QTRY_COMPARE(finished.count(), 4);
    QVERIFY2(finished.last().first().toBool(), qPrintable(voices.statusText()));
    const auto cloudAccount = configureUnifiedSynthesis(context.get()); QVERIFY(!cloudAccount.isEmpty());
    voices.addCloudVersion(QStringLiteral("yuxiaoyun_v3.1")); QTRY_COMPARE(finished.count(), 5);
    const auto id = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    VoiceLibrary library(path); const auto oldVersion = library.version(id); QVERIFY(oldVersion);
    voices.savePreset(id, QStringLiteral("中文旁白"), QStringLiteral("用户自有素材，仅用于本项目旁白")); QTRY_COMPARE(finished.count(), 6);
    QVERIFY(finished.last().first().toBool()); QCOMPARE(library.version(id), oldVersion);
    const auto metadata = voices.versions().first().toMap().value(QStringLiteral("metadata")).toMap();
    QCOMPARE(metadata.value(QStringLiteral("businessSpace")).toString(), QStringLiteral("mock-space"));
    QCOMPARE(metadata.value(QStringLiteral("cloudAccountId")).toString(), cloudAccount);
    QVERIFY(!metadata.contains(QStringLiteral("credentialReference")));
    QCOMPARE(metadata.value(QStringLiteral("preset")).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("中文旁白"));
    QVERIFY(!session.modified());
}

void EditorIntegrationTests::voiceRealChineseSamplesWhenRequested()
{
    const QString root = qEnvironmentVariable("SUBCUE_P3_REAL_SAMPLES");
    if (root.isEmpty()) QSKIP("真实录音验收须显式提供本地样本目录");
    QTemporaryDir directory; auto context = makeContext(directory);
    context->settings.insert(QStringLiteral("asrProvider"), QStringLiteral("qwen3"));
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("real-samples.sqlite")));
    const QString output = sourcePath(QStringLiteral(".test_tmp/p3-real-") + QUuid::createUuid().toString(QUuid::WithoutBraces)); QVERIFY(QDir().mkpath(output));
    QJsonArray report;
    const QStringList files{QDir(root).filePath(QStringLiteral("男声/中文音频1.mp4")), QDir(root).filePath(QStringLiteral("女声/中文/电量不足.mp3"))};
    for (int file = 0; file < files.size(); ++file) {
        QVERIFY(QFileInfo::exists(files[file]));
        QString error; const auto original = AudioQuality::inspect(files[file], &error); QVERIFY2(original, qPrintable(error));
        voices.createProfile(QStringLiteral("真实录音验收 %1").arg(file + 1)); QTRY_VERIFY(!voices.busy());
        voices.prepareSamples({QUrl::fromLocalFile(files[file])}); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 60000);
        QVERIFY2(!voices.samples().isEmpty(), qPrintable(voices.statusText()));
        voices.transcribeSamples(); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 600000);
        QVERIFY2(!voices.statusText().contains(QStringLiteral("失败")) && !voices.statusText().contains(QStringLiteral("不完整")), qPrintable(voices.statusText()));
        QJsonArray samples;
        for (int i = 0; i < voices.samples().size(); ++i) {
            auto sample = voices.samples()[i].toMap();
            const auto text = sample.value(QStringLiteral("transcript")).toString().trimmed();
            sample.remove(QStringLiteral("referencePath"));
            if (text.isEmpty()) voices.editSample(i, {}, false);
            samples.append(QJsonObject::fromVariantMap(sample));
        }
        bool recognized = false;
        for (const auto &sample : samples) recognized = recognized || !sample.toObject().value(QStringLiteral("transcript")).toString().isEmpty();
        QVERIFY2(recognized, qPrintable(voices.statusText()));
        voices.saveSamples(); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 60000);
        bool derived = false;
        for (const auto &value : voices.samples()) {
            const auto sample = value.toMap(); const auto id = sample.value(QStringLiteral("savedVersionId")).toString();
            if (id.isEmpty()) continue;
            voices.inspectVersion(id); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 60000); QVERIFY(!voices.review().isEmpty());
            const double duration = sample.value(QStringLiteral("endSeconds")).toDouble() - sample.value(QStringLiteral("startSeconds")).toDouble();
            const QString prefix = QStringLiteral("speaker-%1").arg(file + 1);
            const QString a = QDir(output).filePath(prefix + QStringLiteral("-A.wav"));
            QVERIFY(QFile::copy(voices.review().value(QStringLiteral("referencePath")).toString(), a));
            voices.deriveReview(0, std::min(12.0, duration), true, true); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 60000);
            QVERIFY2(!voices.derived().isEmpty(), qPrintable(voices.statusText()));
            const QString b = QDir(output).filePath(prefix + QStringLiteral("-B.wav"));
            QVERIFY(QFile::copy(voices.derived().value(QStringLiteral("referencePath")).toString(), b));
            report.append(QJsonObject{{QStringLiteral("source"), files[file]}, {QStringLiteral("originalQuality"), original->toJson()},
                {QStringLiteral("samples"), samples}, {QStringLiteral("reviewTranscript"), sample.value(QStringLiteral("transcript")).toString()},
                {QStringLiteral("A"), a}, {QStringLiteral("B"), b}, {QStringLiteral("derivedQuality"), QJsonObject::fromVariantMap(voices.derived().value(QStringLiteral("quality")).toMap())},
                {QStringLiteral("humanApproved"), false}});
            derived = true; break;
        }
        QVERIFY(derived); QCOMPARE(AudioQuality::inspect(files[file])->sha256, original->sha256);
    }
    QFile summary(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(summary.open(QIODevice::WriteOnly));
    QVERIFY(summary.write(QJsonDocument(report).toJson()) > 0);
    qInfo().noquote() << QStringLiteral("P3 真实录音报告：") << summary.fileName();
}

void EditorIntegrationTests::voiceRealExternalEngineWhenRequested()
{
    const auto engine = qEnvironmentVariable("SUBCUE_L1_ENGINE"), output = qEnvironmentVariable("SUBCUE_L1_OUTPUT");
    if (output.isEmpty()) QSKIP("后续真实引擎须显式配置运行目录，不自动加载模型。");
    QVERIFY(engine == QStringLiteral("index") || engine == QStringLiteral("gpt")); QVERIFY(QDir().mkpath(output));
    const auto python = engine == QStringLiteral("index") ? QStringLiteral("E:/AIModels/TTS/.venv-index/Scripts/python.exe") : QStringLiteral("E:/AIModels/TTS/.venv-gpt/Scripts/python.exe");
    const auto model = engine == QStringLiteral("index") ? QStringLiteral("E:/AIModels/TTS/models/IndexTTS-2.5") : QStringLiteral("E:/AIModels/TTS/models/GPT-SoVITS");
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks);
    session.newProject(); voice.previewText(qEnvironmentVariable("SUBCUE_L1_TEXT", QStringLiteral("今天我们用清晰自然的声音，记录生活中的小变化。"))); voice.applyPreview();
    const auto path = QDir(output).filePath(QStringLiteral("real.subcue-project")); QVERIFY(!QFileInfo::exists(path));
    QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    voice.checkLocal(engine, python, model); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 600000); QVERIFY2(!voice.qwenStatus().isEmpty(), qPrintable(voice.statusText()));
    const auto reference = sourcePath(QStringLiteral("out/validation/voice-p3-20261013/speaker-2-B.wav")); const auto quality = AudioQuality::inspect(reference); QVERIFY(quality); QString error;
    const auto voiceId = QUuid::createUuid().toString(QUuid::WithoutBraces), versionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY2(ProjectStore(path).importVoiceVersion(voiceId, versionId, reference, quality->sha256,
        {{QStringLiteral("qualityState"), QStringLiteral("approved")}, {QStringLiteral("quality"), quality->toJson()},
         {QStringLiteral("referenceText"), QStringLiteral("感觉耗电耗得更快了")}, {QStringLiteral("humanReview"), QStringLiteral("用户此前确认本轮克隆验收参考")}}, &error), qPrintable(error));
    voice.bindVoiceVersion(versionId); QTRY_VERIFY(!voice.busy());
    if (engine == QStringLiteral("index") && qEnvironmentVariableIsSet("SUBCUE_L1_CONTROLS")) {
        QVERIFY(voice.commitDraft({{QStringLiteral("id"), voice.selectedId()}, {QStringLiteral("text"), voice.selected().value(QStringLiteral("text"))},
            {QStringLiteral("spoken"), QStringLiteral("今天我们用清晰自然的声音，记录生活中的小<变化|BIAN4 HUA4>。")}, {QStringLiteral("manualSpoken"), true}, {QStringLiteral("notes"), QString()},
            {QStringLiteral("gain"), 1.0}, {QStringLiteral("pauseMs"), 300.0}, {QStringLiteral("seed"), QStringLiteral("12345")},
            {QStringLiteral("indexParameters"), true}, {QStringLiteral("durationFactor"), 1.15}, {QStringLiteral("emotion"), QStringLiteral("happy")}, {QStringLiteral("emotionStrength"), 0.2}}));
    }
    if (engine == QStringLiteral("gpt") && qEnvironmentVariableIsSet("SUBCUE_L1_GPT_SPEED")) {
        QVERIFY(voice.commitDraft({{QStringLiteral("id"), voice.selectedId()}, {QStringLiteral("text"), voice.selected().value(QStringLiteral("text"))},
            {QStringLiteral("spoken"), voice.selected().value(QStringLiteral("spoken"))}, {QStringLiteral("manualSpoken"), false}, {QStringLiteral("notes"), QString()},
            {QStringLiteral("gain"), 1.0}, {QStringLiteral("pauseMs"), 300.0}, {QStringLiteral("seed"), QStringLiteral("12345")},
            {QStringLiteral("gptParameters"), true}, {QStringLiteral("speedFactor"), qEnvironmentVariable("SUBCUE_L1_GPT_SPEED").toDouble()}}));
    }
    QVERIFY(voice.commitDraft({{QStringLiteral("id"), voice.selectedId()}, {QStringLiteral("text"), voice.selected().value(QStringLiteral("text"))},
        {QStringLiteral("spoken"), voice.selected().value(QStringLiteral("spoken"))}, {QStringLiteral("notes"), QString()}, {QStringLiteral("manualSpoken"), voice.selected().value(QStringLiteral("manualSpoken"))},
        {QStringLiteral("gain"), 1.0}, {QStringLiteral("pauseMs"), 300.0}, {QStringLiteral("seed"), QStringLiteral("12345")}}));
    QSignalSpy generated(&tasks, &TtsTaskController::finished), committed(&tasks, &TtsTaskController::candidateCommitted);
    voice.generateLocal(engine, python, model, QStringLiteral("selected")); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 600000); QVERIFY2(generated.last().first().toBool(), qPrintable(generated.last().last().toString()));
    QCOMPARE(committed.count(), 1); QVERIFY(!committed.first()[3].toBool()); QVERIFY(AudioQuality::inspect(committed.first()[2].toString()));
    QTRY_VERIFY(!voice.busy()); voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    QVERIFY(!voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty());
    const auto selectedTake = voice.selected().value(QStringLiteral("selectedTake")).toMap();
    voice.setArrangement(0.8, 0, QStringLiteral("10"), selectedTake.value(QStringLiteral("sampleCount")).toString());
    const auto edited = voice.snapshot();
    voice.generateLocal(engine == QStringLiteral("index") ? QStringLiteral("gpt") : QStringLiteral("index"), python, model, QStringLiteral("selected"));
    QVERIFY(!tasks.busy()); QCOMPARE(voice.snapshot(), edited);
    voice.generateLocal(engine, python, model, QStringLiteral("selected")); QTRY_VERIFY_WITH_TIMEOUT(tasks.busy(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(ProjectStore(path).jobs().first().toObject().value(QStringLiteral("state")).toString() == QStringLiteral("running"), 600000);
    tasks.cancel(); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 2, 60000); QVERIFY(!generated.last().first().toBool());
    QTRY_VERIFY(!voice.busy()); QCOMPARE(ProjectStore(path).history().size(), 1); QCOMPARE(voice.snapshot(), edited);
    QVERIFY(QDir(output).entryList({QStringLiteral(".subcue-tts-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2);
    const auto rows = ProjectStore(path).jobs(); QFile audit(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("engine"), engine}, {QStringLiteral("preflight"), QJsonObject::fromVariantMap(voice.qwenStatus())},
        {QStringLiteral("jobs"), rows}, {QStringLiteral("takes"), ProjectStore(path).history()}}).toJson());
}
void EditorIntegrationTests::voiceLongNarrationStressWhenRequested()
{
    const auto reportPath = qEnvironmentVariable("SUBCUE_P8_REPORT");
    if (reportPath.isEmpty()) QSKIP("长文压力验证须显式指定报告路径，不自动生成真实配音。");
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get()); ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session); AudioPreviewController preview(context.get(), &session); session.newProject();
    QString text;
    for (int i = 0; i < 500; ++i) text += QStringLiteral("这是中文长文旁白压力测试保留每句独立编辑。\n");
    QElapsedTimer timer; timer.start(); voice.previewText(text); voice.applyPreview();
    QJsonObject report{{QStringLiteral("characters"), text.size()}, {QStringLiteral("segments"), voice.rowCount()}, {QStringLiteral("importMs"), timer.elapsed()}, {QStringLiteral("syntheticAudio"), true}};
    QCOMPARE(voice.rowCount(), 500);
    const auto path = directory.filePath(QStringLiteral("stress.subcue-project")); QSignalSpy saved(&session, &ProjectSession::saved);
    timer.restart(); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 60000);
    report.insert(QStringLiteral("initialSaveMs"), timer.elapsed());
    const auto source = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(source); QVERIFY(quality);
    auto document = voice.snapshot(); const auto items = document.value(QStringLiteral("segments")).toArray(); ProjectStore store(path);
    timer.restart();
    for (const auto &value : items) {
        const auto item = value.toObject(); QString take;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto job = QUuid::createUuid().toString(); take = QUuid::createUuid().toString();
            QVERIFY(store.enqueue(job, {{QStringLiteral("segmentId"), item.value(QStringLiteral("id"))}})); QVERIFY(store.finish(job, take, source));
        }
        QString error;
        document = VoiceDocument::applyTake(document, item.value(QStringLiteral("id")).toString(), {{QStringLiteral("id"), take}, {QStringLiteral("sampleRate"), quality->sampleRate},
            {QStringLiteral("sampleCount"), QString::number(quality->sampleCount)}, {QStringLiteral("sha256"), QString::fromLatin1(quality->sha256.toHex())},
            {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex())}}, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    report.insert(QStringLiteral("fixtureHistoryMs"), timer.elapsed()); voice.reset(document);
    QCOMPARE(store.history(1490, 50).size(), 10); QCOMPARE(store.segmentTakes(items[0].toObject().value(QStringLiteral("id")).toString()).size(), 3);
    QVector<qint64> timings; timings.reserve(100);
    for (int i = 0; i < 100; ++i) {
        timer.restart(); voice.select(items[i * 5].toObject().value(QStringLiteral("id")).toString()); (void)voice.selected(); QCoreApplication::processEvents(); timings.append(timer.nsecsElapsed());
    }
    std::sort(timings.begin(), timings.end()); report.insert(QStringLiteral("controllerSelectionP95Ms"), double(timings[94]) / 1000000.0);
    timer.restart(); QVERIFY(session.saveCurrentProject()); QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 60000); report.insert(QStringLiteral("saveWithHistoryMs"), timer.elapsed());
    VoiceAudioController audio(context.get(), &session, &voice, &preview); audio.setActive(true);
    timer.restart(); audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 180000);
    QVERIFY2(audio.rows().size() == 500, qPrintable(audio.statusText())); report.insert(QStringLiteral("previewMs"), timer.elapsed());
    QVERIFY(audio.waveformBytes() <= 64LL * 1024 * 1024); QVERIFY(audio.cacheBytes() <= 1024LL * 1024 * 1024);
    report.insert(QStringLiteral("waveformBytes"), audio.waveformBytes()); report.insert(QStringLiteral("cacheBytes"), audio.cacheBytes()); audio.stop();
    const auto output = directory.filePath(QStringLiteral("narration.wav")); timer.restart(); audio.exportWav(QUrl::fromLocalFile(output)); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 180000);
    QVERIFY2(QFileInfo::exists(output), qPrintable(audio.statusText())); report.insert(QStringLiteral("exportMs"), timer.elapsed());
    const auto exported = AudioQuality::inspect(output); QVERIFY(exported); QCOMPARE(exported->sampleRate, 48000); QCOMPARE(exported->channels, 1);
    report.insert(QStringLiteral("exportSamples"), QString::number(exported->sampleCount));
    // 长文与历史一起备份、恢复，不能只验证原会话内的试听和导出。
    const auto frozen = voice.snapshot();
    const auto backup = directory.filePath(QStringLiteral("narration-backup.subcue-project"));
    QSignalSpy backedUp(&session, &ProjectSession::backedUp);
    timer.restart(); QVERIFY(session.backupProject(QUrl::fromLocalFile(backup), true));
    QTRY_COMPARE_WITH_TIMEOUT(backedUp.count(), 1, 60000);
    QVERIFY2(backedUp.first().first().toBool(), qPrintable(session.statusText()));
    report.insert(QStringLiteral("portableBackupMs"), timer.elapsed());
    const auto restored = directory.filePath(QStringLiteral("restored.subcue-project"));
    QSignalSpy opened(&session, &ProjectSession::opened);
    timer.restart(); QVERIFY(session.restoreProject(QUrl::fromLocalFile(backup), QUrl::fromLocalFile(restored)));
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 60000);
    QVERIFY2(opened.first().first().toBool(), qPrintable(session.statusText()));
    report.insert(QStringLiteral("restoreMs"), timer.elapsed());
    QCOMPARE(voice.snapshot(), frozen); QVERIFY(!session.modified());
    QCOMPARE(ProjectStore(restored).history(1490, 50).size(), 10);
    for (const auto &value : items) {
        const auto history = ProjectStore(restored).segmentTakes(value.toObject().value(QStringLiteral("id")).toString());
        QCOMPARE(history.size(), 3);
        for (const auto &take : history) {
            const auto owned = ProjectStore(restored).resolveAudioPath(take.toObject().value(QStringLiteral("audioPath")).toString());
            QVERIFY2(QFileInfo::exists(owned), qPrintable(owned));
            QVERIFY(owned != source);
            QVERIFY2(QDir::cleanPath(owned).startsWith(QDir::cleanPath(restored + QStringLiteral(".assets")) + '/'), qPrintable(owned));
        }
    }
    const auto restoredOutput = directory.filePath(QStringLiteral("restored-narration.wav"));
    timer.restart(); audio.exportWav(QUrl::fromLocalFile(restoredOutput));
    QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 180000);
    const auto restoredAudio = AudioQuality::inspect(restoredOutput);
    QVERIFY2(restoredAudio, qPrintable(audio.statusText()));
    QCOMPARE(restoredAudio->sha256, exported->sha256);
    report.insert(QStringLiteral("restoredExportMs"), timer.elapsed());
    report.insert(QStringLiteral("restoredAudioMatches"), true);
    QFile reportFile(reportPath); QVERIFY(reportFile.open(QIODevice::WriteOnly)); QVERIFY(reportFile.write(QJsonDocument(report).toJson()) > 0);
}
void EditorIntegrationTests::voiceFlashConnectionDoesNotSynthesizeOrRevealCredentials()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    AppController editor(context.get()); RoughCutController roughCut(context.get()); ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session);
    const auto reference = QStringLiteral("SubCue/VoiceStudio/ConnectionTest");
    QVERIFY(!voice.saveFlashCredential(reference, QStringLiteral("private-test-key")));
    QVERIFY(!context->credentials.exists(reference));
    voice.testFlashConnection(QStringLiteral("test-space"), reference, QStringLiteral("private-test-key"));
    QCOMPARE(http.sendCount, 0); QVERIFY(!voice.statusText().contains(QStringLiteral("private-test-key")));
    QVERIFY(voice.statusText().contains(QStringLiteral("已停用")));
}
void EditorIntegrationTests::voiceFlashUsageAndUnknownResultsAreProtected()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    QFile fixture(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(fixture.open(QIODevice::ReadOnly)); http.downloadBody = fixture.readAll();
    http.responses.append(HttpResponse{200, {}, QJsonDocument(QJsonObject{{QStringLiteral("request_id"), QStringLiteral("test-flash-request")},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input_tokens"), 100}, {QStringLiteral("output_tokens"), 1000}}},
        {QStringLiteral("output"), QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("stop")}, {QStringLiteral("audio"), QJsonObject{{QStringLiteral("url"), QStringLiteral("http://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/mock.wav")}, {QStringLiteral("expires_at"), 4102444800LL}}}}}}).toJson()});
    AppController editor(context.get()); RoughCutController roughCut(context.get()); ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    voice.previewText(QStringLiteral("第一句旁白。第二句旁白。")); voice.applyPreview(); QSignalSpy saved(&session, &ProjectSession::saved);
    const auto path = directory.filePath(QStringLiteral("cloud.subcue-project")); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces), version = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto account = configureUnifiedSynthesis(context.get()); QVERIFY(!account.isEmpty());
    QVERIFY(ProjectStore(path).importVoiceVersion(id, version, {}, {}, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("model"), QStringLiteral("qwen-audio-3.1-tts-flash")},
        {QStringLiteral("region"), QStringLiteral("beijing")}, {QStringLiteral("providerVoiceId"), QStringLiteral("test-voice")}, {QStringLiteral("businessSpace"), QStringLiteral("mock-space")}, {QStringLiteral("cloudAccountId"), account}}));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.selected().value(QStringLiteral("voiceVersionId")).toString(), version);
    voice.prepareFlash(QStringLiteral("all"), 1.0); QTRY_VERIFY(!voice.busy()); QCOMPARE(http.sendCount, 0); QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 2);
    QVERIFY(!voice.flashPreview().contains(QStringLiteral("calibrationNeeded"))); QVERIFY(!voice.flashPreview().contains(QStringLiteral("estimatedYuan")));
    for (const auto &row : voice.flashPreview().value(QStringLiteral("rows")).toList()) QVERIFY(!row.toMap().contains(QStringLiteral("estimatedYuan")));
    voice.discardFlash(); voice.prepareFlash(QStringLiteral("selected"), 1.0); QTRY_VERIFY(!voice.busy());
    QSignalSpy finished(&tasks, &TtsTaskController::finished);
    http.blockUntilCancel = true; voice.confirmFlash(); QTRY_VERIFY(http.inFlight.load());
    CloudSettingsController cloud(context.get()); cloud.begin();
    QVERIFY(cloud.stageKey(account, QStringLiteral("rotated-TTS-key"))); QVERIFY(cloud.save());
    QVERIFY(http.inFlight.load()); QCOMPARE(http.requests.first().headers.front().value, QByteArrayLiteral("Bearer fake-TTS-secret-not-ASR"));
    http.blockUntilCancel = false;
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000); QVERIFY2(finished.last().first().toBool(), qPrintable(finished.last().last().toString())); QCOMPARE(http.sendCount, 1);
    QTRY_VERIFY(!voice.busy()); QVERIFY(voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty());
    const auto first = ProjectStore(path).jobs().first().toObject(); QCOMPARE(first.value(QStringLiteral("usage")).toObject().value(QStringLiteral("yuan")).toDouble(), 0.01215); QCOMPARE(first.value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
    QVERIFY(!first.value(QStringLiteral("input")).toObject().value(QStringLiteral("billing")).toObject().contains(QStringLiteral("estimatedYuan")));
    QCOMPARE(first.value(QStringLiteral("usage")).toObject().value(QStringLiteral("inputTokens")).toInteger(), 100);
    QCOMPARE(first.value(QStringLiteral("usage")).toObject().value(QStringLiteral("outputTokens")).toInteger(), 1000);
    voice.prepareFlash(QStringLiteral("all"), 1.0); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), 1); QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 1);
    voice.confirmFlash(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000); QCOMPARE(http.sendCount, 2); QVERIFY(finished.last().first().toBool());
    QCOMPARE(http.requests.last().headers.front().value, QByteArrayLiteral("Bearer rotated-TTS-key"));
    voice.prepareFlash(QStringLiteral("all"), 1.0); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 0); voice.confirmFlash(); QCOMPARE(http.sendCount, 2);
    const auto protectedDocument = voice.snapshot();
    cloud.begin(); QString boundModel;
    for (const auto &row : cloud.models()) if (row.toMap().value(QStringLiteral("accountId")).toString() == account) boundModel = row.toMap().value(QStringLiteral("id")).toString();
    QVERIFY(!boundModel.isEmpty()); cloud.editModel(boundModel, {{QStringLiteral("workspace"), QStringLiteral("other-space")}}); QVERIFY(cloud.save());
    voice.prepareFlash(QStringLiteral("all"), 1.0); QTRY_VERIFY(!voice.busy());
    QVERIFY(voice.flashPreview().isEmpty()); QVERIFY(voice.statusText().contains(QStringLiteral("不匹配")));
    QCOMPARE(http.sendCount, 2); QCOMPARE(voice.snapshot(), protectedDocument);
    cloud.begin(); cloud.editModel(boundModel, {{QStringLiteral("workspace"), QStringLiteral("mock-space")}}); QVERIFY(cloud.save());
    voice.prepareFlash(QStringLiteral("all"), 1.0); QTRY_VERIFY(!voice.busy());
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty()); voice.confirmFlash();
    QVERIFY(voice.flashPreview().isEmpty()); QCOMPARE(http.sendCount, 2); QCOMPARE(voice.snapshot(), protectedDocument);
    cloud.begin(); cloud.bindModel(QStringLiteral("speechSynthesis"), boundModel); QVERIFY(cloud.save());
    voice.editSegment(voice.selectedId(), QStringLiteral("新的朗读文本。"), {}, false, {}); voice.prepareFlash(QStringLiteral("selected"), 1.0); QTRY_VERIFY(!voice.busy());
    http.blockUntilCancel = true; voice.confirmFlash(); QTRY_VERIFY(http.inFlight.load()); cloud.begin(); cloud.removeKey(account); QVERIFY(cloud.save());
    QVERIFY(!http.inFlight.load()); QVERIFY(!tasks.busy()); QCOMPARE(http.sendCount, 3);
    const auto unknown = ProjectStore(path).jobs().first().toObject(); QCOMPARE(unknown.value(QStringLiteral("state")).toString(), QStringLiteral("outcome_unknown")); QCOMPARE(unknown.value(QStringLiteral("usage")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    ProjectStore::JobOptions options; options.provider = QStringLiteral("qwen-cloud"); options.cloud = true; options.inputHash = QByteArray::fromHex(unknown.value(QStringLiteral("inputHash")).toString().toLatin1());
    QVERIFY(!ProjectStore(path).enqueue(QStringLiteral("no-automatic-resend"), unknown.value(QStringLiteral("input")).toObject(), options));
    auto paid = TtsRequest::fromSnapshot(unknown.value(QStringLiteral("input")).toObject(), CloudTtsProvider(QStringLiteral("mock-space"), {}, 1.0).capabilities()); QVERIFY(paid);
    paid->spokenText = QStringLiteral("验证已经计费但下载失败的保护。"); options.inputHash = paid->inputHash();
    QVERIFY(ProjectStore(path).enqueue(QStringLiteral("paid-download-failure"), paid->snapshot(), options));
    QVERIFY(ProjectStore(path).transitionJob(QStringLiteral("paid-download-failure"), ProjectStore::JobState::Queued, ProjectStore::JobState::Running));
    QVERIFY(ProjectStore(path).recordJobUsage(QStringLiteral("paid-download-failure"), first.value(QStringLiteral("usage")).toObject()));
    QVERIFY(ProjectStore(path).transitionJob(QStringLiteral("paid-download-failure"), ProjectStore::JobState::Running, ProjectStore::JobState::Failed));
    QVERIFY(!ProjectStore(path).enqueue(QStringLiteral("no-paid-resend"), paid->snapshot(), options));
    options.manualRetry = true; options.retryOf = QStringLiteral("paid-download-failure");
    QVERIFY(ProjectStore(path).enqueue(QStringLiteral("explicit-paid-retry"), paid->snapshot(), options));
    QFile database(path); QVERIFY(database.open(QIODevice::ReadOnly)); QVERIFY(!database.readAll().contains("fake-TTS-secret-not-ASR"));
    cloud.begin(); QVERIFY(cloud.stageKey(account, QStringLiteral("restored-TTS-key"))); QVERIFY(cloud.save());
    voice.prepareFlash(QStringLiteral("selected"), 1.0); QTRY_VERIFY(!voice.busy()); voice.editSegment(voice.selectedId(), QStringLiteral("预览后继续编辑。"), {}, false, {}); voice.confirmFlash(); QCOMPARE(http.sendCount, 3); QVERIFY(!tasks.busy());
}

void EditorIntegrationTests::voiceQwenMissingEnvironmentPreservesEditing()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    voice.previewText(QStringLiteral("无模型时仍可编辑并保存。")); voice.applyPreview(); const auto before = voice.snapshot();
    voice.generateQwen(directory.filePath(QStringLiteral("missing-runtime")), directory.path(), QStringLiteral("all")); QVERIFY(!tasks.busy()); QCOMPARE(voice.snapshot(), before);
    voice.checkQwen(directory.filePath(QStringLiteral("missing-runtime")), directory.path()); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 10000); QVERIFY(voice.qwenStatus().isEmpty()); QVERIFY(!voice.statusText().isEmpty()); QCOMPARE(voice.snapshot(), before);
    QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("editable.subcue-project"))))); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
}

void EditorIntegrationTests::voiceCrossLanguageWhenRequested()
{
    const auto output = qEnvironmentVariable("SUBCUE_L2_OUTPUT");
    if (output.isEmpty()) QSKIP("跨语言真实验收须显式设置输出目录，不自动加载模型。");
    QVERIFY(QDir().mkpath(output)); QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get()); ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    const auto engine = qEnvironmentVariable("SUBCUE_L2_ENGINE", QStringLiteral("qwen")); QVERIFY(engine == QStringLiteral("qwen") || engine == QStringLiteral("index"));
    const auto python = engine == QStringLiteral("qwen") ? QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe") : QStringLiteral("E:/AIModels/TTS/.venv-index/Scripts/python.exe");
    const auto model = engine == QStringLiteral("qwen") ? QStringLiteral("E:/AIModels/TTS/models/Qwen3-TTS/Qwen3-TTS-12Hz-0.6B-Base") : QStringLiteral("E:/AIModels/TTS/models/IndexTTS-2.5");
    const auto kind = engine == QStringLiteral("qwen") ? LocalTtsProvider::Kind::QwenBase : LocalTtsProvider::Kind::Index25;
    voice.checkLocal(engine, python, model); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 180000); QVERIFY2(!voice.qwenStatus().isEmpty(), qPrintable(voice.statusText()));
    voice.previewText(QStringLiteral("今天的天气很好。\nThe weather is nice today.\n今日はいい天気です。")); voice.applyPreview(); QCOMPARE(voice.rowCount(), 3);
    const auto path = QDir(output).filePath(QStringLiteral("cross-language.subcue-project")); QVERIFY(!QFileInfo::exists(path));
    QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto reference = sourcePath(QStringLiteral("out/validation/voice-p3-20261013/speaker-2-B.wav")); const auto quality = AudioQuality::inspect(reference); QVERIFY(quality); QString error;
    const auto version = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY2(ProjectStore(path).importVoiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces), version, reference, quality->sha256,
        {{QStringLiteral("qualityState"), QStringLiteral("approved")}, {QStringLiteral("referenceText"), QStringLiteral("感觉耗电耗得更快了")},
         {QStringLiteral("referenceLanguage"), QStringLiteral("Chinese")}, {QStringLiteral("quality"), quality->toJson()}}, &error), qPrintable(error));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    const QStringList languages{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")};
    const auto items = voice.snapshot().value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        const auto item = items[i].toObject();
        QVERIFY(voice.commitDraft({{QStringLiteral("id"), item.value(QStringLiteral("id")).toString()}, {QStringLiteral("text"), item.value(QStringLiteral("text")).toString()},
            {QStringLiteral("spoken"), QString()}, {QStringLiteral("manualSpoken"), false}, {QStringLiteral("notes"), QString()}, {QStringLiteral("gain"), 1.0},
            {QStringLiteral("pauseMs"), 300.0}, {QStringLiteral("seed"), QStringLiteral("12345")}, {QStringLiteral("language"), languages[i]}}));
    }
    QSignalSpy generated(&tasks, &TtsTaskController::finished), candidates(&tasks, &TtsTaskController::candidateCommitted);
    voice.generateLocal(engine, python, model, QStringLiteral("all")); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 1200000); QVERIFY2(generated.last().first().toBool(), qPrintable(generated.last().last().toString())); QCOMPARE(candidates.count(), 3);
    QTRY_VERIFY(!voice.busy()); voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    AudioPreviewController preview(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &preview); audio.setActive(true);
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000); QCOMPARE(audio.rows().size(), 3);
    const auto wav = QDir(output).filePath(QStringLiteral("explicit-language-sequence.wav")); audio.exportWav(QUrl::fromLocalFile(wav)); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000);
    const auto exported = AudioQuality::inspect(wav); QVERIFY(exported); QCOMPARE(exported->sampleRate, 48000); QCOMPARE(exported->channels, 1);
    const auto jobs = ProjectStore(path).jobs(0, 10); QCOMPARE(jobs.size(), 3); QJsonArray results;
    for (const auto &value : jobs) {
        const auto job = value.toObject(); QCOMPARE(job.value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
        const auto input = job.value(QStringLiteral("input")).toObject(); QCOMPARE(input.value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("Chinese"));
        const auto request = TtsRequest::fromSnapshot(input, LocalTtsProvider(kind, python, {}, model).capabilities(), &error); QVERIFY2(request, qPrintable(error));
        const auto take = ProjectStore(path).segmentTakes(input.value(QStringLiteral("segmentId")).toString()).first().toObject();
        const auto native = ProjectStore(path).resolveAudioPath(take.value(QStringLiteral("audioPath")).toString());
        QVERIFY(QFile::copy(native, QDir(output).filePath(input.value(QStringLiteral("language")).toString() + QStringLiteral(".wav"))));
        results.append(job);
    }
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); audio.stop();
    QCOMPARE(AudioQuality::inspect(reference)->sha256, quality->sha256); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QFile report(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(report.open(QIODevice::WriteOnly));
    report.write(QJsonDocument(QJsonObject{{QStringLiteral("engine"), engine}, {QStringLiteral("preflight"), QJsonObject::fromVariantMap(voice.qwenStatus())}, {QStringLiteral("jobs"), results},
        {QStringLiteral("document"), voice.snapshot()}, {QStringLiteral("exportQuality"), exported->toJson()}, {QStringLiteral("humanAudioAccepted"), false}}).toJson());
}
void EditorIntegrationTests::voiceEmotionWhenRequested()
{
    const auto output = qEnvironmentVariable("SUBCUE_L3_OUTPUT");
    if (output.isEmpty()) QSKIP("真实情绪识别须显式指定输出目录；普通回归不加载模型");
    QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
    QJsonArray jobs;
    for (const auto &language : {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}) {
        const auto path = sourcePath(QStringLiteral("out/validation/voice-l2-20261014/index-cross-language-clean/") + language + QStringLiteral(".wav"));
        QVERIFY(QFileInfo::exists(path));
        jobs.append(QJsonObject{{QStringLiteral("id"), language}, {QStringLiteral("path"), path},
            {QStringLiteral("sha256"), QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(path).toHex())}});
    }
    const auto input = QJsonDocument(QJsonObject{{QStringLiteral("modelDirectory"), QStringLiteral("E:/AIModels/AudioEmotion/emotion2vec_plus_base")}, {QStringLiteral("jobs"), jobs}}).toJson(QJsonDocument::Compact) + '\n';
    std::atomic<bool> cancel{false};
    const auto process = InferenceManager::instance().run(QStringLiteral("E:/AIModels/AudioEmotion/.venv/Scripts/python.exe"),
        {QStringLiteral("-B"), sourcePath(QStringLiteral("tools/audio_emotion_worker.py"))}, input, &cancel, 180000, {}, QStringLiteral("L3 中英日真实音频情绪验收"));
    QFile errors(QDir(output).filePath(QStringLiteral("worker-stderr.txt"))); QVERIFY(errors.open(QIODevice::WriteOnly)); errors.write(process.standardError); errors.close();
    QFile report(QDir(output).filePath(QStringLiteral("result.json"))); QVERIFY(report.open(QIODevice::WriteOnly)); report.write(process.standardOutput); report.close();
    QVERIFY2(process.started && process.exitCode == 0, process.standardError.constData());
    const auto result = QJsonDocument::fromJson(process.standardOutput).object();
    const auto predictions = result.value(QStringLiteral("results")).toArray(); QCOMPARE(predictions.size(), 3);
    for (int i = 0; i < predictions.size(); ++i) {
        QVERIFY(VoiceLibrary::validEmotionPrediction(predictions[i].toObject()));
        QCOMPARE(predictions[i].toObject().value(QStringLiteral("assetSha256")), jobs[i].toObject().value(QStringLiteral("sha256")));
        QVERIFY(!predictions[i].toObject().value(QStringLiteral("reviewed")).toBool());
    }
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    // 真实控制器路径：参考 → 预测 → 人工标签 → 新不可变版本。
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    voices.createProfile(QStringLiteral("真实情绪审核")); QTRY_VERIFY(!voices.busy());
    voices.addLocalVersion(QUrl::fromLocalFile(jobs[0].toObject().value(QStringLiteral("path")).toString()), QStringLiteral("今天的天气很好。")); QTRY_VERIFY(!voices.busy());
    voices.inspectVersion(voices.versions().first().toMap().value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voices.busy());
    voices.recognizeEmotion(QStringLiteral("E:/AIModels/AudioEmotion/.venv/Scripts/python.exe"), QStringLiteral("E:/AIModels/AudioEmotion/emotion2vec_plus_base"));
    QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 180000);
    QVERIFY2(voices.review().value(QStringLiteral("metadata")).toMap().contains(QStringLiteral("emotionPrediction")), qPrintable(voices.statusText()));
    QVERIFY(!voices.review().value(QStringLiteral("metadata")).toMap().contains(QStringLiteral("manualEmotion")));
    voices.approveReview(QStringLiteral("今天的天气很好。"), true, QStringLiteral("Chinese"), QStringLiteral("happy"), true); QTRY_VERIFY(!voices.busy());
    QCOMPARE(voices.versions().size(), 2);
    const auto reviewBeforeCancel = voices.review();
    voices.recognizeEmotion(QStringLiteral("E:/AIModels/AudioEmotion/.venv/Scripts/python.exe"), QStringLiteral("E:/AIModels/AudioEmotion/emotion2vec_plus_base"));
    QTest::qWait(100); voices.cancelAndWait(); QCoreApplication::processEvents();
    QVERIFY(!voices.busy()); QCOMPARE(voices.review(), reviewBeforeCancel);
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
}

void EditorIntegrationTests::voiceEmotionReviewAndCancellation()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); const auto path = directory.filePath(QStringLiteral("voices.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path); voices.createProfile(QStringLiteral("待审情绪")); QTRY_VERIFY(!voices.busy());
    voices.addLocalVersion(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), QStringLiteral("审核转写")); QTRY_VERIFY(!voices.busy());
    VoiceLibrary library(path); const auto original = library.version(voices.versions().first().toMap().value(QStringLiteral("id")).toString()); QVERIFY(original);
    auto metadata = original->value(QStringLiteral("metadata")).toObject();
    metadata.insert(QStringLiteral("manualEmotion"), QStringLiteral("happy"));
    // 仅为审核规则 fixture，不是真实模型的准确性证据。
    metadata.insert(QStringLiteral("emotionPrediction"), QJsonObject{{QStringLiteral("assetSha256"), original->value(QStringLiteral("referenceSha256"))},
        {QStringLiteral("modelVersion"), QString(64, QLatin1Char('a'))}, {QStringLiteral("weightSha256"), QString(64, QLatin1Char('b'))},
        {QStringLiteral("model"), QStringLiteral("emotion2vec_plus_base")}, {QStringLiteral("policy"), QStringLiteral("review-v1-score0.6-margin0.15")},
        {QStringLiteral("label"), QStringLiteral("neutral")}, {QStringLiteral("score"), 0.4}, {QStringLiteral("margin"), 0.1},
        {QStringLiteral("scores"), QJsonArray{0.3,0,0,0.3,0.4,0,0,0,0}}, {QStringLiteral("requiresReview"), true}, {QStringLiteral("reviewed"), false}});
    const auto tagged = library.addVersion(voices.selectedProfile(), metadata, original->value(QStringLiteral("referencePath")).toString()); QVERIFY(tagged);
    voices.inspectVersion(*tagged); QTRY_VERIFY(!voices.busy());
    const auto before = voices.review();
    voices.approveReview(QStringLiteral("审核转写"), true, QStringLiteral("Chinese"), QStringLiteral("happy"));
    QVERIFY(voices.statusText().contains(QStringLiteral("情绪审核"))); QVERIFY(!voices.busy()); QCOMPARE(library.versions(voices.selectedProfile()).size(), 2);
    {
        auto lease = InferenceManager::instance().acquire();
        voices.recognizeEmotion(QCoreApplication::applicationFilePath(), directory.path());
        QVERIFY(voices.busy()); voices.cancelAndWait(); QVERIFY(!voices.busy());
    }
    QCoreApplication::processEvents(); QCOMPARE(voices.review(), before); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    voices.approveReview(QStringLiteral("审核转写"), true, QStringLiteral("Chinese"), QStringLiteral("happy"), true); QTRY_VERIFY(!voices.busy());
    QCOMPARE(library.versions(voices.selectedProfile()).size(), 3);
    QCOMPARE(library.version(*tagged)->value(QStringLiteral("metadata")).toObject(), metadata);
    voices.inspectVersion(*tagged); QTRY_VERIFY(!voices.busy()); voices.discardEmotionPrediction();
    QVERIFY(!voices.review().value(QStringLiteral("metadata")).toMap().contains(QStringLiteral("emotionPrediction")));
    QCOMPARE(voices.review().value(QStringLiteral("metadata")).toMap().value(QStringLiteral("manualEmotion")).toString(), QStringLiteral("happy"));
    voices.approveReview(QStringLiteral("审核转写"), true, QStringLiteral("Chinese"), QStringLiteral("happy")); QTRY_VERIFY(!voices.busy());
    QCOMPARE(library.versions(voices.selectedProfile()).size(), 4);
    QCOMPARE(library.version(*tagged)->value(QStringLiteral("metadata")).toObject(), metadata);
}

void EditorIntegrationTests::voiceEmotionBatchCancellationAndDestruction()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough);
    auto voices = std::make_unique<VoiceLibraryController>(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    voices->createProfile(QStringLiteral("批量情绪取消")); QTRY_VERIFY(!voices->busy());
    voices->prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))}); QTRY_VERIFY(!voices->busy());
    QVERIFY(!voices->samples().isEmpty());
    for (int i = 0; i < voices->samples().size(); ++i) voices->editSample(i, QStringLiteral("手工转写"), false);
    voices->recognizeEmotion(QCoreApplication::applicationFilePath(), directory.path(), true);
    QVERIFY(!voices->busy()); QVERIFY(voices->statusText().contains(QStringLiteral("勾选")));
    voices->editSample(0, QStringLiteral("手工转写"), true);
    const auto before = voices->samples(); const auto samplePath = before.first().toMap().value(QStringLiteral("referencePath")).toString();
    {
        auto lease = InferenceManager::instance().acquire();
        voices->recognizeEmotion(QCoreApplication::applicationFilePath(), directory.path(), true);
        QVERIFY(voices->busy()); voices->cancelAndWait();
    }
    QCoreApplication::processEvents(); QCOMPARE(voices->samples(), before); QVERIFY(!voices->busy());
    QVERIFY(InferenceManager::instance().tasks().isEmpty()); QVERIFY(QFileInfo::exists(samplePath));
    QSignalSpy finished(voices.get(), &VoiceLibraryController::operationFinished);
    {
        auto lease = InferenceManager::instance().acquire();
        voices->recognizeEmotion(QCoreApplication::applicationFilePath(), directory.path(), true);
        QVERIFY(voices->busy()); voices.reset();
    }
    QCoreApplication::processEvents(); QCOMPARE(finished.count(), 0);
    QVERIFY(InferenceManager::instance().tasks().isEmpty()); QVERIFY(!QFileInfo::exists(samplePath));
}

void EditorIntegrationTests::voiceEmotionBatchWhenRequested()
{
    const auto output = qEnvironmentVariable("SUBCUE_L3_BATCH_OUTPUT");
    if (output.isEmpty()) QSKIP("真实批量情绪识别须显式指定新输出目录；普通回归不加载模型");
    QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough);
    const auto path = QDir(output).filePath(QStringLiteral("voices.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path);
    voices.createProfile(QStringLiteral("批量情绪实际链路")); QTRY_VERIFY(!voices.busy());
    QList<QUrl> files;
    for (const auto &language : {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")})
        files.append(QUrl::fromLocalFile(sourcePath(QStringLiteral("out/validation/voice-l2-20261014/index-cross-language-clean/") + language + QStringLiteral(".wav"))));
    voices.prepareSamples(files); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 30000); QVERIFY(voices.samples().size() >= 3);
    for (int i = 0; i < voices.samples().size(); ++i) voices.editSample(i, QStringLiteral("保留手工转写"), i != 0);
    const auto before = voices.samples();
    voices.recognizeEmotion(QStringLiteral("E:/AIModels/AudioEmotion/.venv/Scripts/python.exe"), QStringLiteral("E:/AIModels/AudioEmotion/emotion2vec_plus_base"), true);
    QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 180000);
    QVERIFY2(voices.statusText().contains(QStringLiteral("识别完成")), qPrintable(voices.statusText()));
    QCOMPARE(voices.samples().first(), before.first());
    for (int i = 1; i < voices.samples().size(); ++i) {
        const auto sample = voices.samples()[i].toMap(); const auto prediction = QJsonObject::fromVariantMap(sample.value(QStringLiteral("emotionPrediction")).toMap());
        QVERIFY(VoiceLibrary::validEmotionPrediction(prediction)); QVERIFY(!prediction.value(QStringLiteral("reviewed")).toBool());
        QCOMPARE(prediction.value(QStringLiteral("assetSha256")).toString(), sample.value(QStringLiteral("referenceSha256")).toString());
        QCOMPARE(sample.value(QStringLiteral("transcript")), before[i].toMap().value(QStringLiteral("transcript")));
        QVERIFY(!sample.contains(QStringLiteral("manualEmotion")));
    }
    voices.saveSamples(); QTRY_VERIFY_WITH_TIMEOUT(!voices.busy(), 30000);
    VoiceLibrary library(path);
    for (int i = 1; i < voices.samples().size(); ++i) {
        const auto sample = voices.samples()[i].toMap(); const auto version = library.version(sample.value(QStringLiteral("savedVersionId")).toString()); QVERIFY(version);
        const auto metadata = version->value(QStringLiteral("metadata")).toObject();
        QCOMPARE(metadata.value(QStringLiteral("emotionPrediction")).toObject(), QJsonObject::fromVariantMap(sample.value(QStringLiteral("emotionPrediction")).toMap()));
        QCOMPARE(metadata.value(QStringLiteral("qualityState")).toString(), QStringLiteral("pending"));
        voices.inspectVersion(version->value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voices.busy());
        QCOMPARE(voices.review().value(QStringLiteral("metadata")).toMap().value(QStringLiteral("emotionPrediction")), sample.value(QStringLiteral("emotionPrediction")));
    }
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QFile audit(QDir(output).filePath(QStringLiteral("batch-review.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("samples"), QJsonArray::fromVariantList(voices.samples())}, {QStringLiteral("humanAccuracyAccepted"), false}}).toJson());
}

void EditorIntegrationTests::voiceFlashDownloadedWavCompletesProject()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    const auto fixturePath = qEnvironmentVariable("SUBCUE_FLASH_PROJECT_FIXTURE", mediaPath(QStringLiteral("audio.wav")));
    QFile fixture(fixturePath); QVERIFY(fixture.open(QIODevice::ReadOnly)); http.downloadBody = fixture.readAll(); fixture.close();
    const auto originalHash = RoughCutProjectSerializer::mediaSha256(fixturePath);
    http.responses.append(HttpResponse{200, {}, QJsonDocument(QJsonObject{{QStringLiteral("request_id"), QStringLiteral("offline-replay-only")},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input_tokens"), 15}, {QStringLiteral("output_tokens"), 37}}},
        {QStringLiteral("output"), QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("stop")}, {QStringLiteral("audio"), QJsonObject{
            {QStringLiteral("url"), QStringLiteral("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/offline.wav")}, {QStringLiteral("expires_at"), 4102444800LL}}}}}}).toJson()});
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    const auto output = qEnvironmentVariable("SUBCUE_FLASH_PROJECT_OUTPUT", directory.path()); QVERIFY(QDir().mkpath(output));
    const auto path = QDir(output).filePath(QStringLiteral("cloud-replay.subcue-project")); QVERIFY(!QFileInfo::exists(path));
    voice.previewText(QStringLiteral("你好，这是 SubCue 云端配音测试。")); voice.applyPreview();
    QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto account = configureUnifiedSynthesis(context.get()); QVERIFY(!account.isEmpty());
    const auto version = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(ProjectStore(path).importVoiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces), version, {}, {}, {
        {QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("model"), QStringLiteral("qwen-audio-3.1-tts-flash")},
        {QStringLiteral("region"), QStringLiteral("beijing")}, {QStringLiteral("businessSpace"), QStringLiteral("mock-space")},
        {QStringLiteral("cloudAccountId"), account}, {QStringLiteral("providerVoiceId"), QStringLiteral("offline-test-voice")}}));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    voice.prepareFlash(QStringLiteral("all"), 0.10); QTRY_VERIFY(!voice.busy()); QVERIFY(!voice.flashPreview().contains(QStringLiteral("estimatedYuan")));
    QSignalSpy finished(&tasks, &TtsTaskController::finished); voice.confirmFlash(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().last().toString())); QCOMPARE(http.sendCount, 1);
    QTRY_VERIFY(!voice.busy()); QVERIFY(voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty());
    const auto jobs = ProjectStore(path).jobs(); QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs[0].toObject().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
    const auto usage = jobs[0].toObject().value(QStringLiteral("usage")).toObject();
    QCOMPARE(usage.value(QStringLiteral("inputTokens")).toInt(), 15); QCOMPARE(usage.value(QStringLiteral("outputTokens")).toInt(), 37);
    QVERIFY(std::abs(usage.value(QStringLiteral("yuan")).toDouble() - 0.0004665) < 1e-12);
    voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy()); QVERIFY(!voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty());
    voice.prepareFlash(QStringLiteral("all"), 0.10); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), 1);
    voice.confirmFlash(); QCOMPARE(http.sendCount, 1); QCOMPARE(ProjectStore(path).history().size(), 1);
    const auto cloudAudio = ProjectStore(path).resolveAudioPath(ProjectStore(path).history().first().toObject().value(QStringLiteral("audioPath")).toString());
    for (const auto &scope : {QStringLiteral("all"), QStringLiteral("invalid")}) {
        QVERIFY(QFile::rename(cloudAudio, cloudAudio + QStringLiteral(".offline")));
        voice.prepareFlash(scope, 0.10); QTRY_VERIFY(!voice.busy());
        const auto previewRows = voice.flashPreview().value(QStringLiteral("rows")).toList().size();
        QVERIFY(QFile::rename(cloudAudio + QStringLiteral(".offline"), cloudAudio));
        QCOMPARE(previewRows, 1); QCOMPARE(http.sendCount, 1); QCOMPARE(ProjectStore(path).jobs()[0].toObject().value(QStringLiteral("usage")).toObject(), usage);
        voice.discardFlash();
    }
    AudioPreviewController preview(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &preview);
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000); QCOMPARE(audio.rows().size(), 1); audio.stop();
    const auto wav = QDir(output).filePath(QStringLiteral("cloud-replay-export.wav")); audio.exportWav(QUrl::fromLocalFile(wav)); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    const auto quality = AudioQuality::inspect(wav); QVERIFY2(quality, qPrintable(audio.statusText())); QCOMPARE(quality->sampleRate, 48000);
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); QVERIFY(saved.last().first().toBool());
    QCOMPARE(ProjectStore(path).load()->modules.value(QStringLiteral("voiceStudio")).toObject(), voice.snapshot());
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(fixturePath), originalHash); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QFile audit(QDir(output).filePath(QStringLiteral("replay-audit.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("networkUsed"), false}, {QStringLiteral("syntheticUsage"), true},
        {QStringLiteral("mockSynthesisRequests"), http.sendCount}, {QStringLiteral("sourceSha256"), QString::fromLatin1(originalHash.toHex())}, {QStringLiteral("quality"), quality->toJson()}}).toJson());
}

void EditorIntegrationTests::voiceFlashRealSingleWhenAuthorized()
{
    const auto output = qEnvironmentVariable("SUBCUE_FLASH_AUTHORIZED_OUTPUT");
    if (output.isEmpty()) QSKIP("真实付费单句须明确授权及指定新输出目录。");
    QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
    // 先留下尝试标记：即使网络结果不明，也不能重跑同一授权自动付费。
    QFile marker(QDir(output).filePath(QStringLiteral("attempted"))); QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)); marker.close();
    DpapiCredentialStore credentials;
    CloudConfigStore config(QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("SubCue/connections.sqlite")), &credentials);
    QString key, error; const auto cloud = config.resolveSynthesis(&key, &error); QVERIFY2(!cloud.isEmpty(), qPrintable(error));
    const auto voiceId = qEnvironmentVariable("SUBCUE_FLASH_AUTHORIZED_VOICE"); QVERIFY(!voiceId.isEmpty());
    QJsonObject ready;
    if (!qEnvironmentVariableIsEmpty("SUBCUE_FLASH_AUTHORIZED_READY")) {
        QFile file(qEnvironmentVariable("SUBCUE_FLASH_AUTHORIZED_READY")); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.size() < 64 * 1024);
        const auto bytes = file.readAll();
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), qEnvironmentVariable("SUBCUE_FLASH_AUTHORIZED_SHA256"));
        ready = QJsonDocument::fromJson(bytes).object(); QVERIFY(!ready.isEmpty());
        QCOMPARE(ready.value(QStringLiteral("cloud")).toObject(), cloud);
        QCOMPARE(ready.value(QStringLiteral("voiceId")).toString(), voiceId);
        QCOMPARE(ready.value(QStringLiteral("maximumSynthesisRequests")).toInt(), 1);
        QCOMPARE(ready.value(QStringLiteral("budgetYuan")).toDouble(), 0.10);
        QVERIFY(!ready.value(QStringLiteral("text")).toString().trimmed().isEmpty());
    }
    TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectId = QUuid::createUuid().toString(); job.input.segmentId = QUuid::createUuid().toString(); job.input.voiceVersionId = QUuid::createUuid().toString();
    job.input.projectGeneration = 1; job.input.provider = QStringLiteral("qwen-cloud"); job.input.model = cloud.value(QStringLiteral("model")).toString();
    job.input.modelVersion = QStringLiteral("http-wav24k-20261014"); job.input.region = cloud.value(QStringLiteral("region")).toString();
    job.input.language = QStringLiteral("Chinese"); job.input.providerVoiceId = voiceId;
    job.input.spokenText = ready.isEmpty() ? QStringLiteral("你好，这是 SubCue 云端配音测试。") : ready.value(QStringLiteral("text")).toString();
    if (ready.contains(QStringLiteral("parameters"))) { QVERIFY(ready.value(QStringLiteral("parameters")).isObject()); job.input.effectiveParameters = ready.value(QStringLiteral("parameters")).toObject(); }
    job.billing = {{QStringLiteral("price"), TtsPricing::flashPrice()}};
    QJsonObject audit{{QStringLiteral("text"), job.input.spokenText}, {QStringLiteral("voiceId"), voiceId}, {QStringLiteral("budgetYuan"), 0.10}, {QStringLiteral("parameters"), job.input.effectiveParameters}};
    const auto result = CloudTtsProvider(cloud.value(QStringLiteral("businessSpace")).toString(), key, 0.10).generate({job}, output, nullptr, {}, 120000, {},
        [&](const QString &, const QJsonObject &usage, const QJsonObject &metadata) {
            audit.insert(QStringLiteral("usage"), usage); audit.insert(QStringLiteral("metadata"), metadata);
            QFile record(QDir(output).filePath(QStringLiteral("audit.json"))); if (record.open(QIODevice::WriteOnly)) record.write(QJsonDocument(audit).toJson());
        }, [&](const QString &path, QString *qualityError) {
            // 探针保留原始下载字节，避免 Provider 失败清理后丢失容器诊断证据。
            const auto raw = QDir(output).filePath(QStringLiteral("download-original.wav"));
            if (!QFile::copy(path, raw)) { if (qualityError) *qualityError = QStringLiteral("无法保存原始下载诊断文件。"); return std::optional<AudioQuality>{}; }
            return AudioQuality::inspect(path, qualityError);
        });
    key.clear(); audit.insert(QStringLiteral("success"), result.success()); audit.insert(QStringLiteral("error"), result.error); audit.insert(QStringLiteral("failures"), result.failures);
    if (!result.results.isEmpty()) { const auto quality = AudioQuality::inspect(result.results.first().audioPath); QVERIFY(quality); audit.insert(QStringLiteral("quality"), quality->toJson()); audit.insert(QStringLiteral("audioPath"), result.results.first().audioPath); }
    QFile record(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(record.open(QIODevice::WriteOnly)); record.write(QJsonDocument(audit).toJson()); record.close();
    QVERIFY2(result.success(), qPrintable(result.error)); QCOMPARE(result.results.size(), 1);
}

void EditorIntegrationTests::voiceCloudProjectAcceptanceWhenRequested()
{
    const auto output = qEnvironmentVariable("SUBCUE_CLOUD_ACCEPTANCE_DIRECTORY");
    if (output.isEmpty()) QSKIP("真实云端工程验收须显式准备目录及逐批授权，普通回归不联网");
    QVERIFY(QDir().mkpath(output)); DpapiCredentialStore productionCredentials;
    const bool offline = qEnvironmentVariable("SUBCUE_CLOUD_ACCEPTANCE_OFFLINE") == QStringLiteral("1");
    RecordingHttpClient offlineHttp;
    QTemporaryDir directory; auto context = makeContext(directory);
    if (offline) {
        QFile fixture(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(fixture.open(QIODevice::ReadOnly)); offlineHttp.downloadBody = fixture.readAll(); context->aiHttp = &offlineHttp;
        for (int i = 0; i < 13; ++i) offlineHttp.responses.append({200, {}, QJsonDocument(QJsonObject{
            {QStringLiteral("request_id"), QStringLiteral("offline-cloud-%1").arg(i)},
            {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input_tokens"), 10}, {QStringLiteral("output_tokens"), 30}}},
            {QStringLiteral("output"), QJsonObject{{QStringLiteral("finish_reason"), QStringLiteral("stop")}, {QStringLiteral("audio"), QJsonObject{{QStringLiteral("url"), QStringLiteral("https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/offline.wav")}, {QStringLiteral("expires_at"), 4102444800LL}}}}}}).toJson()});
    }
    context->cloudConfig = std::make_unique<CloudConfigStore>(QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("SubCue/connections.sqlite")), &productionCredentials);
    QString key, error; const auto cloud = context->cloudConfig->resolveSynthesis(&key, &error); key.clear(); QVERIFY2(!cloud.isEmpty(), qPrintable(error));
    const auto configuration = context->cloudConfig->load(&error); QVERIFY2(!configuration.isEmpty(), qPrintable(error));
    QJsonObject plan;
    if (!qEnvironmentVariableIsEmpty("SUBCUE_CLOUD_ACCEPTANCE_PLAN")) {
        QFile file(qEnvironmentVariable("SUBCUE_CLOUD_ACCEPTANCE_PLAN")); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.size() < 128 * 1024);
        plan = QJsonDocument::fromJson(file.readAll()).object(); QVERIFY(!plan.isEmpty());
        QCOMPARE(plan.value(QStringLiteral("cloud")).toObject(), cloud);
        QCOMPARE(plan.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("speedFactor")).toDouble(), 0.85);
        QCOMPARE(plan.value(QStringLiteral("revision")).toObject().value(QStringLiteral("index")).toInt(-1), 0);
        QVERIFY(!plan.value(QStringLiteral("voiceId")).toString().isEmpty());
    }
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks);
    const auto path = QDir(output).filePath(QStringLiteral("cloud-acceptance.subcue-project"));
    QSignalSpy saved(&session, &ProjectSession::saved), opened(&session, &ProjectSession::opened), finished(&tasks, &TtsTaskController::finished);
    if (QFileInfo::exists(path)) { QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 1); QVERIFY(opened.last().first().toBool()); QTRY_VERIFY(!voice.busy()); }
    else {
        QVERIFY(qEnvironmentVariableIsEmpty("SUBCUE_CLOUD_ACCEPTANCE_CONFIRM_SHA256")); session.newProject();
        voice.previewText(plan.isEmpty() ? QStringLiteral("清晨，我推开窗户，看见阳光落在对面的屋顶上。\n请检查手机电量，再出发。") : plan.value(QStringLiteral("manuscript")).toString()); voice.applyPreview();
        if (!plan.isEmpty()) {
            QCOMPARE(voice.rowCount(), 12);
            for (const auto &value : voice.snapshot().value(QStringLiteral("segments")).toArray()) {
                voice.select(value.toObject().value(QStringLiteral("id")).toString()); auto draft = voice.selected();
                draft.insert(QStringLiteral("gptParameters"), true); draft.insert(QStringLiteral("speedFactor"), plan.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("speedFactor")).toDouble());
                QVERIFY2(voice.commitDraft(draft), qPrintable(voice.statusText()));
            }
            voice.select(voice.snapshot().value(QStringLiteral("segments")).toArray().first().toObject().value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voice.busy());
        }
        QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
        const auto version = QUuid::createUuid().toString(QUuid::WithoutBraces); auto binding = cloud;
        binding.insert(QStringLiteral("provider"), QStringLiteral("qwen-cloud")); binding.insert(QStringLiteral("providerVoiceId"), plan.isEmpty() ? QStringLiteral("baiqinglan_v3.1") : plan.value(QStringLiteral("voiceId")).toString());
        QVERIFY2(ProjectStore(path).importVoiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces), version, {}, {}, binding, &error), qPrintable(error));
        voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
        QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); QVERIFY(saved.last().first().toBool());
    }
    const int sentenceCount = plan.isEmpty() ? 2 : 12; QCOMPARE(voice.rowCount(), sentenceCount);
    if (!plan.isEmpty()) for (const auto &value : voice.snapshot().value(QStringLiteral("segments")).toArray()) QCOMPARE(value.toObject().value(QStringLiteral("parameters")).toObject(), plan.value(QStringLiteral("parameters")).toObject());
    QVERIFY(!QFileInfo::exists(QDir(output).filePath(QStringLiteral("attempted"))));
    voice.prepareFlash(QStringLiteral("all"), 0.10); QTRY_VERIFY(!voice.busy());
    const auto preview = QJsonObject::fromVariantMap(voice.flashPreview()); QCOMPARE(preview.value(QStringLiteral("rows")).toArray().size(), sentenceCount);
    QVERIFY(ProjectStore(path).jobs().isEmpty()); QVERIFY(!tasks.busy());
    QJsonObject manifest{{QStringLiteral("cloud"), cloud}, {QStringLiteral("rows"), preview.value(QStringLiteral("rows"))}, {QStringLiteral("budgetYuan"), 0.10},
        {QStringLiteral("baseHash"), preview.value(QStringLiteral("baseHash"))}, {QStringLiteral("maximumSynthesisRequests"), sentenceCount + (plan.isEmpty() ? 0 : 1)},
        {QStringLiteral("configurationHash"), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(configuration).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())}};
    if (!plan.isEmpty()) manifest.insert(QStringLiteral("plan"), plan);
    if (offline) manifest.insert(QStringLiteral("offlineFixture"), true);
    for (const auto &value : configuration.value(QStringLiteral("accounts")).toArray()) if (value.toObject().value(QStringLiteral("id")) == cloud.value(QStringLiteral("cloudAccountId"))) manifest.insert(QStringLiteral("accountName"), value.toObject().value(QStringLiteral("name")));
    QFile ready(QDir(output).filePath(QStringLiteral("ready.json")));
    if (ready.exists()) { QVERIFY(ready.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(ready.readAll()).object(), manifest); ready.close(); }
    else { QVERIFY(ready.open(QIODevice::WriteOnly | QIODevice::NewOnly)); ready.write(QJsonDocument(manifest).toJson()); ready.close(); }
    const auto approvedHash = RoughCutProjectSerializer::mediaSha256(ready.fileName()).toHex();
    if (qEnvironmentVariableIsEmpty("SUBCUE_CLOUD_ACCEPTANCE_CONFIRM_SHA256")) { voice.discardFlash(); qInfo() << "prepared_only_no_network" << approvedHash; return; }
    QCOMPARE(qEnvironmentVariable("SUBCUE_CLOUD_ACCEPTANCE_CONFIRM_SHA256").toLatin1(), approvedHash);
    // 请求前永久留下本批次尝试标记，失败或结果不明后不得复跑同一授权。
    QFile attempted(QDir(output).filePath(QStringLiteral("attempted"))); QVERIFY(attempted.open(QIODevice::WriteOnly | QIODevice::NewOnly)); QCOMPARE(attempted.write(approvedHash), approvedHash.size()); attempted.close();
    QSignalSpy committed(&tasks, &TtsTaskController::candidateCommitted);
    voice.confirmFlash(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 300000); QTRY_VERIFY(!voice.busy());
    QFile audit(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(audit.open(QIODevice::WriteOnly));
    audit.write(QJsonDocument(QJsonObject{{QStringLiteral("approvedManifestSha256"), QString::fromLatin1(approvedHash)}, {QStringLiteral("success"), finished.last().first().toBool()},
        {QStringLiteral("jobs"), ProjectStore(path).jobs()}, {QStringLiteral("offlineFixture"), offline}, {QStringLiteral("humanAudioAccepted"), false}}).toJson()); audit.close();
    QVERIFY2(finished.last().first().toBool(), qPrintable(tasks.statusText())); QCOMPARE(ProjectStore(path).history().size(), sentenceCount); QCOMPARE(committed.count(), sentenceCount);
    double spent = 0;
    for (const auto &value : ProjectStore(path).jobs()) {
        const auto job = value.toObject(); QCOMPARE(job.value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
        const auto usage = job.value(QStringLiteral("usage")).toObject(); const auto amount = TtsPricing::amount(usage, usage.value(QStringLiteral("price")).toObject()); QVERIFY(amount); spent += *amount;
        QVERIFY(preview.value(QStringLiteral("rows")).toArray().contains(QJsonObject{{QStringLiteral("text"), job.value(QStringLiteral("input")).toObject().value(QStringLiteral("spokenText"))}, {QStringLiteral("voice"), job.value(QStringLiteral("input")).toObject().value(QStringLiteral("providerVoiceId"))}}));
        if (!plan.isEmpty()) { QCOMPARE(job.value(QStringLiteral("input")).toObject().value(QStringLiteral("parameters")).toObject(), plan.value(QStringLiteral("parameters")).toObject()); QCOMPARE(job.value(QStringLiteral("input")).toObject().value(QStringLiteral("providerVoiceId")).toString(), plan.value(QStringLiteral("voiceId")).toString()); }
    }
    voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    AudioPreviewController player(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &player);
    audio.prepare(); QTRY_VERIFY(!audio.busy()); QCOMPARE(audio.rows().size(), sentenceCount);
    if (!plan.isEmpty()) {
        const auto segments = voice.snapshot().value(QStringLiteral("segments")).toArray(); voice.select(segments[1].toObject().value(QStringLiteral("id")).toString());
        voice.setArrangement(1, 150, QStringLiteral("10"), voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("sampleCount")).toString());
    }
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("cloud-acceptance.wav")))); QTRY_VERIFY(!audio.busy());
    const auto quality = AudioQuality::inspect(QDir(output).filePath(QStringLiteral("cloud-acceptance.wav"))); QVERIFY2(quality, qPrintable(audio.statusText()));
    if (!plan.isEmpty()) {
        const auto initial = voice.snapshot().value(QStringLiteral("segments")).toArray(); const auto revision = plan.value(QStringLiteral("revision")).toObject();
        QCOMPARE(initial[0].toObject().value(QStringLiteral("text")), revision.value(QStringLiteral("originalText"))); QVERIFY(!revision.value(QStringLiteral("text")).toString().isEmpty());
        voice.select(initial[0].toObject().value(QStringLiteral("id")).toString());
        voice.editSegment(voice.selectedId(), revision.value(QStringLiteral("text")).toString(), {}, false, {});
        voice.prepareFlash(QStringLiteral("all"), 0.10); QTRY_VERIFY(!voice.busy());
        QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 1); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), sentenceCount - 1); voice.discardFlash();
        QVERIFY(spent < 0.10);
        voice.prepareFlash(QStringLiteral("selected"), 0.10 - spent); QTRY_VERIFY(!voice.busy());
        QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 1); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), 0);
        QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList()[0].toMap().value(QStringLiteral("text")).toString(), revision.value(QStringLiteral("text")).toString());
        QCOMPARE(ProjectStore(path).jobs().size(), sentenceCount);
        voice.confirmFlash(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 300000); QTRY_VERIFY(!voice.busy());
        QVERIFY(audit.open(QIODevice::WriteOnly)); audit.write(QJsonDocument(QJsonObject{{QStringLiteral("jobs"), ProjectStore(path).jobs()}, {QStringLiteral("stage"), QStringLiteral("revision")}, {QStringLiteral("offlineFixture"), offline}, {QStringLiteral("humanAudioAccepted"), false}}).toJson()); audit.close();
        QVERIFY2(finished.last().first().toBool(), qPrintable(tasks.statusText())); QCOMPARE(committed.count(), sentenceCount + 1); QCOMPARE(ProjectStore(path).history().size(), sentenceCount + 1);
        QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap(), initial[0].toObject().value(QStringLiteral("selectedTake")).toObject().toVariantMap());
        voice.applyCandidate(committed.last()[1].toString(), QStringLiteral("reset"));
        const auto changed = voice.snapshot().value(QStringLiteral("segments")).toArray();
        for (int i = 1; i < sentenceCount; ++i) QCOMPARE(changed[i], initial[i]);
        audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("cloud-revised.wav")))); QTRY_VERIFY(!audio.busy()); QVERIFY2(AudioQuality::inspect(QDir(output).filePath(QStringLiteral("cloud-revised.wav"))), qPrintable(audio.statusText()));
    }
    const auto document = voice.snapshot(); const auto jobs = ProjectStore(path).jobs(); const auto history = ProjectStore(path).history(); const int saveCount = saved.count();
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), saveCount + 1); QVERIFY(saved.last().first().toBool()); session.newProject();
    const int openCount = opened.count(); QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), openCount + 1); QVERIFY(opened.last().first().toBool()); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.snapshot(), document); QCOMPARE(ProjectStore(path).jobs(), jobs); QCOMPARE(ProjectStore(path).history(), history);
    const auto finalName = plan.isEmpty() ? QStringLiteral("cloud-acceptance.wav") : QStringLiteral("cloud-revised.wav");
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("cloud-reopened.wav")))); QTRY_VERIFY(!audio.busy());
    const auto reopened = AudioQuality::inspect(QDir(output).filePath(QStringLiteral("cloud-reopened.wav"))); QVERIFY(reopened);
    const auto exported = AudioQuality::inspect(QDir(output).filePath(finalName)); QVERIFY(exported);
    QCOMPARE(reopened->sha256, exported->sha256); QCOMPARE(reopened->sampleCount, exported->sampleCount); QCOMPARE(reopened->sampleRate, 48000);
    voice.prepareFlash(QStringLiteral("all"), 0.10); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), sentenceCount); QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 0); voice.discardFlash(); QCOMPARE(ProjectStore(path).jobs(), jobs);
    QJsonObject totalUsage{{QStringLiteral("inputTokens"), 0}, {QStringLiteral("outputTokens"), 0}, {QStringLiteral("yuan"), 0.0}};
    QSet<QString> requestIds;
    QCOMPARE(jobs.size(), manifest.value(QStringLiteral("maximumSynthesisRequests")).toInt());
    for (const auto &value : jobs) {
        const auto job = value.toObject(); QCOMPARE(job.value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
        const auto usage = job.value(QStringLiteral("usage")).toObject(); const auto amount = TtsPricing::amount(usage, usage.value(QStringLiteral("price")).toObject()); QVERIFY(amount);
        QVERIFY(!usage.value(QStringLiteral("requestId")).toString().isEmpty()); QVERIFY(!requestIds.contains(usage.value(QStringLiteral("requestId")).toString())); requestIds.insert(usage.value(QStringLiteral("requestId")).toString());
        for (const auto &name : {QStringLiteral("inputTokens"), QStringLiteral("outputTokens")}) totalUsage.insert(name, totalUsage.value(name).toInteger() + usage.value(name).toInteger());
        totalUsage.insert(QStringLiteral("yuan"), totalUsage.value(QStringLiteral("yuan")).toDouble() + *amount);
    }
    if (offline) { QCOMPARE(offlineHttp.sendCount, jobs.size()); QCOMPARE(offlineHttp.downloadCount, jobs.size()); }
    QVERIFY(audit.open(QIODevice::WriteOnly)); audit.write(QJsonDocument(QJsonObject{{QStringLiteral("approvedManifestSha256"), QString::fromLatin1(approvedHash)}, {QStringLiteral("jobs"), jobs}, {QStringLiteral("historyCount"), history.size()}, {QStringLiteral("reopenedQuality"), reopened->toJson()}, {QStringLiteral("totalUsage"), totalUsage}, {QStringLiteral("offlineFixture"), offline}, {QStringLiteral("success"), true}, {QStringLiteral("humanAudioAccepted"), false}}).toJson());
}

void EditorIntegrationTests::voiceAppliedAudioMustBeIntactBeforeReuse()
{
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("需要独立测试 Python");
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    voice.previewText(QStringLiteral("素材完整性测试。")); voice.applyPreview();
    context->submitProjectStorage([] {}).waitForFinished();
    const auto version = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(ProjectStore(session.storagePath()).importVoiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces), version, {}, {}, {{QStringLiteral("provider"), QStringLiteral("mock")}}));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    QSignalSpy generated(&tasks, &TtsTaskController::finished);
    voice.generateMock(python, QStringLiteral("all")); QTRY_COMPARE(generated.count(), 1); QVERIFY(generated.last().first().toBool()); QTRY_VERIFY(!voice.busy());
    voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    const auto frozen = voice.snapshot();
    const auto path = ProjectStore(session.storagePath()).resolveAudioPath(ProjectStore(session.storagePath()).history().first().toObject().value(QStringLiteral("audioPath")).toString());
    QFile asset(path); QVERIFY(asset.open(QIODevice::ReadOnly)); const auto original = asset.readAll(); asset.close();
    for (const bool missing : {false, true}) {
        if (missing) QVERIFY(QFile::rename(path, path + QStringLiteral(".offline")));
        else { QVERIFY(asset.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(asset.write(QByteArray(original.size(), '\0')), original.size()); asset.close(); }
        voice.generateMock(python, QStringLiteral("invalid"), true); QTRY_VERIFY(!voice.busy());
        const int needed = voice.generationPreview().value(QStringLiteral("needed"), -1).toInt();
        if (missing) QVERIFY(QFile::rename(path + QStringLiteral(".offline"), path));
        else { QVERIFY(asset.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(asset.write(original), original.size()); asset.close(); }
        QCOMPARE(needed, 1); QVERIFY(!tasks.busy()); QCOMPARE(generated.count(), 1); QCOMPARE(voice.snapshot(), frozen);
        voice.discardGeneration();
    }
    voice.generateMock(python, QStringLiteral("invalid"), true); QTRY_VERIFY(!voice.busy());
    QVERIFY(voice.generationPreview().isEmpty()); QVERIFY(!tasks.busy()); QCOMPARE(generated.count(), 1);
    QCOMPARE(ProjectStore(session.storagePath()).history().size(), 1); QCOMPARE(voice.snapshot(), frozen);
}

void EditorIntegrationTests::voiceRealProjectRoundTripWhenRequested()
{
    const auto source = qEnvironmentVariable("SUBCUE_CORE_REAL_PROJECT");
    if (source.isEmpty()) QSKIP("真实音频工程闭环须显式提供已有验收工程，普通回归不加载外部工程");
    const auto output = qEnvironmentVariable("SUBCUE_CORE_PROJECT_OUTPUT"); QVERIFY(!output.isEmpty()); QVERIFY(QDir().mkpath(output));
    const auto originalHash = RoughCutProjectSerializer::mediaSha256(source);
    const auto path = QDir(output).filePath(QStringLiteral("roundtrip.subcue-project")); QVERIFY(!QFileInfo::exists(path)); QString error;
    QVERIFY2(ProjectStore(source).backup(path, &error, ProjectStore::BackupMode::Portable), qPrintable(error));
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks);
    AudioPreviewController preview(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &preview);
    QSignalSpy opened(&session, &ProjectSession::opened), saved(&session, &ProjectSession::saved);
    QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 1); QVERIFY(opened.last().first().toBool()); QTRY_VERIFY(!voice.busy());
    const auto frozen = voice.snapshot(); const auto jobs = ProjectStore(path).jobs(); const auto history = ProjectStore(path).history(); QVERIFY(voice.rowCount() >= 2);
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000); QCOMPARE(audio.rows().size(), voice.rowCount()); audio.stop();
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("before.wav")))); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000);
    const auto before = AudioQuality::inspect(QDir(output).filePath(QStringLiteral("before.wav")), &error); QVERIFY2(before, qPrintable(error));
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool()); session.newProject();
    QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 2); QVERIFY(opened.last().first().toBool()); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.snapshot(), frozen); QCOMPARE(ProjectStore(path).jobs(), jobs); QCOMPARE(ProjectStore(path).history(), history);
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("after.wav")))); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000);
    const auto after = AudioQuality::inspect(QDir(output).filePath(QStringLiteral("after.wav")), &error); QVERIFY2(after, qPrintable(error));
    QCOMPARE(after->sha256, before->sha256); QCOMPARE(after->sampleCount, before->sampleCount); QCOMPARE(after->sampleRate, 48000);
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe"), model = QStringLiteral("E:/AIModels/TTS/models/Qwen3-TTS/Qwen3-TTS-12Hz-0.6B-Base");
    voice.checkQwen(python, model); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 180000); QVERIFY(!voice.qwenStatus().isEmpty());
    voice.generateLocal(QStringLiteral("qwen"), python, model, QStringLiteral("invalid"), true); QTRY_VERIFY(!voice.busy()); QVERIFY(!tasks.busy()); QVERIFY(voice.generationPreview().isEmpty());
    QCOMPARE(ProjectStore(path).jobs(), jobs); QCOMPARE(voice.snapshot(), frozen);
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(source), originalHash); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QFile report(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(report.open(QIODevice::WriteOnly));
    report.write(QJsonDocument(QJsonObject{{QStringLiteral("sourceSha256"), QString::fromLatin1(originalHash.toHex())},
        {QStringLiteral("sentences"), voice.rowCount()}, {QStringLiteral("historyCount"), history.size()}, {QStringLiteral("before"), before->toJson()},
        {QStringLiteral("after"), after->toJson()}, {QStringLiteral("newSynthesisRequests"), 0}, {QStringLiteral("humanAudioAccepted"), false}}).toJson());
}

void EditorIntegrationTests::voiceInterruptedProjectRecoveryWhenRequested()
{
    const auto source = qEnvironmentVariable("SUBCUE_CORE_INTERRUPTED_PROJECT");
    if (source.isEmpty()) QSKIP("仅显式验证本轮中断验收工程的副本，不自动读取用户工程");
    const auto sourceHash = RoughCutProjectSerializer::mediaSha256(source); QString error;
    const auto path = QFileInfo(source).absoluteDir().filePath(QStringLiteral("recovered.subcue-project")); QVERIFY(!QFileInfo::exists(path));
    QVERIFY2(ProjectStore(source).backup(path, &error, ProjectStore::BackupMode::Portable), qPrintable(error));
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks);
    QSignalSpy opened(&session, &ProjectSession::opened), generated(&tasks, &TtsTaskController::finished);
    QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 1); QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText())); QTRY_VERIFY(!voice.busy());
    int completed = 0, interrupted = 0;
    for (const auto &value : ProjectStore(path).jobs()) { const auto job = value.toObject(); completed += job.value(QStringLiteral("state")).toString() == QStringLiteral("completed"); interrupted += job.value(QStringLiteral("state")).toString() == QStringLiteral("interrupted"); }
    QCOMPARE(completed, 1); QCOMPARE(interrupted, 11); QCOMPARE(ProjectStore(path).history().size(), 1); QCOMPARE(voice.rowCount(), 12);
    QVERIFY(AudioQuality::inspect(ProjectStore(path).resolveAudioPath(ProjectStore(path).history().first().toObject().value(QStringLiteral("audioPath")).toString())));
    AudioPreviewController player(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &player);
    const auto output = QFileInfo(path).absoluteDir().filePath(QStringLiteral("incomplete.wav")); audio.exportWav(QUrl::fromLocalFile(output)); QTRY_VERIFY(!audio.busy()); QVERIFY(!QFileInfo::exists(output));
    QVERIFY(audio.statusText().contains(QStringLiteral("缺少"))); QCOMPARE(generated.count(), 0); QVERIFY(!tasks.busy()); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(source), sourceHash);
}

void EditorIntegrationTests::voiceRealQwenWhenRequested()
{
    const auto output = qEnvironmentVariable("SUBCUE_P6_REAL_QWEN");
    if (output.isEmpty()) QSKIP("真实 Qwen 验收须显式设置输出目录，不在普通 CTest 中加载模型");
    QVERIFY(QDir().mkpath(output));
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    const auto model = QStringLiteral("E:/AIModels/TTS/models/Qwen3-TTS/Qwen3-TTS-12Hz-0.6B-Base");
    const auto reference = qEnvironmentVariable("SUBCUE_CORE_QWEN_APPROVED_REFERENCE", sourcePath(QStringLiteral("out/validation/voice-p3-20261013/speaker-2-B.wav")));
    QString error; const auto referenceQuality = AudioQuality::inspect(reference, &error); QVERIFY2(referenceQuality, qPrintable(error));
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    voice.checkQwen(python, model); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 180000); QVERIFY2(!voice.qwenStatus().isEmpty(), qPrintable(voice.statusText()));
    const auto checked = QJsonObject::fromVariantMap(voice.qwenStatus());
    QString manuscript = QStringLiteral("今天的天空很晴朗，我们一起去公园散步。\n请检查手机电量，再出发。");
    if (!qEnvironmentVariable("SUBCUE_P8_REAL_TEXT").isEmpty()) {
        QFile text(qEnvironmentVariable("SUBCUE_P8_REAL_TEXT")); QVERIFY(text.open(QIODevice::ReadOnly)); QVERIFY(text.size() <= 128 * 1024);
        manuscript = QString::fromUtf8(text.readAll()); QVERIFY(!manuscript.trimmed().isEmpty());
    }
    voice.previewText(manuscript); voice.applyPreview(); const int sentenceCount = voice.rowCount(); QVERIFY(sentenceCount >= 2);
    if (!qEnvironmentVariableIsEmpty("SUBCUE_CORE_QWEN_APPROVED_REFERENCE")) {
        voice.select(voice.snapshot().value(QStringLiteral("segments")).toArray().first().toObject().value(QStringLiteral("id")).toString());
        voice.selectMany(voice.snapshot().value(QStringLiteral("segments")).toArray().last().toObject().value(QStringLiteral("id")).toString(), false, true); QTRY_VERIFY(!voice.busy());
        voice.editSelection({{QStringLiteral("parameters"), QVariantMap{{QStringLiteral("seed"), 12345}}}});
        for (const auto &value : voice.snapshot().value(QStringLiteral("segments")).toArray()) QCOMPARE(value.toObject().value(QStringLiteral("parameters")).toObject().value(QStringLiteral("seed")).toInt(), 12345);
        voice.select(voice.snapshot().value(QStringLiteral("segments")).toArray().first().toObject().value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voice.busy());
    }
    const auto path = QDir(output).filePath(QStringLiteral("acceptance.subcue-project")); QVERIFY(!QFileInfo::exists(path));
    QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
    const auto voiceId = QUuid::createUuid().toString(QUuid::WithoutBraces), versionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 用户只确认此参考与转写；不改动全局库或其它样本的审核状态。
    QVERIFY2(ProjectStore(path).importVoiceVersion(voiceId, versionId, reference, referenceQuality->sha256,
        {{QStringLiteral("qualityState"), QStringLiteral("approved")}, {QStringLiteral("quality"), referenceQuality->toJson()}, {QStringLiteral("referenceText"), QStringLiteral("感觉耗电耗得更快了")},
         {QStringLiteral("humanReview"), QStringLiteral("用户确认用于本轮克隆验收，2026-10-13")}}, &error), qPrintable(error));
    voice.bindVoiceVersion(versionId, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.selected().value(QStringLiteral("voiceVersionId")).toString(), versionId);
    QSignalSpy generated(&tasks, &TtsTaskController::finished); QSignalSpy committed(&tasks, &TtsTaskController::candidateCommitted);
    QTimer generationDeadline; generationDeadline.setSingleShot(true); generationDeadline.setInterval(90000);
    if (!qEnvironmentVariableIsEmpty("SUBCUE_CORE_QWEN_APPROVED_REFERENCE")) {
        connect(&tasks, &TtsTaskController::busyChanged, &generationDeadline, [&] { if (tasks.busy()) generationDeadline.start(); else generationDeadline.stop(); });
        connect(&tasks, &TtsTaskController::candidateCommitted, &generationDeadline, [&] { generationDeadline.start(); });
        connect(&generationDeadline, &QTimer::timeout, &tasks, &TtsTaskController::cancel);
    }
    voice.generateQwen(python, model, QStringLiteral("all")); QTRY_VERIFY_WITH_TIMEOUT(tasks.busy() || generated.count() > 0 || !voice.busy(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 10000); QVERIFY2(tasks.busy() || generated.count() > 0, qPrintable(voice.statusText())); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 600000);
    QVERIFY2(generated.last().first().toBool(), qPrintable(generated.last().last().toString())); QCOMPARE(committed.count(), sentenceCount);
    for (const auto &result : committed) { QVERIFY(!result[3].toBool()); QVERIFY(AudioQuality::inspect(result[2].toString())); }
    QTRY_VERIFY(!voice.busy()); voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    const auto initial = voice.snapshot().value(QStringLiteral("segments")).toArray();
    QVERIFY(!initial[0].toObject().value(QStringLiteral("selectedTake")).toObject().isEmpty());
    QVERIFY(!initial[1].toObject().value(QStringLiteral("selectedTake")).toObject().isEmpty());
    AudioPreviewController preview(context.get(), &session); VoiceAudioController audio(context.get(), &session, &voice, &preview);
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000); QCOMPARE(audio.rows().size(), sentenceCount); audio.stop();
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("first-batch.wav")))); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000); QVERIFY2(QFileInfo::exists(QDir(output).filePath(QStringLiteral("first-batch.wav"))), qPrintable(audio.statusText()));
    const auto id = initial[0].toObject().value(QStringLiteral("id")).toString(); voice.select(id);
    const auto take = voice.selected().value(QStringLiteral("selectedTake")).toMap(); voice.setArrangement(0.8, 0, QStringLiteral("10"), take.value(QStringLiteral("sampleCount")).toString());
    voice.editSegment(id, QStringLiteral("今天的天空很晴朗，我们一起去湖边散步。"), {}, false, {});
    voice.generateQwen(python, model, QStringLiteral("selected")); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 2, 600000);
    QVERIFY2(generated.last().first().toBool(), qPrintable(generated.last().last().toString())); QCOMPARE(committed.count(), sentenceCount + 1); QTRY_VERIFY(!voice.busy());
    const auto changed = voice.snapshot().value(QStringLiteral("segments")).toArray();
    for (int i = 1; i < sentenceCount; ++i) QCOMPARE(changed[i], initial[i]);
    QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("id")), take.value(QStringLiteral("id")));
    voice.loadCandidates(); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.candidates().size(), 2);
    const auto replacement = voice.candidates()[0].toMap().value(QStringLiteral("id")).toString(); QVERIFY(voice.candidateIsCurrent(replacement));
    voice.applyCandidate(replacement); QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("id")), take.value(QStringLiteral("id")));
    voice.applyCandidate(replacement, QStringLiteral("reset")); QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("id")).toString(), replacement);
    QCOMPARE(voice.selected().value(QStringLiteral("gain")).toDouble(), 0.8);
    audio.exportWav(QUrl::fromLocalFile(QDir(output).filePath(QStringLiteral("regenerated.wav")))); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 30000); QVERIFY(QFileInfo::exists(QDir(output).filePath(QStringLiteral("regenerated.wav"))));
    voice.generateQwen(python, model, QStringLiteral("selected")); QTRY_VERIFY_WITH_TIMEOUT(tasks.busy(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(ProjectStore(path).jobs().first().toObject().value(QStringLiteral("state")).toString() == QStringLiteral("running"), 600000); tasks.cancel(); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 3, 30000);
    QVERIFY(!generated.last().first().toBool()); QCOMPARE(committed.count(), sentenceCount + 1); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); QVERIFY(saved.last().first().toBool());
    const auto jobs = ProjectStore(path).jobs(0, 50); int complete = 0, canceled = 0;
    for (const auto &job : jobs) { const auto record = job.toObject(); if (record.value(QStringLiteral("state")).toString() == QStringLiteral("completed")) { ++complete; QCOMPARE(record.value(QStringLiteral("result")).toObject().value(QStringLiteral("diagnostics")).toObject().value(QStringLiteral("tokenizerDtype")).toString(), QStringLiteral("torch.float32")); } if (record.value(QStringLiteral("state")).toString() == QStringLiteral("canceled")) ++canceled; }
    QCOMPARE(complete, sentenceCount + 1); QCOMPARE(canceled, 1); QCOMPARE(AudioQuality::inspect(reference)->sha256, referenceQuality->sha256);
    voice.checkQwen(python, model); QTRY_VERIFY_WITH_TIMEOUT(!voice.busy(), 180000); QCOMPARE(QJsonObject::fromVariantMap(voice.qwenStatus()).value(QStringLiteral("manifest")), checked.value(QStringLiteral("manifest")));
    QFile report(QDir(output).filePath(QStringLiteral("audit.json"))); QVERIFY(report.open(QIODevice::WriteOnly));
    QVERIFY(report.write(QJsonDocument(QJsonObject{{QStringLiteral("preflight"), checked}, {QStringLiteral("jobs"), jobs}, {QStringLiteral("reference"), reference},
        {QStringLiteral("referenceText"), QStringLiteral("感觉耗电耗得更快了")}, {QStringLiteral("manuscript"), manuscript}, {QStringLiteral("sentenceCount"), sentenceCount}, {QStringLiteral("humanAudioAccepted"), false},
        {QStringLiteral("firstBatchQuality"), AudioQuality::inspect(QDir(output).filePath(QStringLiteral("first-batch.wav")))->toJson()}}).toJson()) > 0);
    qInfo().noquote() << QStringLiteral("P6 真实验收工程：") << path;
}

void EditorIntegrationTests::voiceDocumentSharesLifecycleAndPreservesEditsDuringSave()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("第一句。第二句。\n第三句。")); voice.applyPreview(); QCOMPARE(voice.rowCount(), 3); QVERIFY(session.modified());
    const auto id = voice.selectedId(); QSemaphore started, release;
    auto blocked = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5000); }); QVERIFY(started.tryAcquire(1, 2000));
    QSignalSpy saved(&session, &ProjectSession::saved); const auto path = directory.filePath(QStringLiteral("voice.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    voice.editSegment(id, QStringLiteral("保存期间的新正文"), {}, false, {}); release.release(); blocked.waitForFinished();
    QTRY_COMPARE(saved.count(), 1); QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText())); QVERIFY(voice.modified()); QVERIFY(session.modified());
    QCOMPARE(ProjectStore(path).load()->modules.value(QStringLiteral("voiceStudio")).toObject().value(QStringLiteral("segments")).toArray()[0].toObject().value(QStringLiteral("text")).toString(), QStringLiteral("第一句。"));
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); QVERIFY(!voice.modified()); QVERIFY(!session.modified());
    session.newProject(); QCOMPARE(voice.rowCount(), 0); QVERIFY(!voice.canUndo());
    QSignalSpy opened(&session, &ProjectSession::opened); QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 1);
    QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText())); QCOMPARE(voice.rowCount(), 3); QCOMPARE(voice.selected().value(QStringLiteral("text")).toString(), QStringLiteral("保存期间的新正文"));
}
void EditorIntegrationTests::voiceDocumentMockCandidatesProtectManualCropAndVersions()
{
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe"); if (!QFileInfo::exists(python)) QSKIP("独立测试 Python 不可用");
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); session.newProject();
    TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); voice.previewText(QStringLiteral("测试句。")); voice.applyPreview();
    QSignalSpy saved(&session, &ProjectSession::saved); const auto path = directory.filePath(QStringLiteral("takes.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto voiceId = QUuid::createUuid().toString(QUuid::WithoutBraces), versionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(ProjectStore(path).importVoiceVersion(voiceId, versionId, {}, {}, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("providerVoiceId"), QStringLiteral("Cherry")}}));
    voice.bindVoiceVersion(versionId); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.selected().value(QStringLiteral("voiceVersionId")).toString(), versionId);
    QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 2); QVERIFY(!voice.modified());
    QSignalSpy generated(&tasks, &TtsTaskController::finished); voice.generateMock(python, QStringLiteral("selected")); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 10000);
    QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.candidates().size(), 1); QVERIFY(!voice.modified());
    const auto historySize = ProjectStore(path).history().size();
    voice.generateMock(python, QStringLiteral("invalid")); QTRY_VERIFY(!voice.busy());
    QCOMPARE(ProjectStore(path).history().size(), historySize); QCOMPARE(generated.count(), 1);
    voice.generateMock(python, QStringLiteral("selected"), true); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.generationPreview().value(QStringLiteral("needed")).toInt(), 1); QCOMPARE(generated.count(), 1);
    voice.discardGeneration(); QVERIFY(voice.generationPreview().isEmpty()); QCOMPARE(generated.count(), 1);
    const auto takeId = voice.candidates()[0].toMap().value(QStringLiteral("id")).toString(); voice.applyCandidate(takeId);
    auto take = voice.selected().value(QStringLiteral("selectedTake")).toMap(); QCOMPARE(take.value(QStringLiteral("id")).toString(), takeId); QVERIFY(!take.contains(QStringLiteral("path")));
    QVERIFY(voice.candidateIsCurrent(takeId)); QSignalSpy audition(&voice, &VoiceStudioController::auditionRequested);
    voice.auditionSelectedTake(); QTRY_VERIFY(!voice.busy()); QCOMPARE(audition.count(), 1); QVERIFY(QFileInfo::exists(audition.first().first().toString()));
    voice.setArrangement(0.8, 400, QStringLiteral("10"), take.value(QStringLiteral("sampleCount")).toString());
    voice.generateMock(python, QStringLiteral("selected")); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 2, 10000); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.candidates().size(), 2); const auto replacement = voice.candidates()[0].toMap().value(QStringLiteral("id")).toString();
    voice.applyCandidate(replacement); QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("id")).toString(), takeId);
    voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("id")).toString(), takeId);
    voice.applyCandidate(replacement, QStringLiteral("keep")); QCOMPARE(voice.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("trimStart")).toString(), QStringLiteral("10"));
    QCOMPARE(voice.selected().value(QStringLiteral("gain")).toDouble(), 0.8);
    voice.editSegment(voice.selectedId(), QStringLiteral("新正文"), {}, false, {}); voice.applyCandidate(takeId, QStringLiteral("reset"));
    QVERIFY(!voice.candidateIsCurrent(takeId));
    QVERIFY(voice.statusText().contains(QStringLiteral("过期"))); QVERIFY(session.saveCurrentProject()); QTRY_COMPARE(saved.count(), 3); QVERIFY(saved.last().first().toBool());
}
void EditorIntegrationTests::voiceDocumentImportDestructionHasNoLateCallback()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    QSemaphore started, release; auto blocked = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5000); }); QVERIFY(started.tryAcquire(1, 2000));
    auto voice = std::make_unique<VoiceStudioController>(context.get(), &session); QSignalSpy changes(voice.get(), &VoiceStudioController::previewChanged);
    voice->previewJson(QUrl::fromLocalFile(directory.filePath(QStringLiteral("absent.json"))), true);
    std::jthread unblock([&] { QThread::msleep(50); release.release(); }); voice.reset(); blocked.waitForFinished(); QCoreApplication::processEvents(); QCOMPARE(changes.count(), 0);
}
void EditorIntegrationTests::voiceDocumentSubtitleSyncAndJsonProtectIndependentEdits()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); session.newProject();
    Subtitle cue; cue.id = QUuid::createUuid().toString(QUuid::WithoutBraces); cue.text = QStringLiteral("字幕正文。"); cue.start = MediaTime::fromMicroseconds(1000000); cue.end = MediaTime::fromMicroseconds(2000000);
    editor.applySubtitles({cue}); voice.previewSubtitleImport(); voice.applyPreview(); QCOMPARE(voice.rowCount(), 1);
    const auto id = voice.selectedId(); voice.editSegment(id, cue.text, QStringLiteral("我手工确认的朗读"), true, QStringLiteral("不执行的备注"));
    const auto frozen = voice.snapshot(); const auto version = voice.selected().value(QStringLiteral("inputVersion"));
    cue.text = QStringLiteral("变化后的字幕正文。"); cue.start = MediaTime::fromMicroseconds(3000000); cue.end = MediaTime::fromMicroseconds(4000000); editor.applySubtitles({cue});
    voice.previewSubtitleSync(false, true); voice.applyPreview(); QCOMPARE(voice.selected().value(QStringLiteral("text")).toString(), QStringLiteral("字幕正文。"));
    QCOMPARE(voice.selected().value(QStringLiteral("inputVersion")), version); QCOMPARE(voice.selected().value(QStringLiteral("subtitleSource")).toMap().value(QStringLiteral("startUs")).toString(), QStringLiteral("3000000"));
    voice.previewSubtitleSync(true, false); voice.applyPreview(); QCOMPARE(voice.selected().value(QStringLiteral("text")).toString(), cue.text);
    QCOMPARE(voice.selected().value(QStringLiteral("spoken")).toString(), QStringLiteral("我手工确认的朗读")); QCOMPARE(voice.selected().value(QStringLiteral("inputVersion")), version);
    QCOMPARE(editor.document()->subtitles().at(0).text, cue.text); voice.previewSubtitleSync(true, true); QCOMPARE(voice.preview().size(), 0); voice.discardPreview();
    auto draft = voice.selected(); draft.insert(QStringLiteral("text"), QStringLiteral("不能部分提交")); draft.insert(QStringLiteral("seed"), QStringLiteral("-1"));
    const auto before = voice.snapshot(); QVERIFY(!voice.commitDraft(draft)); QCOMPARE(voice.snapshot(), before);
    const auto path = directory.filePath(QStringLiteral("sync.subcue-project")); QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    auto incoming = voice.snapshot(); incoming.insert(QStringLiteral("originProjectId"), session.projectId()); auto items = incoming.value(QStringLiteral("segments")).toArray();
    auto item = items[0].toObject(); item.insert(QStringLiteral("voiceVersionId"), QUuid::createUuid().toString(QUuid::WithoutBraces)); items[0] = item; incoming.insert(QStringLiteral("segments"), items);
    const auto jsonPath = directory.filePath(QStringLiteral("update.json")); QFile output(jsonPath); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QJsonDocument(incoming).toJson()); output.close();
    voice.previewJson(QUrl::fromLocalFile(jsonPath), true); QTRY_VERIFY(!voice.busy()); QVERIFY(voice.statusText().contains(QStringLiteral("映射"))); QCOMPARE(voice.snapshot(), before);
    editor.applySubtitles({}); voice.previewSubtitleSync(true, true); voice.applyPreview(); QVERIFY(voice.selected().value(QStringLiteral("sourceMissing")).toBool());
    QCOMPARE(voice.selected().value(QStringLiteral("spoken")).toString(), QStringLiteral("我手工确认的朗读"));
    voice.undo(); QCOMPARE(voice.snapshot(), before); QVERIFY(frozen != before);
}
void EditorIntegrationTests::voiceAudioCacheExportAndCancellationProtectFrozenArrangement()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); AudioPreviewController preview(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("第一句。第二句。")); voice.applyPreview(); QSignalSpy saved(&session, &ProjectSession::saved); const auto path = directory.filePath(QStringLiteral("audio.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto source = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(source); QVERIFY(quality); auto document = voice.snapshot();
    const auto initial = document.value(QStringLiteral("segments")).toArray();
    for (const auto &value : initial) { const auto item = value.toObject(); const auto job = QUuid::createUuid().toString(), take = QUuid::createUuid().toString();
        QVERIFY(ProjectStore(path).enqueue(job, {{QStringLiteral("segmentId"), item.value(QStringLiteral("id"))}})); QVERIFY(ProjectStore(path).finish(job, take, source)); QString error;
        document = VoiceDocument::applyTake(document, item.value(QStringLiteral("id")).toString(), {{QStringLiteral("id"), take}, {QStringLiteral("sampleRate"), quality->sampleRate}, {QStringLiteral("sampleCount"), QString::number(quality->sampleCount)},
            {QStringLiteral("sha256"), QString::fromLatin1(quality->sha256.toHex())}, {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex())}}, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error)); }
    voice.reset(document); VoiceAudioController audio(context.get(), &session, &voice, &preview); audio.setActive(true); audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    QVERIFY2(!audio.rows().isEmpty(), qPrintable(audio.statusText())); QCOMPARE(audio.rows().size(), 2); QVERIFY(audio.cacheBytes() > 0); QVERIFY(audio.cacheBytes() <= 1024LL * 1024 * 1024); QCOMPARE(audio.waveformBytes(), qint64(2 * 256 * sizeof(WaveformPeak))); QCOMPARE(audio.waveform(voice.selectedId()).size(), 256); QTRY_VERIFY(preview.playing());
    audio.seek(initial[1].toObject().value(QStringLiteral("id")).toString(), 0.25); QCOMPARE(voice.selectedId(), initial[1].toObject().value(QStringLiteral("id")).toString()); audio.stop();
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000); QVERIFY(audio.statusText().contains(QStringLiteral("复用"))); audio.stop();
    QSemaphore started, release; auto blocked1 = context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }); auto blocked2 = context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }); QVERIFY(started.tryAcquire(2, 2000));
    const auto output = directory.filePath(QStringLiteral("frozen.wav")); audio.exportWav(QUrl::fromLocalFile(output)); const auto before = voice.snapshot();
    voice.setArrangement(0.4, 50, QStringLiteral("0"), QString::number(quality->sampleCount)); QVERIFY(audio.rows().isEmpty()); release.release(2); blocked1.waitForFinished(); blocked2.waitForFinished(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    QVERIFY2(QFileInfo::exists(output), qPrintable(audio.statusText())); const auto rendered = AudioQuality::inspect(output); QVERIFY(rendered); QCOMPARE(rendered->sampleCount, qint64(qRound64(double(quality->sampleCount) * 48000 / quality->sampleRate) * 2 + 300 * 48)); QVERIFY(voice.snapshot() != before);
    const auto fillerPath = QFileInfo(context->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-preview-cache/") + QString(64, u'f') + QStringLiteral(".wav"));
    QFile filler(fillerPath); QVERIFY(filler.open(QIODevice::WriteOnly)); QVERIFY(filler.resize(1024LL * 1024 * 1024)); filler.close(); audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000); QVERIFY2(!audio.rows().isEmpty(), qPrintable(audio.statusText())); QVERIFY(!QFileInfo::exists(fillerPath)); QVERIFY(audio.cacheBytes() <= 1024LL * 1024 * 1024); audio.stop();
    voice.editSegment(voice.selectedId(), QStringLiteral("修改生成输入"), {}, false, {}); QSignalSpy stale(&audio, &VoiceAudioController::staleExportRequested); audio.exportWav(QUrl::fromLocalFile(directory.filePath(QStringLiteral("stale.wav")))); QTRY_VERIFY(!audio.busy()); QCOMPARE(stale.count(), 1); QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("stale.wav"))));
    audio.exportWav(QUrl::fromLocalFile(directory.filePath(QStringLiteral("sentences"))), false, true, true); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000); QCOMPARE(QDir(directory.filePath(QStringLiteral("sentences"))).entryList({QStringLiteral("*.wav")}, QDir::Files).size(), 2);
    audio.prepare(); audio.cancel(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000); QVERIFY(audio.statusText().contains(QStringLiteral("取消")));
    session.newProject(); QVERIFY(audio.rows().isEmpty()); QCOMPARE(audio.waveformBytes(), qint64(0)); QVERIFY(!preview.playing()); QCOMPARE(AudioQuality::inspect(source)->sha256, quality->sha256);
}
void EditorIntegrationTests::voiceAudioCacheRejectsChangedSource()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); AudioPreviewController preview(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("缓存源文件完整性。")); voice.applyPreview(); context->submitProjectStorage([] {}).waitForFinished();
    const auto source = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(source); QVERIFY(quality);
    auto document = voice.snapshot(); const auto item = document.value(QStringLiteral("segments")).toArray().first().toObject();
    const auto job = QUuid::createUuid().toString(), take = QUuid::createUuid().toString(); QString error;
    QVERIFY(ProjectStore(session.storagePath()).enqueue(job, {{QStringLiteral("segmentId"), item.value(QStringLiteral("id"))}}));
    QVERIFY(ProjectStore(session.storagePath()).finish(job, take, source));
    document = VoiceDocument::applyTake(document, item.value(QStringLiteral("id")).toString(), {{QStringLiteral("id"), take}, {QStringLiteral("sampleRate"), quality->sampleRate},
        {QStringLiteral("sampleCount"), QString::number(quality->sampleCount)}, {QStringLiteral("sha256"), QString::fromLatin1(quality->sha256.toHex())},
        {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex())}}, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    voice.reset(document); VoiceAudioController audio(context.get(), &session, &voice, &preview);
    const auto path = ProjectStore(session.storagePath()).resolveAudioPath(ProjectStore(session.storagePath()).assets().first().toObject().value(QStringLiteral("path")).toString());
    QVERIFY(QFileInfo(path).canonicalFilePath() != QFileInfo(source).canonicalFilePath()); QFile asset(path); QVERIFY(asset.open(QIODevice::ReadOnly)); const auto original = asset.readAll(); asset.close(); QVERIFY(original.size() > 44);
    for (const bool missing : {false, true}) {
        audio.prepare(); QTRY_VERIFY(!audio.busy()); QCOMPARE(audio.rows().size(), 1);
        audio.prepare(); QTRY_VERIFY(!audio.busy()); QVERIFY(audio.statusText().contains(QStringLiteral("复用")));
        if (missing) QVERIFY(QFile::rename(path, path + QStringLiteral(".offline")));
        else { auto changed = original; changed[changed.size() - 8] = char(changed.at(changed.size() - 8) ^ 1); QVERIFY(asset.open(QIODevice::WriteOnly)); QCOMPARE(asset.write(changed), changed.size()); asset.close(); }
        audio.prepare(); QTRY_VERIFY(!audio.busy()); const auto rows = audio.rows(); const auto waveformBytes = audio.waveformBytes(); const auto status = audio.statusText();
        audio.seek(voice.selectedId(), 0.5); const bool playbackStarted = preview.playing() || preview.loading();
        const auto output = directory.filePath(missing ? QStringLiteral("missing.wav") : QStringLiteral("changed.wav"));
        audio.exportWav(QUrl::fromLocalFile(output)); QTRY_VERIFY(!audio.busy());
        if (missing) QVERIFY(QFile::rename(path + QStringLiteral(".offline"), path));
        else { QVERIFY(asset.open(QIODevice::WriteOnly)); QCOMPARE(asset.write(original), original.size()); asset.close(); }
        QVERIFY2(rows.isEmpty(), qPrintable(status)); QCOMPARE(waveformBytes, qint64(0)); QVERIFY(!playbackStarted); QVERIFY(!QFileInfo::exists(output)); QCOMPARE(voice.snapshot(), document);
    }
    audio.prepare(); QTRY_VERIFY(!audio.busy()); QCOMPARE(audio.rows().size(), 1); QVERIFY(audio.statusText().contains(QStringLiteral("复用")));
    QCOMPARE(AudioQuality::inspect(source)->sha256, quality->sha256); QCOMPARE(ProjectStore(session.storagePath()).history().size(), 1);
}
void EditorIntegrationTests::voiceAudioDestructionHasNoLatePublication()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); AudioPreviewController preview(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("尚无音频。")); voice.applyPreview(); QSignalSpy saved(&session, &ProjectSession::saved); QVERIFY(session.saveProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("owner.sqlite"))))); QTRY_COMPARE(saved.count(), 1);
    QSemaphore started, release; auto blocked1 = context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }); auto blocked2 = context->submitAudioProcessing([&] { started.release(); (void)release.tryAcquire(1, 5000); }); QVERIFY(started.tryAcquire(2, 2000));
    auto audio = std::make_unique<VoiceAudioController>(context.get(), &session, &voice, &preview); QSignalSpy changed(audio.get(), &VoiceAudioController::changed);
    const auto output = directory.filePath(QStringLiteral("never.wav")); audio->exportWav(QUrl::fromLocalFile(output)); const auto notifications = changed.count();
    std::jthread unblock([&] { QThread::msleep(50); release.release(2); }); audio.reset(); blocked1.waitForFinished(); blocked2.waitForFinished(); QCoreApplication::processEvents();
    QCOMPARE(changed.count(), notifications); QVERIFY(!QFileInfo::exists(output)); QVERIFY(!preview.playing());
}
void EditorIntegrationTests::voiceRoughCutImportPreservesSourceDecisionsAndExistingEdits()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); VoiceStudioController voice(context.get(), &session); session.newProject();
    RoughCutProject rough; rough.mediaPath = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(rough.mediaPath); QVERIFY(quality);
    rough.mediaSha256 = quality->sha256; rough.sampleRate = quality->sampleRate; rough.channels = quality->channels; rough.sourceSampleCount = quality->sampleCount;
    rough.recording = {{QStringLiteral("r1"), QStringLiteral("先保留这句"), 0, quality->sampleCount / 2}, {QStringLiteral("r2"), QStringLiteral("再保留这句"), quality->sampleCount / 2, quality->sampleCount}};
    for (int i = 0; i < 2; ++i) { RoughCutSegmentDecision decision; decision.recordingIndex = i; decision.userDecision = RoughCutDecision::Keep; rough.decisions.append(decision); }
    rough.state.insert(QStringLiteral("timeline"), QJsonArray{QJsonObject{{QStringLiteral("start"), 0}, {QStringLiteral("end"), quality->sampleCount / 2}, {QStringLiteral("timelineStart"), 0}, {QStringLiteral("text"), QStringLiteral("先保留这句")}, {QStringLiteral("recording"), 0}, {QStringLiteral("decision"), 0}},
        QJsonObject{{QStringLiteral("start"), quality->sampleCount / 2}, {QStringLiteral("end"), quality->sampleCount}, {QStringLiteral("timelineStart"), quality->sampleCount / 2 + quality->sampleRate / 5}, {QStringLiteral("text"), QStringLiteral("再保留这句")}, {QStringLiteral("recording"), 1}, {QStringLiteral("decision"), 0}}});
    const auto path = directory.filePath(QStringLiteral("rough-source.subcue-project")); ProjectStore store(path); QVERIFY(store.create()); auto snapshot = *store.load(); QString problem; const auto module = ProjectStorageAdapter::captureModule(store, rough, snapshot.projectId, &problem); QVERIFY2(module, qPrintable(problem)); snapshot.modules.insert(QStringLiteral("roughcut"), *module); QVERIFY(store.save(snapshot));
    QSignalSpy opened(&session, &ProjectSession::opened); QVERIFY(session.openProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(opened.count(), 1); QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText()));
    const auto before = session.roughCutReferences(); const auto originalFile = RoughCutProjectSerializer::mediaSha256(path); voice.previewText(QStringLiteral("已有独立正文。")); voice.applyPreview(); const auto existing = voice.snapshot().value(QStringLiteral("segments")).toArray().first();
    const auto references = session.roughCutReferences(); QCOMPARE(references.size(), 2); QCOMPARE(session.roughCutReferences(), references); QVERIFY(!QUuid(references[0].toObject().value(QStringLiteral("id")).toString()).isNull());
    voice.previewRoughCutImport(); QCOMPARE(voice.preview().size(), 2); voice.applyPreview(); const auto imported = voice.snapshot().value(QStringLiteral("segments")).toArray(); QCOMPARE(imported.size(), 3); QCOMPARE(imported[0], existing);
    QCOMPARE(imported[1].toObject().value(QStringLiteral("pauseMs")).toDouble(), 200.0); QCOMPARE(imported[2].toObject().value(QStringLiteral("text")).toString(), QStringLiteral("再保留这句"));
    QCOMPARE(imported[1].toObject().value(QStringLiteral("roughCutSource")).toObject().value(QStringLiteral("sourceEndSample")).toString(), QString::number(quality->sampleCount / 2));
    QCOMPARE(session.roughCutReferences(), before); QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), originalFile); QVERIFY(!roughCut.modified()); QCOMPARE(roughCut.decisions().size(), 2); QVERIFY(roughCut.decisions()[0].userDecision == RoughCutDecision::Keep); voice.undo(); QCOMPARE(voice.rowCount(), 1);
}

void EditorIntegrationTests::voiceLibraryPagingAndRestore()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    const QString path = directory.filePath(QStringLiteral("pages.sqlite"));
    VoiceLibrary library(path); QVERIFY(library.initialize());
    const auto profile = library.createProfile(QStringLiteral("声音000")); QVERIFY(profile);
    for (int i = 1; i < 52; ++i) QVERIFY(library.createProfile(QStringLiteral("声音%1").arg(i, 3, 10, QLatin1Char('0'))));
    for (int i = 0; i < 51; ++i) QVERIFY(library.addVersion(*profile, {{QStringLiteral("providerVoiceId"), QStringLiteral("测试%1").arg(i)}}));
    VoiceLibraryController voices(context.get(), &session, path);
    QSignalSpy operations(&voices, &VoiceLibraryController::operationFinished);
    voices.refresh(*profile);
    QTRY_COMPARE(operations.count(), 1);
    QCOMPARE(voices.profiles().size(), 50); QCOMPARE(voices.versions().size(), 50);
    const auto firstPage = voices.versions();
    voices.refresh(*profile, false, 50, 50);
    QTRY_COMPARE(operations.count(), 2);
    QCOMPARE(voices.profileOffset(), 50); QCOMPARE(voices.versionOffset(), 50);
    QCOMPARE(voices.profiles().size(), 2); QCOMPARE(voices.versions().size(), 1);
    for (const auto &item : firstPage) QVERIFY(item.toMap().value(QStringLiteral("id")) != voices.versions().first().toMap().value(QStringLiteral("id")));
    QVERIFY(library.setProfileArchived(*profile, true));
    voices.refresh(*profile, true);
    QTRY_COMPARE(operations.count(), 3); QVERIFY(voices.includeArchived());
    QVERIFY(voices.profiles().first().toMap().value(QStringLiteral("archived")).toBool());
    voices.restoreProfile();
    QTRY_COMPARE(operations.count(), 4); QVERIFY(operations.last().first().toBool());
    QVERIFY(!voices.profiles().first().toMap().value(QStringLiteral("archived")).toBool());
    QCOMPARE(voices.versions().size(), 50);
    voices.refresh(*profile, false, -1, -1);
    QTRY_COMPARE(operations.count(), 5);
    QCOMPARE(voices.profileOffset(), 0); QCOMPARE(voices.versionOffset(), 0);
    QVERIFY(!voices.includeArchived());
}

void EditorIntegrationTests::voiceCloudCloneConsentPersistenceAndCancellation()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty()); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough);
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    const auto url = QStringLiteral("https://reference.example/approved.wav?signature=private-link");
    const auto source = QStringLiteral("https://help.aliyun.com/zh/model-studio/model-pricing");
    const auto date = QDate::currentDate().toString(Qt::ISODate);
    for (const double price : {-1.0, 10001.0, std::numeric_limits<double>::quiet_NaN()}) {
        voices.previewCloudClone(url, QStringLiteral("l4test"), price, source, date); QVERIFY(voices.cloudClone().isEmpty());
    }
    for (const auto &bad : {QStringLiteral("file:///private.wav"), QStringLiteral("http://reference.example/a.wav"), QStringLiteral("https://127.0.0.1/a.wav"), QStringLiteral("https://user:password@reference.example/a.wav")}) {
        voices.previewCloudClone(bad, QStringLiteral("l4test"), 1.0, source, date); QVERIFY(voices.cloudClone().isEmpty());
    }
    voices.previewCloudClone(url, QStringLiteral("bad-prefix"), 1.0, source, date); QVERIFY(voices.cloudClone().isEmpty());
    voices.previewCloudClone(url, QStringLiteral("l4test"), 1.0, source, date);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
    QCOMPARE(http.sendCount, 0); QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("voice-enrollment.json"))));
    voices.confirmCloudClone(true, false, true); QCOMPARE(http.sendCount, 0); QVERIFY(!voices.busy());
    {
        QLockFile lock(directory.filePath(QStringLiteral("voice-enrollment.json.lock"))); QVERIFY(lock.tryLock(0));
        voices.confirmCloudClone(true, true, true); QCOMPARE(http.sendCount, 0); QVERIFY(!voices.busy());
    }
    const auto id = QStringLiteral("qwen-audio-3.1-tts-flash-l4test-result");
    http.responses = {{200, {}, QJsonDocument(QJsonObject{{QStringLiteral("output"), QJsonObject{{QStringLiteral("voice_id"), id}}}, {QStringLiteral("request_id"), QStringLiteral("mock-enrollment-request")}, {QStringLiteral("usage"), QJsonObject{{QStringLiteral("count"), 1}}}}).toJson()},
        {200, {}, QByteArrayLiteral("{\"output\":{\"status\":\"DEPLOYING\",\"target_model\":\"qwen-audio-3.1-tts-flash\"}}")},
        {200, {}, QByteArrayLiteral("{\"output\":{\"status\":\"OK\",\"target_model\":\"qwen-audio-3.1-tts-flash\"}}")}};
    voices.confirmCloudClone(true, true, true); QTRY_VERIFY(!voices.busy());
    QCOMPARE(http.sendCount, 1); QCOMPARE(voices.cloudClone().value(QStringLiteral("voiceId")).toString(), id);
    QVERIFY(voices.versions().isEmpty()); QVERIFY(!http.requests.first().followRedirects);
    const auto input = QJsonDocument::fromJson(http.requests.first().body).object().value(QStringLiteral("input")).toObject();
    QCOMPARE(input.value(QStringLiteral("action")).toString(), QStringLiteral("create_voice")); QCOMPARE(input.value(QStringLiteral("url")).toString(), url);
    QCOMPARE(input.value(QStringLiteral("max_prompt_audio_length")).toDouble(), 10.0);
    QFile journal(directory.filePath(QStringLiteral("voice-enrollment.json"))); QVERIFY(journal.open(QIODevice::ReadOnly)); const auto bytes = journal.readAll(); journal.close();
    QVERIFY(!bytes.contains("private-link")); QVERIFY(!bytes.contains("fake-TTS-secret-not-ASR")); QVERIFY(bytes.contains("rightsAccepted"));
    QCOMPARE(QJsonDocument::fromJson(bytes).object().value(QStringLiteral("reportedVoiceCount")).toInt(), 1);
    VoiceLibraryController restored(context.get(), &session, directory.filePath(QStringLiteral("other-voices.sqlite")));
    QCOMPARE(restored.cloudClone().value(QStringLiteral("voiceId")).toString(), id);
    restored.confirmCloudClone(true, true, true); QCOMPARE(http.sendCount, 1);
    voices.useCreatedCloudClone(); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 2); QVERIFY(voices.versions().isEmpty());
    voices.useCreatedCloudClone(); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 3); QTRY_VERIFY(!voices.versions().isEmpty());
    voices.previewCloudClone(url, QStringLiteral("l4next"), 1.0, source, date);
    QCOMPARE(QDir(directory.filePath(QStringLiteral("voice-enrollment-history"))).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    http.blockUntilCancel = true; voices.confirmCloudClone(true, true, true); QTRY_VERIFY(http.inFlight.load());
    voices.cancelAndWait(); QCoreApplication::processEvents(); http.blockUntilCancel = false;
    QCOMPARE(http.sendCount, 4); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    voices.previewCloudClone(url, QStringLiteral("again"), 1.0, source, date); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    voices.confirmCloudClone(true, true, true); QCOMPARE(http.sendCount, 4);
    restored.reloadCloudClone(); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    restored.acknowledgeCloudClone(false); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    restored.acknowledgeCloudClone(true); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("acknowledged"));
    restored.previewCloudClone(url, QStringLiteral("fresh"), 1.0, source, date);
    QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty());
    restored.confirmCloudClone(true, true, true); QCOMPARE(http.sendCount, 4); QVERIFY(restored.cloudClone().isEmpty());
    QVERIFY(!voices.busy());
    auto destroyed = std::make_unique<VoiceLibraryController>(context.get(), &session);
    destroyed->previewCloudClone(url, QStringLiteral("destroy"), 1.0, source, date);
    QSignalSpy finished(destroyed.get(), &VoiceLibraryController::operationFinished);
    http.blockUntilCancel = true; destroyed->confirmCloudClone(true, true, true); QTRY_VERIFY(http.inFlight.load());
    destroyed.reset(); QCoreApplication::processEvents(); http.blockUntilCancel = false;
    QCOMPARE(http.sendCount, 5); QCOMPARE(finished.count(), 0); QVERIFY(!http.inFlight.load());
    restored.reloadCloudClone(); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    QLockFile lock(directory.filePath(QStringLiteral("voice-enrollment.json.lock"))); QVERIFY(lock.tryLock(0));
    lock.unlock(); restored.acknowledgeCloudClone(true);
    restored.previewCloudClone(url, QStringLiteral("denied"), 1.0, source, date);
    http.responses = {{401, {}, QByteArrayLiteral("{\"message\":\"private-link fake-TTS-secret-not-ASR\"}")}};
    restored.confirmCloudClone(true, true, true); QTRY_VERIFY(!restored.busy()); QCOMPARE(http.sendCount, 6);
    QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    QVERIFY(!restored.statusText().contains(QStringLiteral("private-link"))); QVERIFY(restored.statusText().contains(QStringLiteral("401")));
    restored.confirmCloudClone(true, true, true); QCOMPARE(http.sendCount, 6);
}

void EditorIntegrationTests::voiceCloudDeletionConsentPersistenceAndCancellation()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    const auto account = configureUnifiedSynthesis(context.get()); QVERIFY(!account.isEmpty());
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough); session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const auto path = directory.filePath(QStringLiteral("delete.subcue-project")); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto id = QStringLiteral("qwen-audio-3.1-tts-flash-deletetest-result");
    const HttpResponse query{200, {}, QByteArrayLiteral("{\"output\":{\"target_model\":\"qwen-audio-3.1-tts-flash\",\"status\":\"OK\",\"resource_link\":\"https://private.example/audio?signature=secret\"}}")};
    const HttpResponse deleted{200, {}, QByteArrayLiteral("{\"output\":{},\"usage\":{\"count\":1},\"request_id\":\"mock-deletion-request\"}")};
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    voices.createProfile(QStringLiteral("保留云端快照")); QTRY_VERIFY(!voices.busy());
    http.responses = {query}; voices.addCloudVersion(id); QTRY_VERIFY(!voices.busy()); QTRY_COMPARE(voices.versions().size(), 1);
    const auto version = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    voices.copyToProject(version); QTRY_VERIFY(!voices.busy());
    const auto projectVoice = ProjectStore(path).voiceVersion(version); QVERIFY(projectVoice);
    const auto libraryVoice = VoiceLibrary(directory.filePath(QStringLiteral("voices.sqlite"))).version(version); QVERIFY(libraryVoice);
    http.sendCount = 0; http.requests.clear(); http.responses = {query, deleted};
    for (const auto &invalid : {QStringLiteral("yuxiaoyun_v3.1"), QStringLiteral("other-model-custom-id"), QStringLiteral("qwen-audio-3.1-tts-flash-")}) {
        voices.previewCloudDeletion(invalid); QVERIFY(voices.cloudClone().isEmpty());
    }
    voices.previewCloudDeletion(id); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("delete_prepared"));
    QCOMPARE(http.sendCount, 0); QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("voice-enrollment.json"))));
    voices.confirmCloudDeletion(id, false); voices.confirmCloudDeletion(QStringLiteral("wrong"), true); QCOMPARE(http.sendCount, 0);
    {
        QLockFile lock(directory.filePath(QStringLiteral("voice-enrollment.json.lock"))); QVERIFY(lock.tryLock(0));
        voices.confirmCloudDeletion(id, true); QCOMPARE(http.sendCount, 0); QVERIFY(!voices.busy());
    }
    voices.confirmCloudDeletion(id, true); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 2);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("deleted"));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("reportedVoiceCount")).toInt(), 1);
    QCOMPARE(QJsonDocument::fromJson(http.requests[0].body).object().value(QStringLiteral("input")).toObject().value(QStringLiteral("action")).toString(), QStringLiteral("query_voice"));
    QCOMPARE(QJsonDocument::fromJson(http.requests[1].body).object().value(QStringLiteral("input")).toObject(), (QJsonObject{{QStringLiteral("action"), QStringLiteral("delete_voice")}, {QStringLiteral("voice_id"), id}}));
    QVERIFY(!http.requests[1].followRedirects); QVERIFY(http.requests[1].maxResponseBytes <= 65536);
    QCOMPARE(ProjectStore(path).voiceVersion(version), projectVoice);
    QCOMPARE(VoiceLibrary(directory.filePath(QStringLiteral("voices.sqlite"))).version(version), libraryVoice);
    QCOMPARE(voices.voiceCatalog().value(QStringLiteral("state")).toString(), QStringLiteral("unloaded"));
    QFile journal(directory.filePath(QStringLiteral("voice-enrollment.json"))); QVERIFY(journal.open(QIODevice::ReadOnly));
    const auto bytes = journal.readAll(); journal.close(); QVERIFY(!bytes.contains("fake-TTS-secret-not-ASR")); QVERIFY(!bytes.contains("private.example"));
    VoiceLibraryController restored(context.get(), &session);
    QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("deleted"));
    restored.confirmCloudDeletion(id, true); QCOMPARE(http.sendCount, 2);
    http.sendCount = 0; http.requests.clear(); http.responses = {{200, {}, QByteArrayLiteral("{\"output\":{\"target_model\":\"other-model\",\"status\":\"OK\"}}")}};
    voices.previewCloudDeletion(id); voices.confirmCloudDeletion(id, true); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 1);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("delete_blocked"));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("remoteOutcome")).toString(), QStringLiteral("delete_not_sent"));
    http.sendCount = 0; http.requests.clear(); http.responses = {query, deleted};
    voices.previewCloudDeletion(id); restored.previewCloudDeletion(id); restored.confirmCloudDeletion(id, true); QTRY_VERIFY(!restored.busy());
    voices.confirmCloudDeletion(id, true); QCOMPARE(http.sendCount, 2); QVERIFY(!voices.busy());
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("deleted"));
    voices.previewCloudDeletion(id);
    CloudSettingsController cloud(context.get()); cloud.begin(); QVERIFY(cloud.stageKey(account, QStringLiteral("rotated-delete-key"))); QVERIFY(cloud.save());
    voices.confirmCloudDeletion(id, true); QCOMPARE(http.sendCount, 2); QVERIFY(voices.cloudClone().isEmpty());
    http.sendCount = 0; http.requests.clear(); http.blockUntilCancel = true; http.blockAction = QStringLiteral("query_voice");
    voices.previewCloudDeletion(id); voices.confirmCloudDeletion(id, true); QTRY_VERIFY(http.inFlight.load()); voices.cancelAndWait(); QCoreApplication::processEvents();
    voices.reloadCloudClone(); QCOMPARE(http.sendCount, 1); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("delete_blocked"));
    http.sendCount = 0; http.requests.clear(); http.blockAction = QStringLiteral("delete_voice");
    voices.previewCloudDeletion(id); voices.confirmCloudDeletion(id, true); QTRY_COMPARE(http.sendCount, 2); QTRY_VERIFY(http.inFlight.load());
    voices.cancelAndWait(); QCoreApplication::processEvents(); voices.reloadCloudClone();
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    voices.confirmCloudDeletion(id, true); voices.previewCloudDeletion(id); QCOMPARE(http.sendCount, 2);
    restored.reloadCloudClone(); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    restored.acknowledgeCloudClone(false); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    restored.acknowledgeCloudClone(true); QCOMPARE(restored.cloudClone().value(QStringLiteral("remoteOutcome")).toString(), QStringLiteral("unknown"));
    http.sendCount = 0; http.requests.clear();
    auto destroyed = std::make_unique<VoiceLibraryController>(context.get(), &session);
    destroyed->previewCloudDeletion(id); QSignalSpy finished(destroyed.get(), &VoiceLibraryController::operationFinished);
    destroyed->confirmCloudDeletion(id, true); QTRY_COMPARE(http.sendCount, 2); QTRY_VERIFY(http.inFlight.load());
    destroyed.reset(); QCoreApplication::processEvents(); QCOMPARE(finished.count(), 0); QVERIFY(!http.inFlight.load());
    QLockFile lock(directory.filePath(QStringLiteral("voice-enrollment.json.lock"))); QVERIFY(lock.tryLock(0)); lock.unlock();
    restored.reloadCloudClone(); QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown")); restored.acknowledgeCloudClone(true);
    http.blockUntilCancel = false; http.blockAction.clear(); http.sendCount = 0; http.requests.clear();
    http.responses = {query, {401, {}, QByteArrayLiteral("{\"message\":\"rotated-delete-key private.example\"}")}};
    restored.previewCloudDeletion(id); restored.confirmCloudDeletion(id, true); QTRY_VERIFY(!restored.busy()); QCOMPARE(http.sendCount, 2);
    QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    QVERIFY(restored.statusText().contains(QStringLiteral("401"))); QVERIFY(!restored.statusText().contains(QStringLiteral("rotated-delete-key"))); QVERIFY(!restored.statusText().contains(QStringLiteral("private.example")));
    restored.acknowledgeCloudClone(true); http.sendCount = 0; http.requests.clear(); http.responses = {query, {200, {}, QByteArrayLiteral("{\"output\":{}}")}};
    restored.previewCloudDeletion(id); restored.confirmCloudDeletion(id, true); QTRY_VERIFY(!restored.busy());
    QCOMPARE(restored.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    restored.confirmCloudDeletion(id, true); QCOMPARE(http.sendCount, 2);
    QCOMPARE(ProjectStore(path).voiceVersion(version), projectVoice);
}

void EditorIntegrationTests::voiceCloudUpdateRevisionAndCancellation()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    const auto account = configureUnifiedSynthesis(context.get()); QVERIFY(!account.isEmpty());
    QFile fixture(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(fixture.open(QIODevice::ReadOnly)); http.downloadBody = fixture.readAll();
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); session.newProject();
    voice.previewText(QStringLiteral("保留旧音频并验证更新。")); voice.applyPreview(); QSignalSpy saved(&session, &ProjectSession::saved);
    const auto path = directory.filePath(QStringLiteral("update.subcue-project")); QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const auto id = QStringLiteral("qwen-audio-3.1-tts-flash-updatetest-result"), url = QStringLiteral("https://reference.example/new.wav?signature=private-update");
    const HttpResponse query{200, {}, QByteArrayLiteral("{\"output\":{\"target_model\":\"qwen-audio-3.1-tts-flash\",\"status\":\"OK\"}}")};
    const HttpResponse updated{200, {}, QByteArrayLiteral("{\"output\":{},\"usage\":{\"count\":1},\"request_id\":\"mock-update-request\"}")};
    const HttpResponse generated{200, {}, QByteArrayLiteral("{\"request_id\":\"mock-generation\",\"usage\":{\"input_tokens\":100,\"output_tokens\":1000},\"output\":{\"finish_reason\":\"stop\",\"audio\":{\"url\":\"https://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/test.wav\",\"expires_at\":4102444800}}}")};
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    http.responses = {query}; voices.createProfile(QStringLiteral("原有云端版本")); QTRY_VERIFY(!voices.busy()); voices.addCloudVersion(id); QTRY_VERIFY(!voices.busy()); QTRY_COMPARE(voices.versions().size(), 1);
    const auto oldVersion = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    voices.copyToProject(oldVersion); QTRY_VERIFY(!voices.busy()); voice.bindVoiceVersion(oldVersion, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    const auto oldVoice = ProjectStore(path).voiceVersion(oldVersion); QVERIFY(oldVoice);
    http.sendCount = 0; http.requests.clear(); http.responses = {generated};
    voice.prepareFlash(QStringLiteral("selected"), 1); QTRY_VERIFY(!voice.busy()); QSignalSpy finished(&tasks, &TtsTaskController::finished); voice.confirmFlash(); QTRY_COMPARE(finished.count(), 1); QVERIFY(finished.last().first().toBool());
    const auto oldJobs = ProjectStore(path).jobs(); QCOMPARE(oldJobs.size(), 1); const auto oldDocument = voice.snapshot();
    voice.prepareFlash(QStringLiteral("selected"), 1); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), 1);
    http.sendCount = 0; http.requests.clear(); http.responses = {query, updated};
    voices.previewCloudUpdate(id, QStringLiteral("http://invalid.example/a.wav")); QVERIFY(voices.cloudClone().isEmpty());
    voices.previewCloudUpdate(QStringLiteral("yuxiaoyun_v3.1"), url); QVERIFY(voices.cloudClone().isEmpty());
    voices.previewCloudUpdate(id, url); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("update_prepared")); QCOMPARE(http.sendCount, 0);
    voices.confirmCloudUpdate(id, true, false, true); voices.confirmCloudUpdate(QStringLiteral("wrong"), true, true, true); QCOMPARE(http.sendCount, 0);
    QVERIFY(context->beginCloudMutation({})); voices.confirmCloudUpdate(id, true, true, true); context->endCloudMutation(); QCOMPARE(http.sendCount, 0);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("update_blocked"));
    voices.previewCloudUpdate(id, url);
    QObject sameAccount, otherAccount; int sameCanceled = 0, otherCanceled = 0;
    QVERIFY(context->trackCloudTask(&sameAccount, account, [&] { ++sameCanceled; context->clearCloudTask(&sameAccount); }));
    QVERIFY(context->trackCloudTask(&otherAccount, QStringLiteral("other-account"), [&] { ++otherCanceled; }));
    voices.confirmCloudUpdate(id, true, true, true); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 2); QCOMPARE(sameCanceled, 1); QCOMPARE(otherCanceled, 0); context->clearCloudTask(&otherAccount);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("updated"));
    const auto input = QJsonDocument::fromJson(http.requests[1].body).object().value(QStringLiteral("input")).toObject();
    QCOMPARE(input, (QJsonObject{{QStringLiteral("action"), QStringLiteral("update_voice")}, {QStringLiteral("voice_id"), id}, {QStringLiteral("url"), url}}));
    QFile journal(directory.filePath(QStringLiteral("voice-enrollment.json"))); QVERIFY(journal.open(QIODevice::ReadOnly)); const auto bytes = journal.readAll(); journal.close();
    QVERIFY(!bytes.contains("private-update")); QVERIFY(!bytes.contains("fake-TTS-secret-not-ASR")); QVERIFY(bytes.contains("rightsAccepted"));
    const auto revisions = directory.filePath(QStringLiteral("voice-revisions")); auto binding = oldVoice->value(QStringLiteral("metadata")).toObject();
    const auto revision = CloudTtsProvider::voiceRevision(revisions, binding); QVERIFY(revision); QVERIFY(!revision->isEmpty());
    auto otherBinding = binding; otherBinding.insert(QStringLiteral("cloudAccountId"), QStringLiteral("other-account")); QCOMPARE(CloudTtsProvider::voiceRevision(revisions, otherBinding), std::optional<QString>(QString()));
    QCOMPARE(voice.snapshot(), oldDocument); QCOMPARE(ProjectStore(path).voiceVersion(oldVersion), oldVoice); QCOMPARE(ProjectStore(path).jobs(), oldJobs);
    voice.confirmFlash(); QCOMPARE(http.sendCount, 2); QVERIFY(voice.flashPreview().isEmpty());
    voice.prepareFlash(QStringLiteral("selected"), 1); QTRY_VERIFY(!voice.busy()); QVERIFY(voice.flashPreview().isEmpty()); QVERIFY(voice.statusText().contains(QStringLiteral("更新")));
    TtsJob oldJob; oldJob.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto oldRequest = TtsRequest::fromSnapshot(oldJobs.first().toObject().value(QStringLiteral("input")).toObject(), CloudTtsProvider(QStringLiteral("mock-space"), {}, 1).capabilities()); QVERIFY(oldRequest);
    oldJob.input = *oldRequest; oldJob.billing = oldJobs.first().toObject().value(QStringLiteral("input")).toObject().value(QStringLiteral("billing")).toObject();
    QVERIFY(tasks.start(CloudTtsProvider(QStringLiteral("mock-space"), QStringLiteral("unused-key"), 1, &http), {oldJob})); QTRY_COMPARE(finished.count(), 2); QVERIFY(!finished.last().first().toBool()); QCOMPARE(http.sendCount, 2);
    QSignalSpy selected(&voices, &VoiceLibraryController::voiceSelected);
    http.sendCount = 0; http.requests.clear(); http.responses = {query}; voices.useCreatedCloudClone(); QTRY_VERIFY(!voices.busy()); QTRY_COMPARE(selected.count(), 1);
    const auto newVersion = selected.last().first().toString(); QVERIFY(newVersion != oldVersion);
    QCOMPARE(VoiceLibrary(directory.filePath(QStringLiteral("voices.sqlite"))).version(newVersion)->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("cloudVoiceRevision")).toString(), *revision);
    voices.copyToProject(newVersion); QTRY_VERIFY(!voices.busy()); voice.bindVoiceVersion(newVersion, QStringLiteral("selected")); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.selected().value(QStringLiteral("voiceVersionId")).toString(), newVersion);
    voice.prepareFlash(QStringLiteral("selected"), 1); QTRY_VERIFY(!voice.busy()); QCOMPARE(voice.flashPreview().value(QStringLiteral("reused")).toInt(), 0); QCOMPARE(voice.flashPreview().value(QStringLiteral("rows")).toList().size(), 1);
    http.sendCount = 0; http.requests.clear(); http.responses = {generated}; voice.confirmFlash(); QTRY_COMPARE(finished.count(), 3); QVERIFY(finished.last().first().toBool()); QCOMPARE(http.sendCount, 1);
    const auto newJobs = ProjectStore(path).jobs(); QCOMPARE(newJobs.size(), 2); QVERIFY(newJobs.first().toObject().value(QStringLiteral("inputHash")) != oldJobs.first().toObject().value(QStringLiteral("inputHash")));
    QCOMPARE(newJobs.first().toObject().value(QStringLiteral("usage")).toObject().value(QStringLiteral("yuan")).toDouble(), 0.01215);
    http.sendCount = 0; http.requests.clear(); http.responses = {query, updated}; http.blockUntilCancel = true; http.blockAction = QStringLiteral("update_voice");
    voices.previewCloudUpdate(id, url); voices.confirmCloudUpdate(id, true, true, true); QTRY_COMPARE(http.sendCount, 2); QTRY_VERIFY(http.inFlight.load());
    QVERIFY(!context->trackCloudTask(&sameAccount, account, [] {})); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding));
    voices.cancelAndWait(); QCoreApplication::processEvents(); voices.reloadCloudClone(); QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
    QVERIFY(context->trackCloudTask(&sameAccount, account, [] {})); context->clearCloudTask(&sameAccount);
    voices.previewCloudUpdate(id, url); voices.confirmCloudUpdate(id, true, true, true); QCOMPARE(http.sendCount, 2); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding));
    voices.acknowledgeCloudClone(false); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding)); voices.acknowledgeCloudClone(true);
    const auto checkedRevision = CloudTtsProvider::voiceRevision(revisions, binding); QVERIFY(checkedRevision); QVERIFY(*checkedRevision != *revision);
    http.sendCount = 0; http.requests.clear();
    auto destroyed = std::make_unique<VoiceLibraryController>(context.get(), &session);
    destroyed->previewCloudUpdate(id, url); QSignalSpy late(destroyed.get(), &VoiceLibraryController::operationFinished);
    destroyed->confirmCloudUpdate(id, true, true, true); QTRY_COMPARE(http.sendCount, 2); QTRY_VERIFY(http.inFlight.load()); destroyed.reset(); QCoreApplication::processEvents();
    QCOMPARE(late.count(), 0); QVERIFY(!http.inFlight.load()); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding));
    QVERIFY(context->trackCloudTask(&sameAccount, account, [] {})); context->clearCloudTask(&sameAccount);
    voices.reloadCloudClone(); voices.acknowledgeCloudClone(true); const auto beforeBlockedQuery = CloudTtsProvider::voiceRevision(revisions, binding); QVERIFY(beforeBlockedQuery);
    http.blockUntilCancel = false; http.blockAction.clear(); http.sendCount = 0; http.requests.clear(); http.responses = {{200, {}, QByteArrayLiteral("{\"output\":{\"target_model\":\"other-model\",\"status\":\"OK\"}}")}};
    voices.previewCloudUpdate(id, url); voices.confirmCloudUpdate(id, true, true, true); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 1);
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("update_blocked")); QCOMPARE(CloudTtsProvider::voiceRevision(revisions, binding), beforeBlockedQuery);
    http.sendCount = 0; http.requests.clear(); http.responses = {query, {200, {}, QByteArrayLiteral("{\"output\":{}}")}};
    voices.previewCloudUpdate(id, url); voices.confirmCloudUpdate(id, true, true, true); QTRY_VERIFY(!voices.busy()); QCOMPARE(http.sendCount, 2); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("unknown")); voices.confirmCloudUpdate(id, true, true, true); QCOMPARE(http.sendCount, 2);
    voices.acknowledgeCloudClone(true);
    const auto revisionFiles = QDir(revisions).entryList({QStringLiteral("*.json")}, QDir::Files); QCOMPARE(revisionFiles.size(), 1);
    QFile corrupted(QDir(revisions).filePath(revisionFiles.first())); QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate)); corrupted.write("{"); corrupted.close(); QVERIFY(!CloudTtsProvider::voiceRevision(revisions, binding));
    voice.prepareFlash(QStringLiteral("selected"), 1); QTRY_VERIFY(!voice.busy()); QVERIFY(voice.flashPreview().isEmpty()); QCOMPARE(ProjectStore(path).voiceVersion(oldVersion), oldVoice);
}

void EditorIntegrationTests::voiceLibraryOwnerMaintainsProjectCopies()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    const QString path = directory.filePath(QStringLiteral("voices.sqlite"));
    VoiceLibraryController voices(context.get(), &session, path);
    QVERIFY(!QFileInfo::exists(path));
    QSignalSpy operations(&voices, &VoiceLibraryController::operationFinished);
    voices.createProfile(QStringLiteral("测试旁白"));
    QTRY_COMPARE(operations.count(), 1);
    QVERIFY(operations.last().first().toBool());
    QCOMPARE(voices.profiles().size(), 1);
    const QString profile = voices.selectedProfile();
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty());
    voices.addCloudVersion(QStringLiteral("yuxiaoyun_v3.1"));
    QTRY_COMPARE(operations.count(), 2);
    QVERIFY(operations.last().first().toBool());
    QCOMPARE(voices.versions().size(), 1);
    const QString version = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString project = directory.filePath(QStringLiteral("project.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(project)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY(saved.last().first().toBool());
    voices.copyToProject(version);
    QTRY_COMPARE(operations.count(), 3);
    QVERIFY(operations.last().first().toBool());
    QVERIFY(ProjectStore(project).voiceVersion(version));
    QVERIFY(!session.modified());
    voices.renameProfile(QStringLiteral("重命名"));
    QTRY_COMPARE(operations.count(), 4);
    QCOMPARE(voices.selectedProfile(), profile);
    voices.archiveProfile();
    QTRY_COMPARE(operations.count(), 5);
    QVERIFY(voices.profiles().isEmpty());
    QVERIFY(ProjectStore(project).voiceVersion(version));
    voices.addLocalVersion(QUrl(QStringLiteral("https://invalid.example/audio.wav")), QStringLiteral("禁止读取网址"));
    QCOMPARE(operations.count(), 5);
    QVERIFY(voices.statusText().contains(QStringLiteral("本地")));
    QSemaphore started, release;
    auto blocked = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5000); });
    const bool running = started.tryAcquire(1, 2000);
    voices.createProfile(QStringLiteral("不应提交"));
    voices.cancel();
    release.release(); blocked.waitForFinished();
    voices.cancelAndWait();
    QCoreApplication::processEvents();
    QVERIFY(running); QCOMPARE(operations.count(), 5);
    QVERIFY(!voices.busy()); QVERIFY(voices.profiles().isEmpty());
}

void EditorIntegrationTests::projectStorageQueueSerializesAndCancelsSaves()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    QSemaphore started;
    QSemaphore release;
    std::atomic<bool> secondRan{false};
    auto first = context->submitProjectStorage([&] {
        started.release();
        (void)release.tryAcquire(1, 5'000);
    });
    const bool firstStarted = started.tryAcquire(1, 2'000);
    auto second = context->submitProjectStorage([&] { secondRan = true; });
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    QSignalSpy subtitleSaved(&editor, &AppController::projectSaveFinished);
    QSignalSpy roughSaved(&roughCut, &RoughCutController::projectSaveFinished);
    const QString subtitlePath = directory.filePath(QStringLiteral("canceled.subcue"));
    const QString roughPath = directory.filePath(QStringLiteral("canceled.subcue-roughcut"));
    const bool subtitleQueued = editor.saveProject(QUrl::fromLocalFile(subtitlePath));
    const bool roughQueued = roughCut.saveProject(QUrl::fromLocalFile(roughPath));
    editor.cancelAlignment();
    roughCut.cancelAnalysis();
    const bool ranBeforeRelease = secondRan.load();
    // 所有断言前释放阻塞任务，测试失败也不会把队列留在等待状态。
    release.release();
    first.waitForFinished();
    second.waitForFinished();
    QVERIFY(firstStarted);
    QVERIFY(!ranBeforeRelease);
    QVERIFY(secondRan.load());
    QVERIFY(subtitleQueued);
    QVERIFY(roughQueued);
    QTRY_COMPARE(subtitleSaved.count(), 1);
    QTRY_COMPARE(roughSaved.count(), 1);
    QVERIFY(!subtitleSaved.first().first().toBool());
    QVERIFY(!roughSaved.first().first().toBool());
    QVERIFY(!QFileInfo::exists(subtitlePath));
    QVERIFY(!QFileInfo::exists(roughPath));
}

void EditorIntegrationTests::sharedProjectRoundTripAndSaveAs()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    const QString identity = session.projectId();
    QVERIFY(!identity.isEmpty());
    editor.setScriptText(QStringLiteral("字幕正文"));
    QFile script(directory.filePath(QStringLiteral("文稿.txt")));
    QVERIFY(script.open(QIODevice::WriteOnly)); script.write(QStringLiteral("粗剪正文").toUtf8()); script.close();
    roughCut.loadScript(QUrl::fromLocalFile(script.fileName()));
    QTRY_VERIFY(!roughCut.busy());
    QCOMPARE(roughCut.scriptText(), QStringLiteral("粗剪正文"));
    QVERIFY(session.modified());
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString firstPath = directory.filePath(QStringLiteral("原始.subcue-project"));
    QVERIFY(editor.saveProject(QUrl::fromLocalFile(firstPath)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(session.projectId(), identity);
    QCOMPARE(editor.projectPath(), firstPath);
    QCOMPARE(roughCut.projectPath(), firstPath);
    QVERIFY(!session.modified());
    const QString firstOwnedScript = roughCut.scriptPath();
    QVERIFY(firstOwnedScript != script.fileName());
    QVERIFY(QFileInfo::exists(firstOwnedScript));
    QString error;
    ProjectStorageAdapter adapter(firstPath, ProjectStorageAdapter::Format::UnifiedSqlite);
    const auto subtitle = adapter.loadModule(QStringLiteral("subtitle"), &error);
    const auto rough = adapter.loadModule(QStringLiteral("roughcut"), &error);
    QVERIFY2(subtitle && rough, qPrintable(error));
    QCOMPARE(std::get<Project>(*subtitle).state.value(QStringLiteral("scriptText")).toString(), QStringLiteral("字幕正文"));
    QCOMPARE(std::get<RoughCutProject>(*rough).scriptText, QStringLiteral("粗剪正文"));
    ProjectStore original(firstPath);
    QVERIFY(original.enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("历史输入")}}, &error));
    QFile audio(directory.filePath(QStringLiteral("storage-fixture.bin")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.write("only a storage fixture"); audio.close();
    QVERIFY2(original.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName(), &error), qPrintable(error));
    const auto before = original.load(&error);
    QVERIFY(before);
    editor.setScriptText(QStringLiteral("另存正文"));
    const QString secondPath = directory.filePath(QStringLiteral("另存 #1.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(secondPath)));
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(session.projectId(), identity);
    QCOMPARE(original.load(&error)->revision, before->revision);
    QCOMPARE(ProjectStore(secondPath).history(0, 50, &error).size(), 1);
    QVERIFY(!session.modified());
    QVERIFY(roughCut.scriptPath() != firstOwnedScript);
    QVERIFY(QFileInfo::exists(roughCut.scriptPath()));
    QVERIFY(script.remove());
    QVERIFY(QFile::remove(firstOwnedScript));
    editor.setScriptText(QStringLiteral("再次保存"));
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 3);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(!session.modified());
    QVERIFY(original.enqueue(QStringLiteral("interrupted-job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("等待恢复")}}, &error));
    QSignalSpy opened(&session, &ProjectSession::opened);
    QVERIFY(session.openProject(QUrl::fromLocalFile(firstPath)));
    QTRY_COMPARE(opened.count(), 1);
    QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(editor.scriptText(), QStringLiteral("字幕正文"));
    QCOMPARE(roughCut.scriptText(), QStringLiteral("粗剪正文"));
    QCOMPARE(session.projectId(), identity);
    const auto recovered = original.jobs(0, 1).first().toObject();
    QCOMPARE(recovered.value(QStringLiteral("id")).toString(), QStringLiteral("interrupted-job"));
    QCOMPARE(recovered.value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
    QVERIFY(session.statusText().contains(QStringLiteral("1 个中断任务")));
}

void EditorIntegrationTests::sharedProjectSavePreservesNewEdits()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    WorkspaceRouter router(&editor, &roughCut);
    session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString path = directory.filePath(QStringLiteral("editing.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QSemaphore started, release;
    auto preceding = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5'000); });
    const bool startedInTime = started.tryAcquire(1, 2'000);
    editor.setScriptText(QStringLiteral("已捕获文本"));
    const bool queued = session.saveCurrentProject();
    editor.setScriptText(QStringLiteral("保存期间的新编辑"));
    const bool switched = router.switchTo(QStringLiteral("roughcut"));
    release.release();
    preceding.waitForFinished();
    QVERIFY(startedInTime);
    QVERIFY(queued);
    QVERIFY(switched);
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(editor.scriptText(), QStringLiteral("保存期间的新编辑"));
    QVERIFY(session.modified());
    QString error;
    const auto snapshot = ProjectStorageAdapter(path, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"), &error);
    QVERIFY2(snapshot, qPrintable(error));
    QCOMPARE(std::get<Project>(*snapshot).state.value(QStringLiteral("scriptText")).toString(), QStringLiteral("已捕获文本"));
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 3);
    QVERIFY(saved.last().first().toBool());
    QVERIFY(!session.modified());
}

namespace {
// 只收集工程自身的产物，避免把设置、恢复草稿等无关文件算进“重复保存多出一份”。
QStringList projectArtifacts(const QString &root)
{
    QStringList names;
    QDirIterator iterator(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (QFileInfo(path).fileName().contains(QStringLiteral(".subcue-project"))) names.append(QDir(root).relativeFilePath(path));
    }
    names.sort();
    return names;
}
}

void EditorIntegrationTests::sharedProjectRepeatSaveCreatesNoDuplicateFiles()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    editor.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(editor.hasMedia() && !editor.busy(), 60000);
    // 粗剪模块也要带上素材身份，避免统一工程重开后导不了。
    roughCut.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy() && roughCut.mediaAvailable(), 60000);
    const QString script = directory.filePath(QStringLiteral("粗剪文稿.txt"));
    QFile scriptFile(script);
    QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write(QString::fromUtf8("第一句\n第二句\n").toUtf8());
    scriptFile.close();
    roughCut.loadScript(QUrl::fromLocalFile(script));
    QTRY_VERIFY(!roughCut.busy());
    QString error;
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString path = directory.filePath(QStringLiteral("重复保存.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    // 保存成功后不能再显示“未保存”，否则退出时会再次弹出未保存提示。
    QVERIFY2(!editor.modified(), qPrintable(session.statusText()));
    QVERIFY2(!roughCut.modified(), qPrintable(session.statusText()));
    QVERIFY(!session.modified());
    const auto firstArtifacts = projectArtifacts(directory.path());
    QVERIFY(firstArtifacts.contains(QStringLiteral("重复保存.subcue-project")));
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    // 重复保存只更新同一个工程，不能多出一份工程或资产副本。
    QCOMPARE(projectArtifacts(directory.path()), firstArtifacts);
    QVERIFY(!editor.modified());
    QVERIFY(!session.modified());
    editor.setScriptText(QStringLiteral("第二次保存"));
    QVERIFY(session.modified());
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 3);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(projectArtifacts(directory.path()), firstArtifacts);
    QVERIFY(!session.modified());
    const auto module = ProjectStorageAdapter(path, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"), &error);
    QVERIFY2(module, qPrintable(error));
    QCOMPARE(std::get<Project>(*module).state.value(QStringLiteral("scriptText")).toString(), QStringLiteral("第二次保存"));
    const auto roughModule = ProjectStorageAdapter(path, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("roughcut"), &error);
    QVERIFY2(roughModule, qPrintable(error));
    QCOMPARE(std::get<RoughCutProject>(*roughModule).mediaSha256.size(), 32);
}

void EditorIntegrationTests::sharedProjectFailedSaveAsPreservesEdits_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("existing-project") << QStringLiteral("project");
    QTest::newRow("existing-assets") << QStringLiteral("assets");
    QTest::newRow("parent-is-file") << QStringLiteral("parent");
}

void EditorIntegrationTests::sharedProjectFailedSaveAsPreservesEdits()
{
    QFETCH(QString, failure);
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
    const auto original = directory.filePath(QStringLiteral("original.subcue-project"));
    QSignalSpy saved(&session, &ProjectSession::saved);
    QVERIFY(session.saveProject(QUrl::fromLocalFile(original))); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
    editor.setScriptText(QStringLiteral("尚未保存的字幕")); rough.setScriptText(QStringLiteral("尚未保存的粗剪"));
    voice.previewText(QStringLiteral("尚未保存的旁白。")); voice.applyPreview();
    voice.editSegment(voice.selectedId(), QStringLiteral("尚未保存的旁白。"), QStringLiteral("人工朗读"), true, QStringLiteral("保留备注"));
    const auto document = voice.snapshot(); const auto id = session.projectId(); const auto generation = session.generation();
    session.setRecoveryDrafts({{QStringLiteral("voice"), QVariantMap{{QStringLiteral("text"), QStringLiteral("尚未提交字段")}}}});
    const auto drafts = session.recoveryDrafts(); const auto hash = RoughCutProjectSerializer::mediaSha256(original);
    const auto target = directory.filePath(QStringLiteral("blocked/result.subcue-project"));
    QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
    if (failure == QStringLiteral("assets")) QVERIFY(QDir().mkpath(target + QStringLiteral(".assets")));
    const auto protectedPath = failure == QStringLiteral("project") ? target : failure == QStringLiteral("assets")
        ? target + QStringLiteral(".assets/sentinel") : directory.filePath(QStringLiteral("parent-file"));
    QFile sentinel(protectedPath); QVERIFY(sentinel.open(QIODevice::WriteOnly)); QCOMPARE(sentinel.write("do not overwrite"), qint64(16)); sentinel.close();
    const auto destination = failure == QStringLiteral("parent") ? protectedPath + QStringLiteral("/result.subcue-project") : target;
    QVERIFY(session.saveProject(QUrl::fromLocalFile(destination))); QTRY_COMPARE(saved.count(), 2);
    QVERIFY(!saved.last().first().toBool()); QVERIFY(!session.statusText().isEmpty());
    QCOMPARE(session.path(), original); QCOMPARE(session.storagePath(), original); QCOMPARE(session.projectId(), id); QCOMPARE(session.generation(), generation);
    QCOMPARE(editor.scriptText(), QStringLiteral("尚未保存的字幕")); QCOMPARE(rough.scriptText(), QStringLiteral("尚未保存的粗剪"));
    QCOMPARE(voice.snapshot(), document); QCOMPARE(session.recoveryDrafts(), drafts); QVERIFY(session.modified()); QVERIFY(!session.busy());
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(original), hash);
    QVERIFY(sentinel.open(QIODevice::ReadOnly)); QCOMPARE(sentinel.readAll(), QByteArray("do not overwrite")); sentinel.close();
    QVERIFY(QDir(directory.path()).entryList({QStringLiteral(".subcue-save-project-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    voice.undo(); QCOMPARE(voice.selected().value(QStringLiteral("spoken")).toString(), QStringLiteral("尚未保存的旁白。")); voice.redo(); QCOMPARE(voice.snapshot(), document);
    const auto retry = directory.filePath(QStringLiteral("retry.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(retry))); QTRY_COMPARE(saved.count(), 3); QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(session.path(), retry); QVERIFY(!session.modified());
    QCOMPARE(ProjectStore(retry).load()->modules.value(QStringLiteral("voiceStudio")).toObject(), document);
}

void EditorIntegrationTests::sharedProjectFailedOpenPreservesCurrent()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session);
    session.newProject();
    editor.setScriptText(QStringLiteral("手工字幕"));
    roughCut.setScriptText(QStringLiteral("手工粗剪"));
    voice.previewText(QStringLiteral("保留当前配音正文。")); voice.applyPreview();
    voice.editSegment(voice.selectedId(), QStringLiteral("保留当前配音正文。"), QStringLiteral("独立手工朗读"), true, QStringLiteral("人工备注"));
    const auto document = voice.snapshot();
    session.setRecoveryDrafts({{QStringLiteral("voice"), QVariantMap{{QStringLiteral("text"), QStringLiteral("尚未提交的字段")}}}});
    const auto drafts = session.recoveryDrafts();
    const auto generation = session.generation();
    const QString identity = session.projectId();
    QFile corrupt(directory.filePath(QStringLiteral("broken.subcue-project")));
    QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("broken database"); corrupt.close();
    QSignalSpy opened(&session, &ProjectSession::opened);
    QVERIFY(session.openProject(QUrl::fromLocalFile(corrupt.fileName())));
    QTRY_COMPARE(opened.count(), 1);
    QVERIFY(!opened.last().first().toBool());
    QVERIFY(!session.busy());
    QVERIFY(!editor.busy());
    QVERIFY(!roughCut.busy());
    QCOMPARE(session.projectId(), identity);
    QCOMPARE(editor.scriptText(), QStringLiteral("手工字幕"));
    QCOMPARE(roughCut.scriptText(), QStringLiteral("手工粗剪"));
    QCOMPARE(voice.snapshot(), document); QCOMPARE(session.recoveryDrafts(), drafts);
    QCOMPARE(session.generation(), generation);
    QVERIFY(session.modified());
    const auto destination = directory.filePath(QStringLiteral("failed-restore.subcue-project"));
    QVERIFY(session.restoreProject(QUrl::fromLocalFile(corrupt.fileName()), QUrl::fromLocalFile(destination)));
    QTRY_COMPARE(opened.count(), 2); QVERIFY(!opened.last().first().toBool());
    QCOMPARE(session.projectId(), identity); QCOMPARE(session.generation(), generation);
    QCOMPARE(editor.scriptText(), QStringLiteral("手工字幕"));
    QCOMPARE(roughCut.scriptText(), QStringLiteral("手工粗剪"));
    QCOMPARE(voice.snapshot(), document); QCOMPARE(session.recoveryDrafts(), drafts);
    QVERIFY(!session.busy()); QVERIFY(session.modified()); QVERIFY(!session.changingProject());
    QVERIFY(!QFileInfo::exists(destination)); QVERIFY(!QFileInfo::exists(destination + QStringLiteral(".assets")));
    QVERIFY(QDir(directory.path()).entryList({QStringLiteral(".subcue-backup-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    voice.undo(); QCOMPARE(voice.selected().value(QStringLiteral("spoken")).toString(), QStringLiteral("保留当前配音正文。"));
}

void EditorIntegrationTests::sharedProjectCancelQueuedSaveOnShutdown()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    auto session = std::make_unique<ProjectSession>(context.get(), &editor, &roughCut);
    session->newProject();
    QSemaphore started, release;
    auto preceding = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5'000); });
    QVERIFY(started.tryAcquire(1, 2'000));
    const QString path = directory.filePath(QStringLiteral("not-published.subcue-project"));
    QVERIFY(session->saveProject(QUrl::fromLocalFile(path)));
    session->cancel();
    std::jthread unblock([&] { QThread::msleep(50); release.release(); });
    session.reset();
    QVERIFY(preceding.isFinished());
    QVERIFY(context->projectSession == nullptr);
    QVERIFY(!QFileInfo::exists(path));
    QCoreApplication::processEvents();
}

void EditorIntegrationTests::sharedLegacyLifecycle_data()
{
    QTest::addColumn<bool>("subtitle");
    QTest::newRow("subtitle-json") << true;
    QTest::newRow("roughcut-sqlite") << false;
}

void EditorIntegrationTests::sharedLegacyLifecycle()
{
    QFETCH(bool, subtitle);
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    const QString module = subtitle ? QStringLiteral("subtitle") : QStringLiteral("roughcut");
    const auto format = subtitle ? ProjectStorageAdapter::Format::SubtitleJson : ProjectStorageAdapter::Format::RoughCutSqlite;
    const QString source = directory.filePath(subtitle ? QStringLiteral("旧 #字幕.subcue") : QStringLiteral("旧 #粗剪.subcue-roughcut"));
    QString error;
    if (subtitle) {
        Project project;
        project.state.insert(QStringLiteral("scriptText"), QStringLiteral("原有正文"));
        project.metadata.insert(QStringLiteral("unknown"), QStringLiteral("保留"));
        QVERIFY(ProjectSerializer::save(source, project, &error));
    } else {
        RoughCutProject project;
        project.scriptText = QStringLiteral("原有正文");
        project.state.insert(QStringLiteral("unknown"), QStringLiteral("保留"));
        QVERIFY(RoughCutProjectSerializer::save(source, project, &error));
    }
    const auto initialHash = RoughCutProjectSerializer::mediaSha256(source);
    QSignalSpy opened(&session, &ProjectSession::opened);
    if (subtitle) editor.openProject(QUrl::fromLocalFile(source));
    else roughCut.openProject(QUrl::fromLocalFile(source));
    QTRY_COMPARE(opened.count(), 1);
    QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(session.active());
    QVERIFY(!session.unified());
    QCOMPARE(session.legacyModule(), module);
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(source), initialHash);
    QVERIFY(!session.modified());
    const QString identity = session.projectId();
    QSignalSpy saved(&session, &ProjectSession::saved);
    if (subtitle) editor.setScriptText(QStringLiteral("编辑后正文"));
    else roughCut.setScriptText(QStringLiteral("编辑后正文"));
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(!session.modified());
    QCOMPARE(ProjectStorageAdapter(source, format).load()->projectId, identity);
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY(saved.last().first().toBool());
    const QString copy = directory.filePath(subtitle ? QStringLiteral("copy.subcue") : QStringLiteral("copy.subcue-roughcut"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(copy)));
    QTRY_COMPARE(saved.count(), 3);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(session.path(), copy);
    QCOMPARE(session.projectId(), identity);
    // 模拟外部程序正常保存；当前会话不能悄悄采纳其版本并覆盖。
    const auto external = ProjectStorageAdapter(copy, format).load();
    QVERIFY(external);
    if (subtitle) {
        auto data = std::get<Project>(external->data);
        data.metadata.insert(QStringLiteral("external"), true);
        QVERIFY(ProjectSerializer::save(copy, data));
    } else {
        auto data = std::get<RoughCutProject>(external->data);
        data.state.insert(QStringLiteral("external"), true);
        QVERIFY(RoughCutProjectSerializer::save(copy, data));
    }
    const auto externalHash = RoughCutProjectSerializer::mediaSha256(copy);
    QVERIFY(session.saveCurrentProject());
    QTRY_COMPARE(saved.count(), 4);
    QVERIFY(!saved.last().first().toBool());
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(copy), externalHash);
    QVERIFY(session.openProject(QUrl::fromLocalFile(copy), module));
    QTRY_COMPARE(opened.count(), 2);
    QVERIFY(opened.last().first().toBool());
    const QString converted = directory.filePath(QStringLiteral("converted.subcue-project"));
    QVERIFY(session.convertProject(copy, module, QUrl::fromLocalFile(converted)));
    QTRY_COMPARE(opened.count(), 3);
    QVERIFY2(opened.last().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(session.unified());
    QVERIFY(session.projectId() != identity);
    QCOMPARE(RoughCutProjectSerializer::mediaSha256(copy), externalHash);
    const auto result = ProjectStorageAdapter(converted, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(module, &error);
    QVERIFY2(result, qPrintable(error));
    const auto metadata = subtitle ? std::get<Project>(*result).metadata : std::get<RoughCutProject>(*result).state;
    QCOMPARE(metadata.value(QStringLiteral("unknown")).toString(), QStringLiteral("保留"));
    QVERIFY(metadata.value(QStringLiteral("external")).toBool());
}

void EditorIntegrationTests::qmlSharedProjectWindow()
{
#ifdef Q_OS_WIN
    // offscreen 测试默认找不到系统字体，截图里的中文会变成方块，无法作为人工验收证据。
    qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")).toUtf8());
#endif
    QTemporaryDir directory;
    const QString modulePath = directory.filePath(QStringLiteral("SubCue"));
    QVERIFY(QDir().mkpath(modulePath));
    QByteArray manifest("module SubCue\n");
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml"))).entryList({QStringLiteral("*.qml")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/") + file), QDir(modulePath).filePath(file)));
        if (file == QStringLiteral("Theme.qml")) manifest += "singleton ";
        manifest += QFileInfo(file).baseName().toUtf8() + " 1.0 " + file.toUtf8() + "\n";
    }
    QVERIFY(QDir().mkpath(QDir(modulePath).filePath(QStringLiteral("icons"))));
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList(QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file), QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    }
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly)); qmldir.write(manifest); qmldir.close();
    registerSubCueQmlItems();
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    WorkspaceRouter router(&editor, &roughCut);
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("ui-voices.sqlite")));
    VoiceStudioController voiceDocument(context.get(), &session);
    AudioPreviewController preview(context.get(), &session); router.setAudioPreview(&preview);
    VoiceAudioController voiceAudio(context.get(), &session, &voiceDocument, &preview);
    QQmlApplicationEngine engine;
    TtsTaskController taskRecords(context.get(), &session);
    engine.addImportPath(directory.path());
    QQmlComponent nativeTheme(&engine);
    nativeTheme.setData("import QtQuick; import QtQuick.Controls; QtObject { property bool expandedClientArea: false; property int discardButton: Dialog.Discard; function applyDarkTitleBar() {} }", QUrl());
    QObject *theme = nativeTheme.create();
    QVERIFY(theme); theme->setParent(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &editor);
    engine.rootContext()->setContextProperty(QStringLiteral("roughCut"), &roughCut);
    engine.rootContext()->setContextProperty(QStringLiteral("appRouter"), &router);
    engine.rootContext()->setContextProperty(QStringLiteral("projectSession"), &session);
    engine.rootContext()->setContextProperty(QStringLiteral("ttsTasks"), &taskRecords);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceLibrary"), &voices);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceStudio"), &voiceDocument);
    engine.rootContext()->setContextProperty(QStringLiteral("audioPreview"), &preview);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceAudio"), &voiceAudio);
    engine.rootContext()->setContextProperty(QStringLiteral("gpuQueue"), &InferenceManager::instance());
    auto batch = InferenceManager::instance().acquire(nullptr, 2000, QStringLiteral("调度测试批次"), QStringLiteral("test-batch"));
    QVERIFY(batch);
    std::atomic<bool> cancelGpu = false;
    std::jthread waiting([&] {
        auto queued = InferenceManager::instance().acquire(&cancelGpu, 5000, QStringLiteral("调度测试 ASR"), QStringLiteral("test-asr"));
    });
    QTRY_COMPARE(InferenceManager::instance().tasks().size(), 2);
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(QDir(modulePath).filePath(QStringLiteral("AppRouter.qml"))));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(u'\n')));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    // 固定主屏，避免高 DPI 回归的鼠标坐标落到另一屏或屏幕外。
    window->setScreen(QGuiApplication::primaryScreen());
    window->setPosition(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(24, 24));
    QTRY_COMPARE(window->devicePixelRatio(), QGuiApplication::primaryScreen()->devicePixelRatio());
    QCOMPARE(window->property("sharedProject").value<QObject *>(), &session);
    auto *gpuButton = window->findChild<QObject *>(QStringLiteral("gpuQueueButton"));
    auto *gpuMenu = window->findChild<QObject *>(QStringLiteral("gpuQueueMenu"));
    QVERIFY(gpuButton && gpuMenu);
    QTRY_VERIFY(gpuButton->property("visible").toBool());
    QVERIFY(gpuButton->property("text").toString().contains(u'2'));
    QVERIFY(QMetaObject::invokeMethod(gpuButton, "clicked"));
    QTRY_VERIFY(gpuMenu->property("visible").toBool());
    QTRY_COMPARE(gpuMenu->property("count").toInt(), 2);
    QVERIFY(QMetaObject::invokeMethod(gpuMenu, "close"));
    QVERIFY(router.switchTo(QStringLiteral("roughcut")));
    editor.setScriptText(QStringLiteral("另一个视图的未保存编辑"));
    QTRY_VERIFY(window->title().endsWith(QStringLiteral(" *")));
    QSignalSpy saved(&session, &ProjectSession::saved);
    QVERIFY(session.saveProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("shared.subcue-project")))));
    QTRY_VERIFY2(saved.count() == 1, qPrintable(session.statusText() + QStringLiteral(" voice=%1/%2 library=%3 audio=%4 warnings=%5").arg(voiceDocument.busy()).arg(voiceDocument.modifying()).arg(voices.busy()).arg(voiceAudio.busy()).arg(warnings.join(u'\n'))));
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QTRY_VERIFY(!window->title().endsWith(QStringLiteral(" *")));
    QVERIFY(!window->property("storageBusy").toBool());
    QVERIFY(!window->property("modalEditing").toBool());
    editor.setScriptText(QStringLiteral("第二次保存更新原工程"));
    QVERIFY(QMetaObject::invokeMethod(window, "requestSave", Q_ARG(QVariant, QStringLiteral("subtitle")), Q_ARG(QVariant, false)));
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QCOMPARE(session.path(), directory.filePath(QStringLiteral("shared.subcue-project")));
    auto *saveDialog = window->findChild<QObject *>(QStringLiteral("saveProjectDialog"));
    QVERIFY(saveDialog); QVERIFY(!saveDialog->property("visible").toBool());
    const auto updatedSubtitle = ProjectStorageAdapter(session.path(), ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"));
    QVERIFY(updatedSubtitle);
    QCOMPARE(std::get<Project>(*updatedSubtitle).state.value(QStringLiteral("scriptText")).toString(), editor.scriptText());
    QVERIFY(!session.modified());
    ProjectStore::JobOptions historyOptions; historyOptions.batchId = QStringLiteral("ui-batch");
    QVERIFY(ProjectStore(session.path()).enqueue(QStringLiteral("ui-history"), QJsonObject{{QStringLiteral("text"), QStringLiteral("待审核历史输入")}}, historyOptions));
    auto *dialog = window->findChild<QObject *>(QStringLiteral("ttsTasksDialog"));
    QVERIFY(dialog); QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_COMPARE(taskRecords.records().size(), 1);
    QVERIFY(taskRecords.recordsError().isEmpty());
    auto *recordsList = window->findChild<QObject *>(QStringLiteral("ttsTaskRecordsList"));
    QVERIFY(recordsList); QTRY_COMPARE(recordsList->property("count").toInt(), 1);
    QVERIFY(window->property("modalEditing").toBool());
    QVERIFY(dialog->setProperty("batchFilter", QStringLiteral("ui-batch")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "refresh"));
    QTRY_VERIFY(!taskRecords.recordsLoading()); QCOMPARE(taskRecords.records().size(), 1);
    QTest::qWait(150);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/tts-record-dialog.png"))));
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    auto *voiceDialog = window->findChild<QObject *>(QStringLiteral("voiceLibraryDialog"));
    QVERIFY(voiceDialog); QVERIFY(QMetaObject::invokeMethod(voiceDialog, "open"));
    QTRY_VERIFY(!voices.busy());
    voices.createProfile(QStringLiteral("中文长文旁白"));
    QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.profiles().size(), 1);
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty());
    voices.addCloudVersion(QStringLiteral("yuxiaoyun_v3.1"));
    QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.versions().size(), 1);
    voices.setDefaultVersion(voices.versions().first().toMap().value(QStringLiteral("id")).toString());
    QTRY_VERIFY(!voices.busy()); QVERIFY(voices.versions().first().toMap().value(QStringLiteral("isDefault")).toBool());
    auto *voiceList = window->findChild<QObject *>(QStringLiteral("voiceVersionsList"));
    QVERIFY(voiceList); QTRY_COMPARE(voiceList->property("count").toInt(), 1);
    QTest::qWait(150);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-library-dialog.png"))));
    auto *cloneDialog = window->findChild<QObject *>(QStringLiteral("cloudCloneDialog"));
    auto *cloneAction = window->findChild<QQuickItem *>(QStringLiteral("confirmCloudClone"));
    auto *cloneScroll = window->findChild<QObject *>(QStringLiteral("cloudCloneScroll"));
    QVERIFY(cloneDialog && cloneAction && cloneScroll); QVERIFY(QMetaObject::invokeMethod(cloneDialog, "open"));
    QVERIFY(!cloneAction->isEnabled());
    for (const auto &field : {"cloudCloneRights", "cloudClonePrivacy", "cloudCloneCost"}) {
        auto *check = window->findChild<QObject *>(QString::fromLatin1(field)); QVERIFY(check); QVERIFY(!check->property("checked").toBool());
    }
    const QVariantMap cloneFields{{QStringLiteral("cloudCloneUrl"), QStringLiteral("https://reference.example/approved.wav?signature=hidden")},
        {QStringLiteral("cloudClonePrefix"), QStringLiteral("uitest")}, {QStringLiteral("cloudClonePrice"), QStringLiteral("1.0")},
        {QStringLiteral("cloudClonePriceSource"), QStringLiteral("https://help.aliyun.com/zh/model-studio/model-pricing")}, {QStringLiteral("cloudClonePriceDate"), QDate::currentDate().toString(Qt::ISODate)}};
    for (auto it = cloneFields.begin(); it != cloneFields.end(); ++it) { auto *field = window->findChild<QObject *>(it.key()); QVERIFY(field); field->setProperty("text", it.value()); }
    auto *clonePreview = window->findChild<QObject *>(QStringLiteral("previewCloudClone")); QVERIFY(clonePreview);
    QVERIFY(QMetaObject::invokeMethod(clonePreview, "clicked"));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
    QVERIFY(!cloneAction->isEnabled());
    for (const auto &field : {"cloudCloneRights", "cloudClonePrivacy", "cloudCloneCost"}) window->findChild<QObject *>(QString::fromLatin1(field))->setProperty("checked", true);
    QTRY_VERIFY(cloneAction->isEnabled());
    auto *cloneFlickable = cloneScroll->property("contentItem").value<QObject *>(); QVERIFY(cloneFlickable);
    const QSize cloneWindowSize = window->size();
    for (const auto &size : {QSize(900, 560), QSize(1366, 768), QSize(1920, 1080)}) {
        window->resize(size); QTest::qWait(100);
        cloneFlickable->setProperty("contentY", qMax(0., cloneFlickable->property("contentHeight").toDouble() - cloneFlickable->property("height").toDouble()));
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-cloud-clone-%1.png").arg(size.width()))));
    }
    QVERIFY(QMetaObject::invokeMethod(cloneDialog, "close")); QTRY_VERIFY(voices.cloudClone().isEmpty());
    window->resize(cloneWindowSize); QTest::qWait(100);
    auto *deletionDialog = window->findChild<QObject *>(QStringLiteral("cloudDeletionDialog"));
    auto *deletionAction = window->findChild<QQuickItem *>(QStringLiteral("confirmCloudDeletion"));
    auto *deletionAccepted = window->findChild<QObject *>(QStringLiteral("cloudDeletionAccepted"));
    auto *deletionTyped = window->findChild<QObject *>(QStringLiteral("cloudDeletionTyped"));
    QVERIFY(deletionDialog && deletionAction && deletionAccepted && deletionTyped); QVERIFY(QMetaObject::invokeMethod(deletionDialog, "open"));
    QVERIFY(!deletionAccepted->property("checked").toBool()); QVERIFY(!deletionAction->isVisible());
    window->findChild<QObject *>(QStringLiteral("cloudDeletionVoice"))->setProperty("text", QStringLiteral("qwen-audio-3.1-tts-flash-uitest-delete"));
    QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("previewCloudDeletion")), "clicked"));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("delete_prepared"));
    QVERIFY(!deletionAction->isEnabled()); deletionAccepted->setProperty("checked", true); deletionTyped->setProperty("text", QStringLiteral("wrong"));
    QVERIFY(!deletionAction->isEnabled()); deletionTyped->setProperty("text", QStringLiteral("qwen-audio-3.1-tts-flash-uitest-delete")); QTRY_VERIFY(deletionAction->isEnabled());
    auto *deletionScroll = window->findChild<QObject *>(QStringLiteral("cloudDeletionScroll")); QVERIFY(deletionScroll);
    auto *deletionFlickable = deletionScroll->property("contentItem").value<QObject *>(); QVERIFY(deletionFlickable);
    for (const auto &size : {QSize(900, 560), QSize(1366, 768), QSize(1920, 1080)}) {
        window->resize(size); QTest::qWait(100);
        deletionFlickable->setProperty("contentY", qMax(0., deletionFlickable->property("contentHeight").toDouble() - deletionFlickable->property("height").toDouble()));
        QTest::qWait(100);
        QVERIFY(deletionAction->mapToScene(QPointF{}).y() >= 0);
        QVERIFY(deletionAction->mapToScene(QPointF{}).y() + deletionAction->height() <= window->height());
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-cloud-delete-%1.png").arg(size.width()))));
    }
    QVERIFY(QMetaObject::invokeMethod(deletionDialog, "close")); QTRY_VERIFY(voices.cloudClone().isEmpty());
    window->resize(cloneWindowSize); QTest::qWait(100);
    deletionDialog->setProperty("updating", true); QVERIFY(QMetaObject::invokeMethod(deletionDialog, "open"));
    QVERIFY(!deletionAccepted->property("checked").toBool());
    auto *updatePrivacy = window->findChild<QObject *>(QStringLiteral("cloudUpdatePrivacy"));
    auto *updateCost = window->findChild<QObject *>(QStringLiteral("cloudUpdateCost")); QVERIFY(updatePrivacy && updateCost);
    QVERIFY(!updatePrivacy->property("checked").toBool()); QVERIFY(!updateCost->property("checked").toBool());
    window->findChild<QObject *>(QStringLiteral("cloudUpdateUrl"))->setProperty("text", QStringLiteral("https://reference.example/new.wav?signature=hidden"));
    QVERIFY(QMetaObject::invokeMethod(window->findChild<QObject *>(QStringLiteral("previewCloudDeletion")), "clicked"));
    QCOMPARE(voices.cloudClone().value(QStringLiteral("state")).toString(), QStringLiteral("update_prepared"));
    deletionTyped->setProperty("text", QStringLiteral("qwen-audio-3.1-tts-flash-uitest-delete")); deletionAccepted->setProperty("checked", true);
    QVERIFY(!deletionAction->isEnabled()); updatePrivacy->setProperty("checked", true); QVERIFY(!deletionAction->isEnabled()); updateCost->setProperty("checked", true); QTRY_VERIFY(deletionAction->isEnabled());
    for (const auto &size : {QSize(900, 560), QSize(1366, 768), QSize(1920, 1080)}) {
        window->resize(size); QTest::qWait(100);
        deletionFlickable->setProperty("contentY", qMax(0., deletionFlickable->property("contentHeight").toDouble() - deletionFlickable->property("height").toDouble()));
        QTest::qWait(100); QVERIFY(deletionAction->mapToScene(QPointF{}).y() >= 0);
        QVERIFY(deletionAction->mapToScene(QPointF{}).y() + deletionAction->height() <= window->height());
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-cloud-update-%1.png").arg(size.width()))));
    }
    QVERIFY(QMetaObject::invokeMethod(deletionDialog, "close")); QTRY_VERIFY(voices.cloudClone().isEmpty());
    window->resize(cloneWindowSize); QTest::qWait(100);
    voices.addLocalVersion(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), QStringLiteral("中文参考转写"));
    QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.versions().size(), 2);
    auto *reviewDialog = window->findChild<QObject *>(QStringLiteral("voiceReferenceReviewDialog"));
    QVERIFY(reviewDialog); QVERIFY(QMetaObject::invokeMethod(reviewDialog, "open"));
    voices.inspectVersion(voices.versions().first().toMap().value(QStringLiteral("id")).toString());
    QTRY_VERIFY(!voices.busy()); QVERIFY(!voices.review().isEmpty());
    auto *reviewText = window->findChild<QObject *>(QStringLiteral("referenceReviewTranscript"));
    auto *playReference = window->findChild<QObject *>(QStringLiteral("referencePreviewPlay"));
    QVERIFY(reviewText && playReference); QVERIFY(reviewText->setProperty("text", QStringLiteral("试听期间保留的审核草稿")));
    auto *referenceEmotion = window->findChild<QObject *>(QStringLiteral("voiceReferenceEmotion"));
    auto *derivedEmotion = window->findChild<QObject *>(QStringLiteral("voiceDerivedEmotion"));
    QVERIFY(referenceEmotion && derivedEmotion); QCOMPARE(referenceEmotion->property("count").toInt(), 10);
    auto *emotionEnabled = window->findChild<QObject *>(QStringLiteral("voiceEmotionEnabled"));
    auto *recognizeEmotion = window->findChild<QObject *>(QStringLiteral("recognizeReferenceEmotion"));
    QVERIFY(emotionEnabled && recognizeEmotion); QVERIFY(!emotionEnabled->property("checked").toBool()); QVERIFY(!recognizeEmotion->property("visible").toBool());
    QVERIFY(emotionEnabled->setProperty("checked", true)); QTRY_VERIFY(recognizeEmotion->property("visible").toBool());
    QCOMPARE(referenceEmotion->property("currentIndex").toInt(), 0); QCOMPARE(derivedEmotion->property("currentIndex").toInt(), 0);
    QVERIFY(referenceEmotion->setProperty("currentIndex", 4));
    QVERIFY(QMetaObject::invokeMethod(playReference, "clicked")); QTRY_VERIFY(preview.playing());
    QCOMPARE(reviewText->property("text").toString(), QStringLiteral("试听期间保留的审核草稿"));
    QCOMPARE(referenceEmotion->property("currentIndex").toInt(), 4);
    QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-reference-review.png"))));
    auto *emotionScroll = window->findChild<QObject *>(QStringLiteral("voiceEmotionReviewScroll")); QVERIFY(emotionScroll);
    auto *emotionFlickable = emotionScroll->property("contentItem").value<QObject *>(); QVERIFY(emotionFlickable);
    QVERIFY(emotionFlickable->property("contentHeight").toReal() > emotionFlickable->property("height").toReal());
    QVERIFY(emotionFlickable->setProperty("contentY", 300)); QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-emotion-controls.png"))));
    QVERIFY(QMetaObject::invokeMethod(reviewDialog, "close"));
    auto *sampleDialog = window->findChild<QObject *>(QStringLiteral("voiceSampleBatchDialog"));
    auto *samplesList = window->findChild<QObject *>(QStringLiteral("voicePreparedSamples"));
    QVERIFY(sampleDialog && samplesList); QVERIFY(QMetaObject::invokeMethod(sampleDialog, "open"));
    voices.prepareSamples({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav")))});
    QTRY_VERIFY(!voices.busy()); QVERIFY(!voices.samples().isEmpty());
    QTRY_COMPARE(samplesList->property("count").toInt(), voices.samples().size());
    auto *batchEmotion = window->findChild<QObject *>(QStringLiteral("voiceBatchEmotionEnabled"));
    auto *batchEmotionAction = window->findChild<QQuickItem *>(QStringLiteral("recognizeSampleEmotions"));
    auto *batchEmotionPython = window->findChild<QObject *>(QStringLiteral("voiceBatchEmotionPython"));
    auto *batchEmotionModel = window->findChild<QObject *>(QStringLiteral("voiceBatchEmotionModel"));
    QVERIFY(batchEmotion && batchEmotionAction && batchEmotionPython && batchEmotionModel);
    QVERIFY(!batchEmotion->property("checked").toBool()); QVERIFY(!batchEmotionAction->isVisible());
    QVERIFY(batchEmotion->setProperty("checked", true)); QTRY_VERIFY(batchEmotionAction->isVisible());
    QVERIFY(!batchEmotionAction->isEnabled());
    batchEmotionPython->setProperty("text", QStringLiteral("本地 Python 路径")); batchEmotionModel->setProperty("text", QStringLiteral("本地模型目录"));
    QTRY_VERIFY(batchEmotionAction->isEnabled());
    auto *batchScroll = window->findChild<QObject *>(QStringLiteral("voiceBatchReviewScroll")); QVERIFY(batchScroll);
    auto *batchFlickable = samplesList; QVERIFY(batchFlickable);
    for (const auto &size : {QSize(900, 560), QSize(1366, 768), QSize(1920, 1080)}) {
        window->resize(size); QTest::qWait(100);
        batchFlickable->setProperty("contentY", qMax(0., batchFlickable->property("contentHeight").toDouble() - batchFlickable->property("height").toDouble()));
        QTest::qWait(100);
        QVERIFY(batchEmotionAction->mapToScene(QPointF{}).y() >= 0);
        QVERIFY(batchEmotionAction->mapToScene(QPointF{}).y() + batchEmotionAction->height() <= window->height());
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-batch-%1.png").arg(size.width()))));
    }
    QTest::qWait(100); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-reference-batch.png"))));
    QVERIFY(QMetaObject::invokeMethod(sampleDialog, "close"));
    auto *addVoice = window->findChild<QObject *>(QStringLiteral("addVoiceButton")); QVERIFY(addVoice);
    QVERIFY(QMetaObject::invokeMethod(addVoice, "clicked"));
    auto *addPage = window->findChild<QQuickItem *>(QStringLiteral("voiceAddPage")); QVERIFY(addPage); QTRY_VERIFY(addPage->isVisible());
    auto *trainingButton = window->findChild<QObject *>(QStringLiteral("addTrainedVoice")); QVERIFY(trainingButton);
    QVERIFY(QMetaObject::invokeMethod(trainingButton, "clicked"));
    auto *unavailable = window->findChild<QQuickItem *>(QStringLiteral("unavailableVoicePage")); QVERIFY(unavailable); QTRY_VERIFY(unavailable->isVisible());
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-training-unavailable.png"))));
    QVERIFY(QMetaObject::invokeMethod(unavailable, "close"));
    QVERIFY(QMetaObject::invokeMethod(addVoice, "clicked"));
    context->settings.insert(QStringLiteral("asrProvider"), QStringLiteral("dashscope")); // 此 UI 回归不启动真实本地模型。
    auto *referenceButton = window->findChild<QObject *>(QStringLiteral("addReferenceVoice")); QVERIFY(referenceButton);
    QVERIFY(QMetaObject::invokeMethod(referenceButton, "clicked"));
    auto *collectPage = window->findChild<QQuickItem *>(QStringLiteral("creationCollectPage")); QVERIFY(collectPage); QTRY_VERIFY(collectPage->isVisible());
    // 准备 15 秒非静音素材，满足「10 秒起可分析」；长素材按静音或时长切开。
    const auto longAudioPath = directory.filePath(QStringLiteral("creation-15s.wav"));
    {
        QFile longAudio(longAudioPath); QVERIFY(longAudio.open(QIODevice::WriteOnly));
        QDataStream wav(&longAudio); wav.setByteOrder(QDataStream::LittleEndian);
        wav.writeRawData("RIFF", 4); wav << quint32(36 + 16000 * 15 * 2);
        wav.writeRawData("WAVEfmt ", 8); wav << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000) << quint16(2) << quint16(16);
        wav.writeRawData("data", 4); wav << quint32(16000 * 15 * 2);
        for (int i = 0; i < 16000 * 15; ++i) wav << static_cast<qint16>(6000.0 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * i / 16000.0));
    }
    voices.prepareSamples({QUrl::fromLocalFile(longAudioPath)}, true); QTRY_VERIFY(!voices.busy());
    QVERIFY(!voices.samples().isEmpty());
    QVERIFY(voices.creationDraft().value(QStringLiteral("usableSeconds")).toDouble() >= 10);
    QTest::qWait(50);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-create-collect.png"))));
    auto *analyzeButton = window->findChild<QQuickItem *>(QStringLiteral("creationAnalyzeButton")); QVERIFY(analyzeButton);
    QTRY_VERIFY(analyzeButton->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(analyzeButton, "clicked"));
    auto *analyzePage = window->findChild<QQuickItem *>(QStringLiteral("creationAnalyzePage")); QVERIFY(analyzePage); QTRY_VERIFY(analyzePage->isVisible());
    QTRY_VERIFY(voices.creationDraft().value(QStringLiteral("analyzed")).toBool());
    QTest::qWait(50);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-create-analyze.png"))));
    auto *continueButton = window->findChild<QQuickItem *>(QStringLiteral("creationContinueButton")); QVERIFY(continueButton);
    QTRY_VERIFY(continueButton->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(continueButton, "clicked"));
    auto *profilePage = window->findChild<QQuickItem *>(QStringLiteral("creationProfilePage")); QVERIFY(profilePage); QTRY_VERIFY(profilePage->isVisible());
    auto *creationName = window->findChild<QObject *>(QStringLiteral("creationVoiceName")); QVERIFY(creationName); creationName->setProperty("text", QStringLiteral("我的参考音色"));
    window->setMinimumHeight(440);
    for (const auto &size : {QSize(900, 440), QSize(1366, 768), QSize(1920, 1080)}) {
        window->resize(size); QTest::qWait(100);
        auto *createButton = window->findChild<QQuickItem *>(QStringLiteral("createReferenceVoice")); QVERIFY(createButton);
        const auto bounds = createButton->mapRectToScene(createButton->boundingRect());
        QVERIFY(bounds.left() >= 0 && bounds.right() <= window->width()); QVERIFY(bounds.top() >= 0 && bounds.bottom() <= window->height());
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-create-%1.png").arg(size.width()))));
    }
    const auto originalDefaultVoice = voiceDocument.defaultVoiceVersionId();
    std::function<QQuickItem *(QQuickItem *, const QString &)> creationItem = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems()) if (auto *found = creationItem(child, name)) return found;
        return nullptr;
    };
    auto *createVoiceButton = window->findChild<QObject *>(QStringLiteral("createReferenceVoice")); QVERIFY(createVoiceButton);
    QTRY_VERIFY(!createVoiceButton->property("enabled").toBool());
    // 没有转写时创建按钮直接禁用，并在页脚说明缺什么；不再点完才冒出一行灰字。
    auto *blockReason = window->findChild<QObject *>(QStringLiteral("creationBlockReason")); QVERIFY(blockReason);
    QTRY_VERIFY(blockReason->property("visible").toBool());
    QVERIFY(blockReason->property("text").toString().contains(QStringLiteral("转写")));
    auto *creationLanguage = creationItem(window->contentItem(), QStringLiteral("creationReferenceLanguage")); QVERIFY(creationLanguage);
    QCOMPARE(creationLanguage->property("currentIndex").toInt(), 0);
    creationLanguage->setProperty("currentIndex", 1); QVERIFY(QMetaObject::invokeMethod(creationLanguage, "activated", Q_ARG(int, 1)));
    QTest::qWait(50);
    const auto promptIndex = voices.creationDraft().value(QStringLiteral("promptIndex")).toInt();
    QVERIFY(promptIndex >= 0);
    QCOMPARE(voices.samples().value(promptIndex).toMap().value(QStringLiteral("referenceLanguage")).toString(), QStringLiteral("English"));
    auto *creationText = creationItem(window->contentItem(), QStringLiteral("creationTranscript")); QVERIFY(creationText);
    creationText->setProperty("text", QStringLiteral("已试听并核对的参考原话"));
    QTRY_VERIFY(createVoiceButton->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(createVoiceButton, "clicked")); QTRY_VERIFY(!voices.busy());
    QVERIFY2(!voices.creating(), qPrintable(voices.statusText())); QTRY_VERIFY(!profilePage->isVisible()); QVERIFY(voices.samples().isEmpty());
    QCOMPARE(voiceDocument.defaultVoiceVersionId(), originalDefaultVoice);
    QVERIFY(QMetaObject::invokeMethod(voiceDialog, "close"));
    QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/shared-project-workspace.png"))));
    window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
    QTest::keyClick(window, Qt::Key_3, Qt::ControlModifier);
    QTRY_COMPARE(router.workspace(), QStringLiteral("voice"));
    voiceDocument.previewText(QStringLiteral("今天是2026-10-13，温度20℃。\n记录长文旁白。")); voiceDocument.applyPreview();
    auto *voiceWorkspace = window->findChild<QObject *>(QStringLiteral("voiceWorkspace"));
    auto *bodyField = window->findChild<QObject *>(QStringLiteral("voiceBodyEditor"));
    QVERIFY(voiceWorkspace && bodyField); QTRY_VERIFY(bodyField->property("enabled").toBool());
    QVERIFY(window->findChild<QObject *>(QStringLiteral("globalSettingsButton")));
    auto *generateButton = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("voiceGenerateButton"));
    auto *generateMissing = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("voiceGenerateMissingButton"));
    QVERIFY(generateButton && generateMissing);
    auto *inlineCandidates = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("voiceInlineCandidates")); QVERIFY(inlineCandidates);
    voiceWorkspace->setProperty("candidatesExpanded", true); QTest::qWait(80);
    QVERIFY(inlineCandidates->isVisible()); QVERIFY(inlineCandidates->parentItem());
    QVERIFY(inlineCandidates->parentItem()->mapToScene(QPointF{}).x() < window->width() * 0.7);
    voiceWorkspace->setProperty("candidatesExpanded", false);

    QVERIFY(generateButton->isVisible()); QVERIFY(generateMissing->isVisible());
    auto *wavOptions = voiceWorkspace->findChild<QObject *>(QStringLiteral("voiceWavOptions")); QVERIFY(wavOptions);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "requestExport"));
    QTRY_VERIFY(wavOptions->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(wavOptions, "close"));
    auto *changesDialog = window->findChild<QObject *>(QStringLiteral("voiceChangesDialog")); if (changesDialog) QMetaObject::invokeMethod(changesDialog, "close");
    QVERIFY(bodyField->setProperty("text", QStringLiteral("保存前的中文草稿")));
    QVariant committed; QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committed))); QVERIFY(committed.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("text")).toString(), QStringLiteral("保存前的中文草稿"));
    auto *bodyItem = qobject_cast<QQuickItem *>(bodyField); QVERIFY(bodyItem); bodyItem->forceActiveFocus(Qt::OtherFocusReason);
    QInputMethodEvent composing(QStringLiteral("尚未确认的拼音"), {}); QCoreApplication::sendEvent(bodyItem, &composing);
    QVERIFY(bodyField->property("inputMethodComposing").toBool());
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committed))); QVERIFY(!committed.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("text")).toString(), QStringLiteral("保存前的中文草稿"));
    // 后台候选通知不能重置正在输入的草稿或输入法。
    QVERIFY(bodyField->setProperty("text", QStringLiteral("保持我的输入草稿"))); emit voiceDocument.candidatesChanged();
    QCOMPARE(bodyField->property("text").toString(), QStringLiteral("保持我的输入草稿"));
    QInputMethodEvent completed; QCoreApplication::sendEvent(bodyItem, &completed);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committed))); QVERIFY(committed.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("text")).toString(), QStringLiteral("保持我的输入草稿"));
    const auto source = mediaPath(QStringLiteral("audio.wav")); const auto quality = AudioQuality::inspect(source); QVERIFY(quality); auto audioDocument = voiceDocument.snapshot();
    for (const auto &value : audioDocument.value(QStringLiteral("segments")).toArray()) { const auto item = value.toObject(); const auto job = QUuid::createUuid().toString(), take = QUuid::createUuid().toString();
        QVERIFY(ProjectStore(session.path()).enqueue(job, {{QStringLiteral("segmentId"), item.value(QStringLiteral("id"))}})); QVERIFY(ProjectStore(session.path()).finish(job, take, source)); QString problem;
        audioDocument = VoiceDocument::applyTake(audioDocument, item.value(QStringLiteral("id")).toString(), {{QStringLiteral("id"), take}, {QStringLiteral("sampleRate"), quality->sampleRate}, {QStringLiteral("sampleCount"), QString::number(quality->sampleCount)},
            {QStringLiteral("sha256"), QString::fromLatin1(quality->sha256.toHex())}, {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(audioDocument, item).toHex())}}, {}, &problem); QVERIFY2(problem.isEmpty(), qPrintable(problem)); }
    voiceDocument.reset(audioDocument); voiceAudio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!voiceAudio.busy(), 10000); QTRY_VERIFY(preview.playing()); QCOMPARE(voiceAudio.rows().size(), 2);
    std::function<QQuickItem *(QQuickItem *)> findWaveform = [&](QQuickItem *item) -> QQuickItem * { if (item->objectName() == QStringLiteral("voiceWaveform")) return item; for (auto *child : item->childItems()) if (auto *found = findWaveform(child)) return found; return nullptr; };
    QTest::qWait(100); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-before-waveform.png"))));
    QQuickItem *waveform = nullptr; QTRY_VERIFY2((waveform = findWaveform(window->contentItem())) != nullptr, qPrintable(warnings.join(u'\n'))); QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, waveform->mapToScene(QPointF(waveform->width() / 2, waveform->height() / 2)).toPoint());
    QTest::qWait(100); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-waveform.png")))); voiceAudio.stop();
    window->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(window));
    QTRY_VERIFY(waveform->width() > 0 && waveform->height() > 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::ShiftModifier, waveform->mapToScene(QPointF(waveform->width() * 0.1, waveform->height() / 2)).toPoint());
    QTest::mouseMove(window, waveform->mapToScene(QPointF(waveform->width() * 0.8, waveform->height() / 2)).toPoint());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::ShiftModifier, waveform->mapToScene(QPointF(waveform->width() * 0.8, waveform->height() / 2)).toPoint());
    QTRY_VERIFY(voiceDocument.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("manualCrop")).toBool()); QVERIFY(voiceAudio.rows().isEmpty()); QCOMPARE(voiceAudio.waveformBytes(), qint64(0));
    QTest::qWait(100); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-workspace.png"))));
    window->setMinimumSize(QSize{860, 400});
    for (const QSize size : {QSize{900, 440}, QSize{911, 512}, QSize{960, 540}, QSize{1092, 614}, QSize{1100, 700}, QSize{1280, 720}, QSize{1366, 768}, QSize{1440, 900}, QSize{1920, 1080}}) {
        const auto width = size.width();
        window->resize(size); QTest::qWait(80);
        qInfo() << "voice_client=" << window->size() << "requested=" << size << "dpr=" << window->devicePixelRatio();
        for (auto *button : {generateButton, generateMissing}) {
            const auto bounds = button->mapRectToScene(button->boundingRect());
            QVERIFY(bounds.left() >= 0 && bounds.right() <= window->width());
            QVERIFY(bounds.top() >= 0 && bounds.bottom() <= window->height());
        }
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-layout-%1.png").arg(width))));
        if (width == 911) {
            auto *panel = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("voiceInspectorPanel")); QVERIFY(panel);
            QVERIFY(panel->isVisible());
            const auto bounds = panel->mapRectToScene(panel->boundingRect());
            QVERIFY(bounds.left() >= 0 && bounds.right() <= window->width() + 1);
        }
    }
    for (auto *item : voiceWorkspace->findChildren<QQuickItem *>()) {
        if (item->inherits("QQuickFlickable") && item->height() > 300 && item->mapToScene(QPointF{}).x() > window->width() * 0.65) {
            item->setProperty("contentY", std::max(0.0, item->property("contentHeight").toDouble() - item->height()));
        }
    }
    QTest::qWait(50); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-qwen-controls.png"))));
    auto *engineSelector = voiceWorkspace->findChild<QObject *>(QStringLiteral("voiceEngineSelector")); QVERIFY(engineSelector); engineSelector->setProperty("currentIndex", 1);
    QTest::qWait(50);
    for (auto *item : voiceWorkspace->findChildren<QQuickItem *>()) if (item->inherits("QQuickFlickable") && item->height() > 300 && item->mapToScene(QPointF{}).x() > window->width() * 0.65) item->setProperty("contentY", std::max(0.0, item->property("contentHeight").toDouble() - item->height()));
    QTest::qWait(50); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-flash-controls.png"))));
    auto *flashDialog = voiceWorkspace->findChild<QObject *>(QStringLiteral("voiceFlashDialog")); QVERIFY(flashDialog);
    const QSize flashWindowSize = window->size();
    for (const auto &size : {QSize(900, 560), QSize(1366, 768)}) {
        window->resize(size); QVERIFY(QMetaObject::invokeMethod(flashDialog, "open")); QTest::qWait(100);
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-flash-confirm-%1.png").arg(size.width()))));
        QVERIFY(QMetaObject::invokeMethod(flashDialog, "reject"));
    }
    window->resize(flashWindowSize); QTest::qWait(100);
    QCOMPARE(engineSelector->property("count").toInt(), 4); engineSelector->setProperty("currentIndex", 2);
    auto *duration = voiceWorkspace->findChild<QObject *>(QStringLiteral("indexDurationField"));
    auto *emotion = voiceWorkspace->findChild<QObject *>(QStringLiteral("indexEmotionChoice")); QVERIFY(duration); QVERIFY(emotion);
    QCOMPARE(emotion->property("count").toInt(), 9); duration->setProperty("text", QStringLiteral("0.49")); QVERIFY(!duration->property("acceptableInput").toBool());
    duration->setProperty("text", QStringLiteral("1.15")); QVERIFY(duration->property("acceptableInput").toBool());
    emotion->setProperty("currentIndex", 1);
    for (auto *item : voiceWorkspace->findChildren<QQuickItem *>()) if (item->inherits("QQuickFlickable") && item->height() > 300 && item->mapToScene(QPointF{}).x() > window->width() * 0.65) item->setProperty("contentY", 0);
    QTest::qWait(50); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-index-controls.png"))));
    voiceWorkspace->setProperty("draftDirty", true); voiceWorkspace->setProperty("indexParametersDirty", true);
    engineSelector->setProperty("currentIndex", 3); QTest::qWait(50);
    QVariant committedDraft; QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("durationFactor")).toDouble(), 1.15);
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("emotion")).toString(), QStringLiteral("happy"));
    QVERIFY(!voiceWorkspace->property("indexParametersDirty").toBool());
    auto *gptSpeed = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("gptSpeedField")); QVERIFY(gptSpeed); QVERIFY(gptSpeed->isVisible());
    gptSpeed->setProperty("text", QStringLiteral("0.49")); voiceWorkspace->setProperty("draftDirty", true); voiceWorkspace->setProperty("gptParametersDirty", true);
    const auto beforeSpeed = voiceDocument.snapshot();
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(!committedDraft.toBool()); QCOMPARE(voiceDocument.snapshot(), beforeSpeed);
    gptSpeed->setProperty("text", QStringLiteral("0.85"));
    QVariant speedDraft; QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "recoveryFields", Q_RETURN_ARG(QVariant, speedDraft)));
    QCOMPARE(speedDraft.toMap().value(QStringLiteral("speed")).toString(), QStringLiteral("0.85")); QVERIFY(speedDraft.toMap().value(QStringLiteral("gptDirty")).toBool());
    gptSpeed->setProperty("text", QStringLiteral("1")); QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "restoreRecoveryFields", Q_ARG(QVariant, speedDraft)));
    QCOMPARE(gptSpeed->property("text").toString(), QStringLiteral("0.85")); QVERIFY(voiceWorkspace->property("gptParametersDirty").toBool());
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("speedFactor")).toDouble(), 0.85);
    QVERIFY(!voiceWorkspace->property("gptParametersDirty").toBool());
    voiceDocument.undo(); QVERIFY(!voiceDocument.selected().value(QStringLiteral("parameters")).toMap().contains(QStringLiteral("speedFactor"))); voiceDocument.redo();
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("speedFactor")).toDouble(), 0.85);
    const QSize speedWindowSize = window->size();
    for (const auto &size : {QSize(900, 560), QSize(1366, 768)}) {
        window->resize(size); QTest::qWait(100); voiceWorkspace->setProperty("inspectorCollapsed", false); QTest::qWait(150);
        for (auto *item : voiceWorkspace->findChildren<QQuickItem *>()) if (item->inherits("QQuickFlickable") && item->isAncestorOf(gptSpeed))
            item->setProperty("contentY", std::max(0.0, item->property("contentY").toDouble() + gptSpeed->mapToScene(QPointF{}).y() - item->mapToScene(QPointF{}).y() - 50));
        QTest::qWait(100); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-gpt-speed-%1.png").arg(size.width()))));
    }
    window->resize(speedWindowSize); QTest::qWait(100);
    engineSelector->setProperty("currentIndex", 1); QTest::qWait(50); QVERIFY(gptSpeed->isVisible());
    auto *seedInput = voiceWorkspace->findChild<QQuickItem *>(QStringLiteral("voiceSeedField")); QVERIFY(seedInput);
    const auto beforeCloudSeed = voiceDocument.snapshot();
    for (const auto &seed : {QStringLiteral("-1"), QStringLiteral("65536")}) {
        seedInput->setProperty("text", seed); voiceWorkspace->setProperty("draftDirty", true);
        QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(!committedDraft.toBool());
        QCOMPARE(voiceDocument.snapshot(), beforeCloudSeed);
    }
    for (const int seed : {0, 65535}) {
        seedInput->setProperty("text", QString::number(seed)); voiceWorkspace->setProperty("draftDirty", true);
        QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
        QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("seed")).toInt(), seed);
    }
    seedInput->setProperty("text", QString()); voiceWorkspace->setProperty("draftDirty", true);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QVERIFY(!voiceDocument.selected().value(QStringLiteral("parameters")).toMap().contains(QStringLiteral("seed")));
    gptSpeed->setProperty("text", QStringLiteral("0.75")); voiceWorkspace->setProperty("draftDirty", true); voiceWorkspace->setProperty("gptParametersDirty", true);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("speedFactor")).toDouble(), 0.75);
    voiceDocument.undo(); QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("speedFactor")).toDouble(), 0.85); voiceDocument.redo();
    gptSpeed->setProperty("text", QStringLiteral("1")); voiceWorkspace->setProperty("draftDirty", true); voiceWorkspace->setProperty("gptParametersDirty", true);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QVERIFY(!voiceDocument.selected().value(QStringLiteral("parameters")).toMap().contains(QStringLiteral("speedFactor"))); voiceDocument.undo();
    QTest::qWait(50); QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-cloud-speed.png"))));
    engineSelector->setProperty("currentIndex", 3); QTest::qWait(50);
    seedInput->setProperty("text", QStringLiteral("2147483647")); voiceWorkspace->setProperty("draftDirty", true);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("parameters")).toMap().value(QStringLiteral("seed")).toLongLong(), 2147483647LL);
    seedInput->setProperty("text", QString()); voiceWorkspace->setProperty("draftDirty", true);
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    auto *languageChoice = voiceWorkspace->findChild<QObject *>(QStringLiteral("voiceTargetLanguage")); QVERIFY(languageChoice);
    // GPT-SoVITS 的参考语言和目标语言已分开，目标语言支持中英日。
    QCOMPARE(languageChoice->property("count").toInt(), 3);
    engineSelector->setProperty("currentIndex", 0); QTest::qWait(30);
    QCOMPARE(languageChoice->property("count").toInt(), 3); languageChoice->setProperty("currentIndex", 1); QVERIFY(QMetaObject::invokeMethod(languageChoice, "activated", Q_ARG(int, 1)));
    QVERIFY(QMetaObject::invokeMethod(voiceWorkspace, "commitPending", Q_RETURN_ARG(QVariant, committedDraft))); QVERIFY(committedDraft.toBool());
    QCOMPARE(voiceDocument.selected().value(QStringLiteral("language")).toString(), QStringLiteral("English"));
    QVERIFY(voiceDocument.selected().value(QStringLiteral("selectedTake")).toMap().value(QStringLiteral("manualCrop")).toBool());
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-cross-language-controls.png"))));
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/voice-gpt-controls.png"))));
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    cancelGpu = true;
    waiting.join();
    batch.reset();
    QTRY_VERIFY(!gpuButton->property("visible").toBool());
    if (!qEnvironmentVariable("SUBCUE_P8_UI_REPORT").isEmpty()) {
        QString longText; for (int i = 0; i < 500; ++i) longText += QStringLiteral("这是中文长文旁白压力测试保留每句独立编辑。\n");
        voiceDocument.reset(VoiceDocument::appendText(VoiceDocument::empty(), longText));
        window->contentItem()->forceActiveFocus(); QTest::qWait(100);
        const auto segments = voiceDocument.snapshot().value(QStringLiteral("segments")).toArray(); QVector<qint64> frames;
        QElapsedTimer response;
        for (int i = 0; i < 50; ++i) {
            response.start(); voiceDocument.select(segments[i * 10].toObject().value(QStringLiteral("id")).toString());
            QCoreApplication::processEvents(); window->update(); QVERIFY(!window->grabWindow().isNull()); frames.append(response.nsecsElapsed());
        }
        std::sort(frames.begin(), frames.end());
        QVector<qint64> switching, settings;
        for (int i = 0; i < 50; ++i) {
            response.restart();
            QVERIFY(router.switchTo(i % 3 == 0 ? QStringLiteral("subtitle") : i % 3 == 1 ? QStringLiteral("roughcut") : QStringLiteral("voice")));
            QCoreApplication::processEvents(); switching.append(response.nsecsElapsed());
            response.restart(); QVERIFY(QMetaObject::invokeMethod(window, "openSettings", Q_ARG(QVariant, QStringLiteral("asr")), Q_ARG(QVariant, QString())));
            auto *host = window->property("settingsWindow").value<QObject *>(); QVERIFY(host);
            auto *loader = host->findChild<QObject *>(QStringLiteral("settingsContentLoader")); QVERIFY(loader);
            while (loader->property("status").toInt() != 1 && response.elapsed() < 5000) QTest::qWait(1);
            QCOMPARE(loader->property("status").toInt(), 1);
            QCoreApplication::processEvents(); settings.append(response.nsecsElapsed());
            QVERIFY(QMetaObject::invokeMethod(host, "hide")); QCoreApplication::processEvents();
        }
        std::sort(switching.begin(), switching.end()); std::sort(settings.begin(), settings.end());
        QFile report(qEnvironmentVariable("SUBCUE_P8_UI_REPORT")); QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(QJsonObject{{QStringLiteral("segments"), 500}, {QStringLiteral("samples"), frames.size()},
            {QStringLiteral("selectionAndFrameReadbackP95Ms"), double(frames[47]) / 1000000.0},
            {QStringLiteral("moduleSwitchP95Ms"), double(switching[47]) / 1000000.0}, {QStringLiteral("settingsReadyP95Ms"), double(settings[47]) / 1000000.0},
            {QStringLiteral("devicePixelRatio"), window->devicePixelRatio()}, {QStringLiteral("platform"), QGuiApplication::platformName()}}).toJson());
        QVERIFY(router.switchTo(QStringLiteral("voice"))); QCoreApplication::processEvents();
        QVERIFY(window->grabWindow().save(QDir(QFileInfo(qEnvironmentVariable("SUBCUE_P8_UI_REPORT")).absolutePath()).filePath(QStringLiteral("long-document-ui.png"))));
    }
    editor.setScriptText(QStringLiteral("关闭时放弃的字幕文稿"));
    roughCut.setScriptText(QStringLiteral("关闭时放弃的粗剪文稿"));
    voiceDocument.previewText(QStringLiteral("关闭时放弃的配音文稿。")); voiceDocument.applyPreview();
    QVERIFY(editor.modified() && roughCut.modified() && voiceDocument.modified());
    auto *unsaved = window->findChild<QObject *>(QStringLiteral("roughCutUnsavedDialog"));
    QVERIFY(unsaved);
    QVERIFY(!window->close());
    QTRY_VERIFY(unsaved->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(unsaved, "reject"));
    QTRY_VERIFY(!unsaved->property("visible").toBool());
    QVERIFY(window->isVisible()); QVERIFY(!window->property("allowClose").toBool());
    QVERIFY(!window->close());
    // 点击真正的标准按钮，覆盖 discarded 信号发出后 Qt 才关闭旧弹窗的顺序。
    for (const auto &workspace : {QStringLiteral("subtitle"), QStringLiteral("roughcut"), QStringLiteral("voice")}) {
        QTRY_COMPARE(unsaved->property("workspace").toString(), workspace);
        QTRY_VERIFY(unsaved->property("visible").toBool());
        auto standardButton = engine.newQObject(unsaved).property(QStringLiteral("standardButton"));
        auto *discard = qobject_cast<QQuickItem *>(standardButton.call({QJSValue(theme->property("discardButton").toInt())}).toQObject());
        QVERIFY(discard); QTRY_VERIFY(discard->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, discard->mapToScene(discard->boundingRect().center()).toPoint());
    }
    QTRY_VERIFY(!window->isVisible());
    QVERIFY(window->property("allowClose").toBool());
    QVERIFY(session.recoverableProjects().isEmpty());
}

void EditorIntegrationTests::qmlCloseWhileSavingKeepsSavedState()
{
    QTemporaryDir directory;
    const QString modulePath = directory.filePath(QStringLiteral("SubCue"));
    QVERIFY(QDir().mkpath(modulePath));
    QByteArray manifest("module SubCue\n");
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml"))).entryList({QStringLiteral("*.qml")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/") + file), QDir(modulePath).filePath(file)));
        if (file == QStringLiteral("Theme.qml")) manifest += "singleton ";
        manifest += QFileInfo(file).baseName().toUtf8() + " 1.0 " + file.toUtf8() + "\n";
    }
    QVERIFY(QDir().mkpath(QDir(modulePath).filePath(QStringLiteral("icons"))));
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList(QDir::Files))
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file), QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly)); qmldir.write(manifest); qmldir.close();
    registerSubCueQmlItems();
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    auto context = makeContext(directory);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    WorkspaceRouter router(&editor, &roughCut);
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("close-voices.sqlite")));
    VoiceStudioController voiceDocument(context.get(), &session);
    AudioPreviewController preview(context.get(), &session); router.setAudioPreview(&preview);
    VoiceAudioController voiceAudio(context.get(), &session, &voiceDocument, &preview);
    TtsTaskController taskRecords(context.get(), &session);
    QQmlApplicationEngine engine;
    engine.addImportPath(directory.path());
    QQmlComponent nativeTheme(&engine);
    nativeTheme.setData("import QtQuick; import QtQuick.Controls; QtObject { property bool expandedClientArea: false; property int discardButton: Dialog.Discard; function applyDarkTitleBar() {} }", QUrl());
    QObject *theme = nativeTheme.create();
    QVERIFY(theme); theme->setParent(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &editor);
    engine.rootContext()->setContextProperty(QStringLiteral("roughCut"), &roughCut);
    engine.rootContext()->setContextProperty(QStringLiteral("appRouter"), &router);
    engine.rootContext()->setContextProperty(QStringLiteral("projectSession"), &session);
    engine.rootContext()->setContextProperty(QStringLiteral("ttsTasks"), &taskRecords);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceLibrary"), &voices);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceStudio"), &voiceDocument);
    engine.rootContext()->setContextProperty(QStringLiteral("audioPreview"), &preview);
    engine.rootContext()->setContextProperty(QStringLiteral("voiceAudio"), &voiceAudio);
    engine.rootContext()->setContextProperty(QStringLiteral("gpuQueue"), &InferenceManager::instance());
    QStringList warnings;
    connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(QDir(modulePath).filePath(QStringLiteral("AppRouter.qml"))));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(u'\n')));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    window->setScreen(QGuiApplication::primaryScreen());
    window->setPosition(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(24, 24));
    QTRY_COMPARE(window->devicePixelRatio(), QGuiApplication::primaryScreen()->devicePixelRatio());
    const QString path = directory.filePath(QStringLiteral("关闭窗口保存.subcue-project"));
    QSignalSpy saved(&session, &ProjectSession::saved);
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    editor.setScriptText(QStringLiteral("保存后立刻关闭窗口"));
    QVERIFY(window->title().endsWith(QStringLiteral(" *")));
    // 阻塞存储队列，模拟大工程保存期间用户点关闭：不能在关闭时丢弃这次保存。
    QSemaphore started, release;
    auto blocker = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 20000); });
    const bool blocked = started.tryAcquire(1, 5'000);
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    QTest::qWait(200);
    QVERIFY(session.busy());
    QVERIFY(!window->close());
    QVERIFY(window->property("closeAfterStorage").toBool());
    QVERIFY(window->isVisible());
    release.release();
    blocker.waitForFinished();
    QVERIFY(blocked);
    QTRY_COMPARE(saved.count(), 2);
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(!session.modified());
    QTRY_VERIFY(!window->isVisible());
    QVERIFY(window->property("allowClose").toBool());
}

void EditorIntegrationTests::voiceCapabilityDrivenReferenceAndLanguage()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough); session.newProject();
    VoiceLibraryController voices(context.get(), &session); voices.createProfile(QStringLiteral("免转写参考")); QTRY_VERIFY(!voices.busy());
    voices.addLocalVersion(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), {}, false); QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.versions().size(), 1);
    const auto original = voices.versions().first().toMap().value(QStringLiteral("id")).toString(); voices.inspectVersion(original); QTRY_VERIFY(!voices.busy());
    voices.approveReview({}, true, QStringLiteral("Chinese"), {}, false, false); QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.versions().size(), 2);
    const auto metadata = voices.versions().first().toMap().value(QStringLiteral("metadata")).toMap(); QCOMPARE(metadata.value(QStringLiteral("qualityState")).toString(), QStringLiteral("approved")); QVERIFY(metadata.value(QStringLiteral("referenceText")).toString().isEmpty());
    {
        VoiceStudioController voice(context.get(), &session); voice.previewText(QStringLiteral("Hello world."), QStringLiteral("qwen"));
        QCOMPARE(voice.preview().first().toMap().value(QStringLiteral("after")).toMap().value(QStringLiteral("language")).toString(), QStringLiteral("English")); voice.applyPreview();
        voice.previewText(QStringLiteral("中文 and English"), QStringLiteral("qwen")); QVERIFY(voice.preview().first().toMap().value(QStringLiteral("kind")).toString().contains(QStringLiteral("混合"))); voice.discardPreview();
        voice.persistSessionState({{QStringLiteral("engine"), 3}, {QStringLiteral("favorites"), QStringList{QStringLiteral("scoped/voice")}}});
        auto document = VoiceDocument::setDefaultVoice(voice.snapshot(), QUuid::createUuid().toString(QUuid::WithoutBraces)); auto items = document.value(QStringLiteral("segments")).toArray(); auto row = items.first().toObject(); row.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("seed"), 7}}); items[0] = row; document.insert(QStringLiteral("segments"), items);
        const auto appended = VoiceDocument::appendText(document, QStringLiteral("Inherited sentence.")); QCOMPARE(appended.value(QStringLiteral("segments")).toArray().last().toObject().value(QStringLiteral("parameters")).toObject().value(QStringLiteral("seed")).toInt(), 7);
    }
    VoiceStudioController restored(context.get(), &session); QCOMPARE(restored.sessionState().value(QStringLiteral("engine")).toInt(), 3);
}
void EditorIntegrationTests::voiceMultiSelectionLockingAndRecovery()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("第一句。第二句。\n第三句。")); voice.applyPreview(); QCOMPARE(voice.rowCount(), 3);

    context->submitProjectStorage([] {}).waitForFinished();
    const auto version = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(ProjectStore(session.storagePath()).importVoiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces), version, {}, {}, {{QStringLiteral("provider"), QStringLiteral("mock")}}));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    const auto first = voice.data(voice.index(0), VoiceStudioController::Id).toString(), last = voice.data(voice.index(2), VoiceStudioController::Id).toString();
    voice.select(first); voice.selectMany(last, false, true); QCOMPARE(voice.selectedIds().size(), 3);
    voice.editSelection({{QStringLiteral("gain"), 0.8}, {QStringLiteral("pauseMs"), 250}});
    for (int i = 0; i < 3; ++i) QCOMPARE(voice.data(voice.index(i), VoiceStudioController::Gain).toDouble(), 0.8);
    voice.select(first); voice.setLocked(true); QVERIFY(voice.selected().value(QStringLiteral("locked")).toBool());
    const auto locked = voice.snapshot(); voice.editSegment(first, QStringLiteral("不能修改"), {}, false, {}); QCOMPARE(voice.snapshot(), locked);
    voice.previewDelete(); voice.applyPreview(); QCOMPARE(voice.snapshot(), locked);
    voice.selectMany(last, false, true); voice.editSelection({{QStringLiteral("gain"), 0.5}});
    QCOMPARE(voice.data(voice.index(0), VoiceStudioController::Gain).toDouble(), 0.8); QCOMPARE(voice.data(voice.index(2), VoiceStudioController::Gain).toDouble(), 0.5);
    QCOMPARE(voice.selectionProperties().value(QStringLiteral("locked")).toInt(), 1);
    voice.setSessionState({{QStringLiteral("engine"), 3}, {QStringLiteral("voicesWidth"), 220}});
    const auto frozen = voice.snapshot(); voice.reset(frozen); QVERIFY(voice.data(voice.index(0), VoiceStudioController::Locked).toBool()); QCOMPARE(voice.sessionState().value(QStringLiteral("engine")).toInt(), 3);
    voice.select(first); voice.setLocked(false); QVERIFY(!voice.selected().value(QStringLiteral("locked")).toBool()); voice.undo(); QVERIFY(voice.selected().value(QStringLiteral("locked")).toBool());
    const auto python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe"); if (!QFileInfo::exists(python)) QSKIP("独立测试 Python 不可用");
    TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks); voice.selectMany(last, false, true);
    QSignalSpy generated(&tasks, &TtsTaskController::finished); voice.generateMock(python, QStringLiteral("selection")); QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 10000); QTRY_VERIFY(!voice.busy());
    QCOMPARE(ProjectStore(session.storagePath()).jobs().size(), 2); QVERIFY(voice.data(voice.index(0), VoiceStudioController::Locked).toBool());
    voice.reset(voice.snapshot()); voice.loadCandidates(); QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.data(voice.index(1), VoiceStudioController::GenerationState).toString(), QStringLiteral("candidate"));
    voice.editSegment(last, QStringLiteral("修改最后一句。"), {}, false, {});
    QCOMPARE(voice.data(voice.index(2), VoiceStudioController::GenerationState).toString(), QStringLiteral("stale"));
    QCOMPARE(voice.data(voice.index(1), VoiceStudioController::GenerationState).toString(), QStringLiteral("candidate"));
}
void EditorIntegrationTests::voiceCloudDiscoveryScopesAndCancellation()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    const auto account = configureUnifiedSynthesis(context.get()); QVERIFY(!account.isEmpty());
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough); session.newProject();
    VoiceLibraryController voices(context.get(), &session);
    const auto response = [](const QJsonObject &output) { return HttpResponse{200, {}, QJsonDocument(QJsonObject{{QStringLiteral("output"), output}}).toJson(QJsonDocument::Compact)}; };
    QJsonArray full; for (int i = 0; i < 100; ++i) full.append(QJsonObject{{QStringLiteral("voice_id"), QStringLiteral("owned-OK")}});
    const QJsonObject ok{{QStringLiteral("target_model"), QStringLiteral("qwen-audio-3.1-tts-flash")}, {QStringLiteral("status"), QStringLiteral("OK")}, {QStringLiteral("resource_link"), QStringLiteral("https://example.com/reference.wav?private=signature")}};
    const QJsonArray tail{QJsonObject{{QStringLiteral("voice_id"), QStringLiteral("pending")}}, QJsonObject{{QStringLiteral("voice_id"), QStringLiteral("wrong-model")}}};
    http.responses = {response({{QStringLiteral("voice_list"), full}}), response(ok), response({{QStringLiteral("voice_list"), tail}}), response({{QStringLiteral("target_model"), QStringLiteral("qwen-audio-3.1-tts-flash")}, {QStringLiteral("status"), QStringLiteral("DEPLOYING")}}), response({{QStringLiteral("target_model"), QStringLiteral("other-model")}, {QStringLiteral("status"), QStringLiteral("OK")}})};
    voices.discoverVoices(); QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.voiceCatalog().value(QStringLiteral("state")).toString(), QStringLiteral("success"));
    const auto rows = voices.voiceCatalog().value(QStringLiteral("rows")).toList(); QCOMPARE(rows.size(), 3); QCOMPARE(http.sendCount, 5);
    QVERIFY(rows[0].toMap().value(QStringLiteral("compatible")).toBool()); QVERIFY(!rows[1].toMap().value(QStringLiteral("compatible")).toBool()); QVERIFY(!rows[2].toMap().value(QStringLiteral("compatible")).toBool());
    for (const auto &request : http.requests) { QVERIFY(request.url.path().endsWith(QStringLiteral("customization"))); QVERIFY(!request.body.contains("synthesize")); }
    QVERIFY(!QJsonDocument::fromVariant(voices.voiceCatalog()).toJson().contains("private=signature"));
    QSignalSpy selected(&voices, &VoiceLibraryController::voiceSelected); voices.useDiscoveredVoice(QStringLiteral("pending")); QCOMPARE(selected.count(), 0);
    voices.useDiscoveredVoice(QStringLiteral("owned-OK")); QTRY_VERIFY(!voices.busy()); QCOMPARE(selected.count(), 1); QCOMPARE(http.sendCount, 5);
    { VoiceLibraryController cached(context.get(), &session); cached.discoverVoices(false); QCOMPARE(cached.voiceCatalog().value(QStringLiteral("state")).toString(), QStringLiteral("stale_cache")); QCOMPARE(http.sendCount, 5); }
    http.responses.append(response(ok)); QFile audio(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(audio.open(QIODevice::ReadOnly)); http.downloadBody = audio.readAll();
    QSignalSpy audition(&voices, &VoiceLibraryController::auditionRequested); voices.previewCloudVoice(QStringLiteral("owned-OK")); QTRY_VERIFY(!voices.busy()); QCOMPARE(audition.count(), 1); QCOMPARE(http.sendCount, 6);
    const auto previewFile = audition.first().first().toString(); QVERIFY(QFileInfo::exists(previewFile)); voices.previewCloudVoice(QStringLiteral("owned-OK")); QCOMPARE(audition.count(), 2); QCOMPARE(http.sendCount, 6);
    CloudSettingsController cloud(context.get()); cloud.begin(); QVERIFY(cloud.stageKey(account, QStringLiteral("rotated-catalog-key"))); QVERIFY(cloud.save());
    voices.useDiscoveredVoice(QStringLiteral("owned-OK")); QCOMPARE(selected.count(), 1); QVERIFY(voices.statusText().contains(QStringLiteral("刷新")));
    http.blockUntilCancel = true; voices.discoverVoices(); QTRY_VERIFY(http.inFlight.load()); voices.cancelAndWait(); QVERIFY(!http.inFlight.load()); QVERIFY(!voices.busy()); QVERIFY(!QFileInfo::exists(previewFile));
    QCoreApplication::processEvents(); QCOMPARE(selected.count(), 1);
    http.blockUntilCancel = false; http.responses.append(HttpResponse{403, {}, {}}); voices.discoverVoices(); QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.voiceCatalog().value(QStringLiteral("state")).toString(), QStringLiteral("unauthorized"));
}
void EditorIntegrationTests::voiceNativeModelDiscoveryAndWorkspace()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    CloudSettingsController cloud(context.get()); cloud.begin(); const auto account = cloud.addAccount(QStringLiteral("bailian"));
    QVERIFY(cloud.stageKey(account, QStringLiteral("native-catalog-key")));
    cloud.discoverModels(account); QVERIFY(!cloud.busy()); QCOMPARE(http.sendCount, 0); QVERIFY(!cloud.discovered().isEmpty());
    cloud.editAccount(account, {{QStringLiteral("workspace"), QStringLiteral("account-workspace")}});
    http.responses = {{200, {}, QByteArrayLiteral("{\"success\":true,\"output\":{\"total\":2,\"models\":[{\"model\":\"qwen-audio-3.1-tts-flash\",\"name\":\"Flash\"},{\"model\":\"unsupported-tts\"}]}}")}};
    cloud.discoverModels(account); QTRY_VERIFY(!cloud.busy()); QCOMPARE(http.sendCount, 1); QCOMPARE(http.requests.first().method, QByteArrayLiteral("GET"));
    QCOMPARE(http.requests.first().url.host(), QStringLiteral("account-workspace.cn-beijing.maas.aliyuncs.com")); QCOMPARE(http.requests.first().url.path(), QStringLiteral("/api/v1/models")); QCOMPARE(cloud.discovered().size(), 1);
    const auto model = cloud.addModel(account, cloud.discovered().first().toMap().value(QStringLiteral("id")).toString(), QStringLiteral("qwen-tts")); cloud.bindModel(QStringLiteral("speechSynthesis"), model); QVERIFY(cloud.save());
    QString key, error; const auto synthesis = context->cloudConfig->resolveSynthesis(&key, &error); QVERIFY2(!synthesis.isEmpty(), qPrintable(error)); QCOMPARE(synthesis.value(QStringLiteral("businessSpace")).toString(), QStringLiteral("account-workspace"));
}
void EditorIntegrationTests::voiceEditorCapabilitiesAndBuiltinCatalog()
{
    QTemporaryDir directory; auto context = makeContext(directory); RecordingHttpClient http; context->aiHttp = &http;
    AppController editor(context.get()); RoughCutController rough(context.get()); ProjectSession session(context.get(), &editor, &rough);
    VoiceStudioController voice(context.get(), &session); session.newProject();
    QCOMPARE(voice.engineCapabilities(QStringLiteral("gpt")).value(QStringLiteral("languages")).toStringList(),
        (QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}));
    QVERIFY(voice.engineCapabilities(QStringLiteral("index")).value(QStringLiteral("parameters")).toStringList().contains(QStringLiteral("emotion")));
    QVERIFY(!voice.engineCapabilities(QStringLiteral("qwen")).value(QStringLiteral("parameters")).toStringList().contains(QStringLiteral("emotion")));
    VoiceLibraryController voices(context.get(), &session);
    QVERIFY(!voices.builtinVoices().isEmpty());
    voice.previewText(QStringLiteral("首次连接。")); voice.applyPreview();
    QCOMPARE(voice.generationReadiness(QStringLiteral("cloud"), {}, {}).value(QStringLiteral("action")).toString(), QStringLiteral("settings"));
    const auto builtin = voices.builtinVoices().first().toMap();
    QFile sample(mediaPath(QStringLiteral("audio.wav"))); QVERIFY(sample.open(QIODevice::ReadOnly)); http.downloadBody = sample.readAll();
    QSignalSpy audition(&voices, &VoiceLibraryController::auditionRequested); voices.previewCloudVoice(builtin.value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voices.busy());
    QCOMPARE(audition.count(), 1); QCOMPARE(http.sendCount, 0); QCOMPARE(http.downloadCount, 1); QVERIFY(http.downloadRequest.headers.empty()); QVERIFY(!http.downloadRequest.followRedirects); QCOMPARE(http.downloadRequest.maxDownloadBytes, 32 * 1024 * 1024);
    QVERIFY(voices.previewCacheBytes() > 0 && voices.previewCacheBytes() <= 32 * 1024 * 1024);
    voices.previewCloudVoice(builtin.value(QStringLiteral("id")).toString()); QCOMPARE(audition.count(), 2); QCOMPARE(http.downloadCount, 1);
    voices.cancelAndWait();
    http.blockDownloadUntilCancel = true;
    voices.previewCloudVoice(builtin.value(QStringLiteral("id")).toString()); QTRY_VERIFY(http.inFlight.load());
    QVERIFY(voices.previewLoading()); voices.cancelPreview(); QCoreApplication::processEvents();
    QVERIFY(!voices.previewLoading()); QVERIFY(!voices.busy()); QCOMPARE(audition.count(), 2);
    voices.previewCloudVoice(builtin.value(QStringLiteral("id")).toString()); QTRY_VERIFY(http.inFlight.load());
    session.newProject(); QCoreApplication::processEvents();
    QVERIFY(!voices.previewLoading()); QVERIFY(!voices.busy()); QCOMPARE(audition.count(), 2);
    http.blockDownloadUntilCancel = false;
    voices.useBuiltinVoice(builtin.value(QStringLiteral("id")).toString());
    QVERIFY(!voices.busy()); QCOMPARE(http.sendCount, 0);
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty());
    QSignalSpy selected(&voices, &VoiceLibraryController::voiceSelected);
    voices.useBuiltinVoice(builtin.value(QStringLiteral("id")).toString());
    QTRY_VERIFY(!voices.busy()); QCOMPARE(selected.count(), 1); QCOMPARE(http.sendCount, 0);
    QCOMPARE(voices.versions().size(), 1);
    const auto metadata = voices.versions().first().toMap().value(QStringLiteral("metadata")).toMap();
    QCOMPARE(metadata.value(QStringLiteral("providerVoiceId")).toString(), builtin.value(QStringLiteral("id")).toString());
    QVERIFY(metadata.contains(QStringLiteral("catalogOrigin"))); QVERIFY(!metadata.contains(QStringLiteral("key")));
    const auto profileCount = voices.profiles().size(); const auto firstVersion = voices.versions().first().toMap().value(QStringLiteral("id"));
    voices.useBuiltinVoice(builtin.value(QStringLiteral("id")).toString()); QTRY_VERIFY(!voices.busy()); QCOMPARE(voices.profiles().size(), profileCount); QCOMPARE(voices.versions().first().toMap().value(QStringLiteral("id")), firstVersion);
    voice.previewText(QStringLiteral("第一句。第二句。")); voice.applyPreview();
    voice.bindVoiceVersion(selected.first().first().toString(), QStringLiteral("selected"), selected.first().last().toString());
    QTRY_VERIFY(!voice.busy());
    QCOMPARE(voice.selected().value(QStringLiteral("effectiveVoiceVersionId")).toString(), selected.first().first().toString());
    QCOMPARE(voice.projectVoices().size(), 1); QCOMPARE(voice.generationReadiness(QStringLiteral("cloud"), {}, {}).value(QStringLiteral("state")).toString(), QStringLiteral("ready"));
    QCOMPARE(voice.generationReadiness(QStringLiteral("qwen"), {}, {}).value(QStringLiteral("state")).toString(), QStringLiteral("voice"));
    QVERIFY(voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty()); QCOMPARE(http.sendCount, 0);
}
void EditorIntegrationTests::voiceSearchPreservesSelectionAndDocument()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
    voice.previewText(QStringLiteral("第一段旁白。\n第二段旁白。")); voice.applyPreview();
    const auto frozen = voice.snapshot(); const auto selected = voice.selectedId();
    voice.setManuscriptFilter(QStringLiteral("第二"), QStringLiteral("missing"), 2);
    QCOMPARE(voice.filteredModel()->rowCount(), 1); QCOMPARE(voice.selectedId(), selected); QCOMPARE(voice.snapshot(), frozen);
    QCOMPARE(voice.filteredIndex(selected), -1); QVERIFY(!voice.adjacentFilteredId(1).isEmpty());
    voice.setManuscriptFilter({}, QStringLiteral("all")); QCOMPARE(voice.filteredModel()->rowCount(), 2);
    QCOMPARE(voice.filteredIndex(selected), 0); QCOMPARE(voice.snapshot(), frozen);
}

void EditorIntegrationTests::recoveryPreservesUnnamedDocumentAndFieldDrafts()
{
    QTemporaryDir directory; QString id, workspace;
    {
        auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
        ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
        voice.previewText(QStringLiteral("崩溃后仍可继续的文稿。")); voice.applyPreview();
        id = session.projectId(); workspace = QFileInfo(session.storagePath()).absolutePath();
        session.setRecoveryDrafts({{QStringLiteral("voice"), QVariantMap{{QStringLiteral("id"), voice.selectedId()}, {QStringLiteral("text"), QStringLiteral("未应用的字段草稿")}, {QStringLiteral("gain"), QStringLiteral("非法输入也保留")}}}});
        session.shutdown(); QVERIFY(QFileInfo::exists(workspace)); QCOMPARE(session.recoverableProjects().size(), 1);
    }
    {
        auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
        ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
        QSignalSpy opened(&session, &ProjectSession::opened); QVERIFY(session.recoverProject(id)); QTRY_COMPARE(opened.count(), 1);
        QVERIFY2(opened.first().first().toBool(), qPrintable(session.statusText()));
        QVERIFY(session.path().isEmpty()); QCOMPARE(voice.rowCount(), 1); QVERIFY(session.modified());
        QCOMPARE(voice.selected().value(QStringLiteral("text")).toString(), QStringLiteral("崩溃后仍可继续的文稿。"));
        QCOMPARE(session.recoveryDrafts().value(QStringLiteral("voice")).toMap().value(QStringLiteral("gain")).toString(), QStringLiteral("非法输入也保留"));
        session.setRecoveryDrafts({});
        const auto destination = directory.filePath(QStringLiteral("recovered.subcue-project")); QSignalSpy saved(&session, &ProjectSession::saved);
        QVERIFY(session.saveProject(QUrl::fromLocalFile(destination))); QTRY_COMPARE(saved.count(), 1);
        QVERIFY2(saved.first().first().toBool(), qPrintable(session.statusText()));
        QVERIFY(!QFileInfo::exists(workspace)); QVERIFY(session.recoverableProjects().isEmpty());
        voice.editSegment(voice.selectedId(), QStringLiteral("尚未正式保存的新正文。"), {}, false, {});
        session.shutdown(); QCOMPARE(session.recoverableProjects().size(), 1);
        ProjectStore external(destination); const auto changed = external.load(); QVERIFY(changed); QVERIFY(external.save(*changed));
    }
    {
        auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
        ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
        QSignalSpy opened(&session, &ProjectSession::opened); QVERIFY(session.recoverProject(id)); QTRY_COMPARE(opened.count(), 1);
        QVERIFY(!opened.first().first().toBool()); QVERIFY(session.statusText().contains(QStringLiteral("外部修改")));
        QCOMPARE(voice.rowCount(), 0); QVERIFY(session.path().isEmpty());
        QVERIFY(session.discardRecovery(id)); QVERIFY(session.recoverableProjects().isEmpty());
    }
}

void EditorIntegrationTests::normalExitDiscardsRecovery()
{
    for (const bool named : {false, true}) {
        QTemporaryDir directory;
        auto context = makeContext(directory);
        AppController editor(context.get()); RoughCutController rough(context.get());
        ProjectSession session(context.get(), &editor, &rough);
        session.newProject();
        if (named) {
            QSignalSpy saved(&session, &ProjectSession::saved);
            QVERIFY(session.saveProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("saved.subcue-project")))));
            QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
        }
        const auto storage = session.storagePath();
        editor.setScriptText(QStringLiteral("正常关闭时选择放弃的修改"));
        session.setRecoveryDrafts({{QStringLiteral("subtitle"), QVariantMap{{QStringLiteral("id"), QStringLiteral("draft")}, {QStringLiteral("text"), QStringLiteral("尚未应用的字段")}}}});
        QTRY_COMPARE_WITH_TIMEOUT(session.recoverableProjects().size(), 1, 15000);
        session.shutdown(true);
        QVERIFY(session.recoverableProjects().isEmpty());
        QCOMPARE(QFileInfo::exists(storage), named);
        if (named) {
            const auto stored = ProjectStorageAdapter(storage, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"));
            QVERIFY(stored);
            QVERIFY(std::get<Project>(*stored).state.value(QStringLiteral("scriptText")).toString().isEmpty());
        }
    }
}

void EditorIntegrationTests::openingProjectDiscardsPreviousRecovery()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough);
    session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved), opened(&session, &ProjectSession::opened);
    const auto original = directory.filePath(QStringLiteral("original.subcue-project"));
    const auto next = directory.filePath(QStringLiteral("next.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(original)));
    QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.last().first().toBool());
    QVERIFY(ProjectStore(next).create());
    editor.setScriptText(QStringLiteral("切换工程时放弃的内容"));
    QTRY_COMPARE_WITH_TIMEOUT(session.recoverableProjects().size(), 1, 15000);
    QVERIFY(session.openProject(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.subcue-project")))));
    QTRY_COMPARE(opened.count(), 1); QVERIFY(!opened.last().first().toBool());
    QCOMPARE(session.recoverableProjects().size(), 1);
    QCOMPARE(editor.scriptText(), QStringLiteral("切换工程时放弃的内容"));
    QVERIFY(session.openProject(QUrl::fromLocalFile(next)));
    QTRY_COMPARE(opened.count(), 2); QVERIFY(opened.last().first().toBool());
    QVERIFY(session.recoverableProjects().isEmpty());
    session.shutdown(true);
    QVERIFY(session.recoverableProjects().isEmpty());
    const auto stored = ProjectStorageAdapter(original, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"));
    QVERIFY(stored);
    QVERIFY(std::get<Project>(*stored).state.value(QStringLiteral("scriptText")).toString().isEmpty());
}

void EditorIntegrationTests::voiceSaveWaitsWithoutCancellingGeneration()
{
    const auto python = sourcePath(QStringLiteral(".venv-inference/Scripts/python.exe"));
    if (!QFileInfo::exists(python)) QSKIP("需要独立 Python 执行 Mock 协议测试");
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session); TtsTaskController tasks(context.get(), &session);
    voice.setTasks(&tasks); session.setVoiceServices(&tasks, nullptr, nullptr); session.newProject();
    voice.previewText(QStringLiteral("等待保存的旁白。")); voice.applyPreview();
    const auto version = QUuid::createUuid().toString(), profile = QUuid::createUuid().toString();
    QTRY_VERIFY(QFileInfo::exists(session.storagePath()));
    QVERIFY(ProjectStore(session.storagePath()).importVoiceVersion(profile, version, {}, {}, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("providerVoiceId"), QStringLiteral("Cherry")}}));
    voice.bindVoiceVersion(version); QTRY_VERIFY(!voice.busy());
    QSignalSpy finished(&tasks, &TtsTaskController::finished), saved(&session, &ProjectSession::saved);
    QSemaphore entered, release;
    auto blocker = context->submitProjectStorage([&] { entered.release(); (void)release.tryAcquire(1, 5000); });
    QVERIFY(entered.tryAcquire(1, 2000));
    voice.generateMock(python, QStringLiteral("selected")); QVERIFY(voice.busy());
    const auto output = directory.filePath(QStringLiteral("waited.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(output))); QVERIFY(session.savePending()); QVERIFY(!session.busy());
    session.cancelPendingSave(); QVERIFY(!session.savePending()); QCOMPARE(finished.count(), 0);
    QVERIFY(session.saveProject(QUrl::fromLocalFile(output))); QVERIFY(session.savePending());
    release.release();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(tasks.statusText()));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QVERIFY2(saved.first().first().toBool(), qPrintable(session.statusText()));
    QVERIFY(!session.savePending()); QCOMPARE(ProjectStore(output).history().size(), 1); QVERIFY(blocker.isFinished());
}

void EditorIntegrationTests::voiceUnsavedWorkspaceGeneratesExportsAndMigratesAssets()
{
    const auto python = sourcePath(QStringLiteral(".venv-inference/Scripts/python.exe"));
    if (!QFileInfo::exists(python)) QSKIP("需要项目独立 Python 执行 Mock 协议测试");
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    VoiceStudioController voice(context.get(), &session);
    TtsTaskController tasks(context.get(), &session); voice.setTasks(&tasks);
    VoiceLibraryController voices(context.get(), &session, directory.filePath(QStringLiteral("voices.sqlite")));
    AudioPreviewController preview(context.get(), &session);
    VoiceAudioController audio(context.get(), &session, &voice, &preview); audio.setActive(true);
    session.newProject();
    QVERIFY(session.path().isEmpty()); QVERIFY(!session.storagePath().isEmpty());
    const auto temporary = QFileInfo(session.storagePath()).absolutePath();
    voice.previewText(QStringLiteral("第一句旁白。第二句旁白。")); voice.applyPreview();
    QCOMPARE(voice.rowCount(), 2);
    QSignalSpy operations(&voices, &VoiceLibraryController::operationFinished);
    voices.createProfile(QStringLiteral("无需保存的旁白")); QTRY_COMPARE(operations.count(), 1);
    QVERIFY(!configureUnifiedSynthesis(context.get()).isEmpty());
    voices.addCloudVersion(QStringLiteral("yuxiaoyun_v3.1")); QTRY_COMPARE(operations.count(), 2);
    const auto version = voices.versions().first().toMap().value(QStringLiteral("id")).toString();
    voices.copyToProject(version); QTRY_COMPARE(operations.count(), 3);
    QVERIFY2(operations.last().first().toBool(), qPrintable(voices.statusText()));
    QVERIFY(ProjectStore(session.storagePath()).voiceVersion(version));
    voice.bindVoiceVersion(version, QStringLiteral("unbound")); QTRY_VERIFY(!voice.busy());
    QSignalSpy generated(&tasks, &TtsTaskController::finished);
    voice.generateMock(python, QStringLiteral("invalid"));
    QVERIFY2(voice.busy() || tasks.busy() || generated.count() > 0, qPrintable(voice.statusText()));
    QTRY_COMPARE_WITH_TIMEOUT(generated.count(), 1, 15000);
    QVERIFY2(generated.last().first().toBool(), qPrintable(tasks.statusText()));
    QTRY_VERIFY(!voice.busy()); voice.applySafeCandidates(); QTRY_VERIFY(!voice.busy());
    QVERIFY(!voice.selected().value(QStringLiteral("selectedTake")).toMap().isEmpty());
    audio.prepare(); QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    QCOMPARE(audio.rows().size(), 2); QTRY_VERIFY(preview.playing());
    preview.pause(); QVERIFY(preview.paused()); QVERIFY(!preview.playing());
    const auto pausedPosition = preview.positionUs(); QTest::qWait(80); QCOMPARE(preview.positionUs(), pausedPosition);
    preview.resume(); QVERIFY(preview.playing()); QVERIFY(!preview.paused()); audio.stop();
    audio.exportWav(QUrl::fromLocalFile(directory.filePath(QStringLiteral("unsaved.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    QVERIFY2(QFileInfo::exists(directory.filePath(QStringLiteral("unsaved.wav"))), qPrintable(audio.statusText()));
    QVERIFY(session.path().isEmpty()); QVERIFY(session.modified());
    const auto savedPath = directory.filePath(QStringLiteral("named.subcue-project"));
    QSignalSpy saved(&session, &ProjectSession::saved);
    QSemaphore started, release;
    auto pendingStorage = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5000); });
    QVERIFY(started.tryAcquire(1, 2000));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(savedPath)));
    // 迁移时不能再启动依赖旧临时资产的任务，文本仍保留在当前工作区。
    audio.prepare(); QVERIFY(!audio.busy());
    preview.play(voice.candidates().first().toMap().value(QStringLiteral("path")).toString(),
                 voice.candidates().first().toMap().value(QStringLiteral("sha256")).toString());
    QVERIFY(!preview.loading()); QVERIFY(!preview.playing());
    voice.loadCandidates(); QVERIFY(!voice.busy());
    voice.generateMock(python, QStringLiteral("selected")); QVERIFY(!tasks.busy());
    release.release(); QTRY_COMPARE(saved.count(), 1); QVERIFY(pendingStorage.isFinished());
    QVERIFY2(saved.last().first().toBool(), qPrintable(session.statusText()));
    QTRY_VERIFY(!voice.busy()); QTRY_VERIFY(!QFileInfo::exists(temporary));
    QCOMPARE(session.storagePath(), savedPath); QVERIFY(!session.modified());
    QCOMPARE(ProjectStore(savedPath).history().size(), 2);
    QVERIFY(ProjectStore(savedPath).voiceVersion(version));
    for (const auto &candidate : voice.candidates()) QVERIFY(QFileInfo::exists(candidate.toMap().value(QStringLiteral("path")).toString()));
    audio.exportWav(QUrl::fromLocalFile(directory.filePath(QStringLiteral("saved.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!audio.busy(), 10000);
    const auto savedAudio = AudioQuality::inspect(directory.filePath(QStringLiteral("saved.wav")));
    const auto unsavedAudio = AudioQuality::inspect(directory.filePath(QStringLiteral("unsaved.wav")));
    QVERIFY2(savedAudio, qPrintable(audio.statusText())); QVERIFY(unsavedAudio);
    QCOMPARE(savedAudio->sha256, unsavedAudio->sha256);
    QStringList discarded;
    for (int i = 0; i < 50; ++i) {
        session.newProject(); discarded.append(QFileInfo(session.storagePath()).absolutePath());
    }
    session.shutdown(); QCoreApplication::processEvents();
    for (const auto &path : discarded) QVERIFY2(!QFileInfo::exists(path), qPrintable(path));
    QVERIFY(!tasks.busy()); QVERIFY(!preview.playing()); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QVERIFY(QFileInfo::exists(savedPath));
}

void EditorIntegrationTests::ttsTaskRejectsSilentAudioAndContinuesBatch()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString path = directory.filePath(QStringLiteral("quality.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1);
    const QString python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("需要独立测试解释器");
    LocalTtsProvider provider(LocalTtsProvider::Kind::Mock, python, sourcePath(QStringLiteral("tools/tts_python_worker.py")));
    TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectId = session.projectId(); job.input.projectGeneration = session.generation();
    job.input.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces); job.input.voiceVersionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.spokenText = QStringLiteral("质检提交测试"); job.input.language = QStringLiteral("Chinese");
    job.input.provider = QStringLiteral("mock"); job.input.model = QStringLiteral("mock-tone"); job.input.modelVersion = QStringLiteral("1");
    auto silent = job; silent.id = QUuid::createUuid().toString(QUuid::WithoutBraces); silent.simulation = QStringLiteral("silence");
    TtsTaskController tasks(context.get(), &session);
    QSignalSpy finished(&tasks, &TtsTaskController::finished), candidates(&tasks, &TtsTaskController::candidateCommitted);
    QVERIFY(tasks.start(provider, {silent, job})); QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!finished.first().first().toBool()); QCOMPARE(candidates.count(), 1);
    QCOMPARE(ProjectStore(path).history().size(), 1);
    const auto records = ProjectStore(path).jobs(); QCOMPARE(records.size(), 2);
    for (const auto &item : records) {
        const auto record = item.toObject();
        if (record.value(QStringLiteral("id")).toString() == silent.id) {
            QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
            QVERIFY(record.value(QStringLiteral("diagnostics")).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("静音")));
        } else {
            QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
            QVERIFY(record.value(QStringLiteral("result")).toObject().value(QStringLiteral("quality")).isObject());
        }
    }
    QVERIFY(!session.modified()); QVERIFY(InferenceManager::instance().tasks().isEmpty());
}

void EditorIntegrationTests::ttsTaskCountsOnlyCommittedCandidates()
{
    QTemporaryDir directory; auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut); session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString path = directory.filePath(QStringLiteral("commit-failure.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path))); QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.first().first().toBool());
    const QString python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("需要独立测试解释器");
    LocalTtsProvider provider(LocalTtsProvider::Kind::Mock, python, sourcePath(QStringLiteral("tools/tts_python_worker.py")));
    TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectId = session.projectId(); job.input.projectGeneration = session.generation();
    job.input.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces); job.input.voiceVersionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.spokenText = QStringLiteral("候选提交失败测试"); job.input.language = QStringLiteral("Chinese");
    job.input.provider = QStringLiteral("mock"); job.input.model = QStringLiteral("mock-tone"); job.input.modelVersion = QStringLiteral("1");
    // 只对本测试的空资产目录注入写入故障，不依赖线程时序或真实模型。
    QVERIFY(QDir().rmdir(path + QStringLiteral(".assets")));
    QFile blocked(path + QStringLiteral(".assets")); QVERIFY(blocked.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(blocked.write("blocked"), qint64(7)); blocked.close();
    TtsTaskController tasks(context.get(), &session);
    QSignalSpy finished(&tasks, &TtsTaskController::finished), candidates(&tasks, &TtsTaskController::candidateCommitted);
    QVERIFY(tasks.start(provider, {job})); QTRY_COMPARE(finished.count(), 1); QVERIFY(!finished.first().first().toBool());
    QCOMPARE(candidates.count(), 0); QVERIFY(ProjectStore(path).history().isEmpty());
    QCOMPARE(ProjectStore(path).jobs().first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
    QCOMPARE(tasks.segmentState(job.input.segmentId), QStringLiteral("failed"));
    QVERIFY(tasks.statusText().contains(QStringLiteral("完成 0 句")));
    QVERIFY(blocked.remove()); QVERIFY(QDir().mkpath(path + QStringLiteral(".assets")));
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(tasks.start(provider, {job})); QTRY_COMPARE(finished.count(), 2); QVERIFY(finished.last().first().toBool());
    QCOMPARE(candidates.count(), 1); QCOMPARE(ProjectStore(path).history().size(), 1);
    QCOMPARE(tasks.segmentState(job.input.segmentId), QStringLiteral("candidate"));
    QVERIFY(tasks.statusText().contains(QStringLiteral("完成 1 句")));
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
}

void EditorIntegrationTests::ttsTaskOwnerPersistsAndCancelsBeforeProjectChange()
{
    QTemporaryDir directory;
    auto context = makeContext(directory);
    AppController editor(context.get()); RoughCutController roughCut(context.get());
    ProjectSession session(context.get(), &editor, &roughCut);
    session.newProject();
    QSignalSpy saved(&session, &ProjectSession::saved);
    const QString path = directory.filePath(QStringLiteral("tts.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(path)));
    QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.first().first().toBool());
    TtsTaskController tasks(context.get(), &session);
    const QString python = QStringLiteral("E:/AIModels/TTS/.venv/Scripts/python.exe");
    if (!QFileInfo::exists(python)) QSKIP("需要独立测试解释器");
    LocalTtsProvider provider(LocalTtsProvider::Kind::Mock, python, sourcePath(QStringLiteral("tools/tts_python_worker.py")));
    TtsJob job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.projectId = session.projectId(); job.input.projectGeneration = session.generation();
    job.input.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.voiceVersionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.input.spokenText = QStringLiteral("Mock 所有者测试"); job.input.language = QStringLiteral("Chinese");
    job.input.provider = QStringLiteral("mock"); job.input.model = QStringLiteral("mock-tone"); job.input.modelVersion = QStringLiteral("1");
    QSignalSpy finished(&tasks, &TtsTaskController::finished);
    QSignalSpy candidate(&tasks, &TtsTaskController::candidateCommitted);
    QVERIFY(tasks.start(provider, {job}));
    editor.setScriptText(QStringLiteral("生成期间手工编辑"));
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY2(finished.first().first().toBool(), qPrintable(tasks.statusText()));
    QCOMPARE(candidate.count(), 1);
    QVERIFY(QFileInfo::exists(candidate.first().at(2).toString()));
    QCOMPARE(editor.scriptText(), QStringLiteral("生成期间手工编辑"));
    QVERIFY(session.modified());
    QCOMPARE(ProjectStore(path).history().size(), 1);
    const auto batchId = ProjectStore(path).jobs().first().toObject().value(QStringLiteral("batchId")).toString();
    QVERIFY(!QUuid(batchId).isNull());
    tasks.loadRecords(0, 50, batchId);
    QTRY_VERIFY(!tasks.recordsLoading());
    QCOMPARE(tasks.records().size(), 1);
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation = QStringLiteral("delay");
    QVERIFY(tasks.start(provider, {job}));
    QTRY_VERIFY(!InferenceManager::instance().tasks().isEmpty());
    session.newProject(); // 发出切换通知，先取消并等待旧 Worker／存储，再改变工程。
    QVERIFY(!tasks.busy()); QVERIFY(InferenceManager::instance().tasks().isEmpty());
    const auto oldJobs = ProjectStore(path).jobs();
    QCOMPARE(oldJobs.first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
    QCOMPARE(ProjectStore(path).history().size(), 1);
    QCoreApplication::processEvents();
    QCOMPARE(candidate.count(), 1); QCOMPARE(finished.count(), 1);
    QVERIFY(session.projectId() != job.input.projectId);
    QVERIFY(!tasks.start(provider, {job}));
    const QString secondPath = directory.filePath(QStringLiteral("second.subcue-project"));
    QVERIFY(session.saveProject(QUrl::fromLocalFile(secondPath)));
    QTRY_COMPARE(saved.count(), 2); QVERIFY(saved.last().first().toBool());
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.simulation.clear();
    job.input.projectId = session.projectId(); job.input.projectGeneration = session.generation();
    QSemaphore started, release;
    auto preceding = context->submitProjectStorage([&] { started.release(); (void)release.tryAcquire(1, 5000); });
    QVERIFY(started.tryAcquire(1, 2000));
    auto queued = std::make_unique<TtsTaskController>(context.get(), &session);
    QVERIFY(queued->start(provider, {job}));
    queued->cancel();
    std::jthread unblock([&] { QThread::msleep(50); release.release(); });
    queued.reset(); // 销毁等待后台及存储任务，不能依赖 GUI 的完成回调。
    QVERIFY(preceding.isFinished());
    QVERIFY(ProjectStore(secondPath).history().isEmpty());
    QVERIFY(InferenceManager::instance().tasks().isEmpty());
    QCoreApplication::processEvents();
}

void EditorIntegrationTests::destroyingControllerWaitsForQueuedSave_data()
{
    QTest::addColumn<bool>("roughCut");
    QTest::newRow("subtitle") << false;
    QTest::newRow("roughcut") << true;
}

void EditorIntegrationTests::destroyingControllerWaitsForQueuedSave()
{
    QFETCH(bool, roughCut);
    QTemporaryDir directory;
    auto context = makeContext(directory);
    QSemaphore started;
    QSemaphore release;
    auto preceding = context->submitProjectStorage([&] {
        started.release();
        (void)release.tryAcquire(1, 5'000);
    });
    QVERIFY(started.tryAcquire(1, 2'000));
    const QString path = directory.filePath(roughCut
        ? QStringLiteral("destroyed.subcue-roughcut") : QStringLiteral("destroyed.subcue"));
    if (roughCut) {
        auto controller = std::make_unique<RoughCutController>(context.get());
        QVERIFY(controller->saveProject(QUrl::fromLocalFile(path)));
        controller->cancelAnalysis();
        std::jthread unblock([&] { QThread::msleep(50); release.release(); });
        controller.reset();
    } else {
        auto controller = std::make_unique<AppController>(context.get());
        QVERIFY(controller->saveProject(QUrl::fromLocalFile(path)));
        controller->cancelAlignment();
        std::jthread unblock([&] { QThread::msleep(50); release.release(); });
        controller.reset();
    }
    QVERIFY(preceding.isFinished());
    QVERIFY(!QFileInfo::exists(path));
    // 排队到 GUI 的完成通知在接收者销毁后不得再次访问控制器。
    QCoreApplication::processEvents();
}

QString EditorIntegrationTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

QString EditorIntegrationTests::sourcePath(const QString &relative) const
{
    return QDir(QString::fromUtf8(SUBCUE_SOURCE_DIR)).filePath(relative);
}

void EditorIntegrationTests::cleanup()
{
    // 临时目录删除不会删除 Windows 凭据；按本用例创建的路径隔离清理。
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

std::unique_ptr<ApplicationContext> EditorIntegrationTests::makeContext(const QTemporaryDir &dir)
{
    credentialPaths_.append(dir.filePath(QStringLiteral("credentials.dat")));
    return std::make_unique<ApplicationContext>(
        dir.filePath(QStringLiteral("settings.json")),
        dir.filePath(QStringLiteral("credentials.dat")),
        AudioDeviceKind::Virtual);
}

void EditorIntegrationTests::qmlShortcutsMatchFeatureMatrix()
{
    QFile cppFile(sourcePath(QStringLiteral("src/app/qml/Main.qml")));
    QVERIFY(cppFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString cppSource = QString::fromUtf8(cppFile.readAll());
    const QSet<QString> cpp = shortcutTokens(cppSource);
    QCOMPARE(cpp, frozenEditorShortcuts());
    QVERIFY(cppSource.contains(QStringLiteral("createNextScriptCue")));
    QVERIFY(cppSource.contains(QStringLiteral("exportSubtitles")));

    QFile workspaceFile(sourcePath(QStringLiteral("src/app/qml/SubtitleWorkspace.qml")));
    QVERIFY(workspaceFile.open(QIODevice::ReadOnly | QIODevice::Text));
    auto editingKeys = frozenEditorShortcuts();
    editingKeys.remove(QStringLiteral("Ctrl+O"));
    editingKeys.remove(QStringLiteral("Ctrl+I"));
    editingKeys.insert(QStringLiteral("Shift+Tab"));
    editingKeys.insert(QStringLiteral("Ctrl+S"));
    editingKeys.insert(QStringLiteral("Ctrl+Shift+Z"));
    QCOMPARE(shortcutTokens(QString::fromUtf8(workspaceFile.readAll())), editingKeys);
}

void EditorIntegrationTests::roughReviewNavigationPreservesFilters()
{
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")).toUtf8());
#endif
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("SubCue")));
    const QString modulePath = dir.filePath(QStringLiteral("SubCue"));
    QByteArray manifest("module SubCue\n");
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml"))).entryList({QStringLiteral("*.qml")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/") + file), QDir(modulePath).filePath(file)));
        if (file == QStringLiteral("Theme.qml")) manifest += "singleton ";
        manifest += QFileInfo(file).baseName().toUtf8() + " 1.0 " + file.toUtf8() + "\n";
    }
    QVERIFY(QDir(modulePath).mkdir(QStringLiteral("icons")));
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList(QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file), QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    }
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly));
    QCOMPARE(qmldir.write(manifest), manifest.size());
    qmldir.close();
    registerSubCueQmlItems();
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    auto context = makeContext(dir);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    QStringList warnings;
    QQmlApplicationEngine engine;
    engine.addImportPath(dir.path());
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &editor);
    engine.rootContext()->setContextProperty(QStringLiteral("roughCut"), &roughCut);
    connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) warnings.append(error.toString());
    });
    engine.loadData(R"(
        import QtQuick
        import QtQuick.Controls
        import QtQuick.Window
        import SubCue
        Window {
            width: 1440; height: 900; visible: true
            property var settingsWindow: null
            property alias newAction: newAction
            property alias openAction: openAction
            property alias saveAction: saveAction
            property alias saveAsAction: saveAsAction
            property alias importAction: importAction
            property alias scriptAction: scriptAction
            property alias exportAction: exportAction
            Action { id: newAction; text: "New"; property string keySequence: "Ctrl+N" }
            Action { id: openAction; text: "Open"; property string keySequence: "Ctrl+O" }
            Action { id: saveAction; text: "Save"; property string keySequence: "Ctrl+S" }
            Action { id: saveAsAction; text: "Save As"; property string keySequence: "Ctrl+Shift+S" }
            Action { id: importAction; text: "Import"; property string keySequence: "Ctrl+I" }
            Action { id: scriptAction; text: "Script"; property string keySequence: "Ctrl+Shift+I" }
            Action { id: exportAction; text: "Export"; property string keySequence: "Ctrl+E" }
            RoughCutWorkspace { anchors.fill: parent }
        }
    )");
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(u'\n')));
    auto *workspace = engine.rootObjects().first()->findChild<QObject *>(QStringLiteral("roughCutWorkspace"));
    auto *list = engine.rootObjects().first()->findChild<QObject *>(QStringLiteral("roughCutResultList"));
    auto *timeline = engine.rootObjects().first()->findChild<TimelineSceneItem *>(QStringLiteral("roughCutTimeline"));
    QVERIFY(workspace && list && timeline);

    RoughCutProject project;
    project.mediaPath = mediaPath(QStringLiteral("audio.wav"));
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(project.mediaPath);
    project.sampleRate = 16'000;
    project.channels = 1;
    project.sourceSampleCount = 16'000;
    for (int index = 0; index < 4; ++index) {
        project.recording.append({QString::number(index), QStringLiteral("复核导航片段"), index * 4'000, (index + 1) * 4'000});
        RoughCutSegmentDecision decision;
        decision.recordingIndex = index;
        decision.autoDecision = index == 0 ? RoughCutDecision::Keep : index == 2 ? RoughCutDecision::Cut : RoughCutDecision::Review;
        // 明确人工剪除，避免自动剪除保护规则将测试片段提升为待复核。
        if (index == 2) decision.userDecision = RoughCutDecision::Cut;
        project.decisions.append(decision);
    }
    const QString path = dir.filePath(QStringLiteral("review.subcue-roughcut"));
    QVERIFY(RoughCutProjectSerializer::save(path, project));
    roughCut.openProject(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy() && roughCut.resultCount() == 4, 8'000);
    QCOMPARE(roughCut.resultModel()->property("keepCount").toInt(), 1);
    QCOMPARE(roughCut.resultModel()->property("reviewCount").toInt(), 2);
    QCOMPARE(roughCut.resultModel()->property("cutCount").toInt(), 1);
    QVERIFY(list->setProperty("currentIndex", 0));
    QTRY_VERIFY(list->property("currentItem").value<QObject *>());
    QSignalSpy filterChanged(&roughCut, &RoughCutController::statusFilterChanged);

    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QCOMPARE(roughCut.statusFilter(), QStringLiteral("ALL"));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
    QCOMPARE(timeline->selectedCueId(), QStringLiteral("1"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 3);
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, -1)));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 3);
    QCOMPARE(filterChanged.count(), 0);

    roughCut.setStatusFilter(QStringLiteral("REVIEW"));
    QVERIFY(list->setProperty("currentIndex", 0));
    QTRY_VERIFY(list->property("currentItem").value<QObject *>());
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QCOMPARE(roughCut.statusFilter(), QStringLiteral("REVIEW"));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 1);

    roughCut.setStatusFilter(QStringLiteral("KEEP"));
    QVERIFY(list->setProperty("currentIndex", 0));
    QTRY_VERIFY(list->property("currentItem").value<QObject *>());
    filterChanged.clear();
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QCOMPARE(roughCut.statusFilter(), QStringLiteral("KEEP"));
    QCOMPARE(workspace->property("hiddenTimelineSelection").toInt(), 1);
    QCOMPARE(timeline->selectedCueId(), QStringLiteral("1"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, 1)));
    QCOMPARE(workspace->property("hiddenTimelineSelection").toInt(), 3);
    QCOMPARE(timeline->selectedCueId(), QStringLiteral("3"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "goToReview", Q_ARG(QVariant, -1)));
    QCOMPARE(workspace->property("hiddenTimelineSelection").toInt(), 1);
    QCOMPARE(list->property("count").toInt(), 1);
    QCOMPARE(filterChanged.count(), 0);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
}

void EditorIntegrationTests::qmlImportDropAndLayout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("SubCue")));
    const QString modulePath = dir.filePath(QStringLiteral("SubCue"));
    QByteArray manifest("module SubCue\n");
    const QStringList qmlFiles = QDir(sourcePath(QStringLiteral("src/app/qml"))).entryList({QStringLiteral("*.qml")}, QDir::Files);
    for (const QString &file : qmlFiles) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/") + file), QDir(modulePath).filePath(file)));
        if (file == QStringLiteral("Theme.qml")) manifest += "singleton ";
        manifest += QFileInfo(file).baseName().toUtf8() + " 1.0 " + file.toUtf8() + "\n";
    }
    QVERIFY(QDir(modulePath).mkdir(QStringLiteral("icons")));
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList(
             {QStringLiteral("*.svg"), QStringLiteral("*.png")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file), QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    }
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly));
    QCOMPARE(qmldir.write(manifest), manifest.size());
    qmldir.close();
    registerSubCueQmlItems();
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")).toUtf8());
#endif

    auto context = makeContext(dir);
    AppController controller(context.get());
    QStringList warnings;
    QQmlApplicationEngine engine;
    engine.addImportPath(dir.path());
    QQmlComponent nativeTheme(&engine);
    nativeTheme.setData("import QtQuick; QtObject { property bool expandedClientArea: false; function applyDarkTitleBar() {} }", QUrl());
    QObject *theme = nativeTheme.create();
    QVERIFY(theme);
    theme->setParent(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &controller);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(QDir(modulePath).filePath(QStringLiteral("Main.qml"))));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    QVERIFY(window);
    QTest::qWait(200);
    QElapsedTimer settingsTimer;
    settingsTimer.start();
    QVERIFY(QMetaObject::invokeMethod(window, "openSettings"));
    qInfo() << "settings_open_ms=" << settingsTimer.elapsed();
    QVERIFY(settingsTimer.elapsed() < 200);
    auto *settingsWindow = window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    QVERIFY(settingsWindow);
    QTRY_VERIFY(settingsWindow->isVisible());
    auto *settingsLoader = settingsWindow->findChild<QObject *>(QStringLiteral("settingsContentLoader"));
    QVERIFY(settingsLoader);
    QTRY_COMPARE(settingsLoader->property("status").toInt(), 1);
    QTest::qWait(100);
    QVERIFY(settingsWindow->findChild<QObject *>(QStringLiteral("fontFamilyField")));
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("aiAssistSection")));
    QVERIFY(settingsWindow->findChild<QObject *>(QStringLiteral("asrReviewSection")));
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("aiApiKeyField")));
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("testAiConnectionButton")));
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("clearAiKeyButton")));
    QVERIFY(window->findChild<QObject *>(QStringLiteral("aiReviewMenuButton")));
    QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/settings-%1.png")
        .arg(QGuiApplication::platformName()))));
    auto *settingsContent = settingsLoader->property("item").value<QObject *>(); QVERIFY(settingsContent);
    QTRY_VERIFY(!settingsContent->property("dirty").toBool());
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("voiceCloudWorkspace")));
    auto *cloud = qobject_cast<CloudSettingsController *>(controller.cloudSettings()); QVERIFY(cloud);
    auto *cloudPanel = settingsWindow->findChild<QObject *>(QStringLiteral("cloudSettingsPanel")); QVERIFY(cloudPanel);
    const auto accountId = cloud->addAccount(QStringLiteral("bailian"));
    const auto secondId = cloud->addAccount(QStringLiteral("deepseek"));
    cloud->editAccount(accountId, {{QStringLiteral("name"), QStringLiteral("工作账号 · 多模型与长中文名称检查")}});
    QVariant cloudAccount(accountId);
    QVERIFY(QMetaObject::invokeMethod(cloudPanel, "chooseAccount", Q_ARG(QVariant, cloudAccount)));
    auto *cloudKey = settingsWindow->findChild<QObject *>(QStringLiteral("cloudNewKeyField")); QVERIFY(cloudKey);
    cloudPanel->setProperty("replacing", true); cloudKey->setProperty("text", QStringLiteral("qml-cloud-sentinel"));
    QVariant secondAccount(secondId);
    QVERIFY(QMetaObject::invokeMethod(cloudPanel, "chooseAccount", Q_ARG(QVariant, secondAccount)));
    QVERIFY(cloudKey->property("text").toString().isEmpty());
    QVERIFY(!QJsonDocument::fromVariant(cloud->accounts()).toJson().contains("qml-cloud-sentinel"));
    QVERIFY(QMetaObject::invokeMethod(cloudPanel, "chooseAccount", Q_ARG(QVariant, cloudAccount)));
    QCOMPARE(settingsWindow->findChild<QObject *>(QStringLiteral("cloudKeyMask"))->property("text").toString(), QStringLiteral("••••••••  ·  待替换"));
    const auto ttsModel = cloud->addModel(accountId, QStringLiteral("qwen-audio-3.1-tts-flash"), QStringLiteral("qwen-tts"));
    cloud->editModel(ttsModel, {{QStringLiteral("workspace"), QStringLiteral("workspace-example")}});
    QVERIFY(!cloud->addModel(accountId, QStringLiteral("fun-asr-flash"), QStringLiteral("dashscope-asr")).isEmpty());
    cloudPanel->setProperty("modelId", ttsModel);
    settingsContent->setProperty("selectedCategory", 5);
    settingsWindow->resize(900, 680); QTest::qWait(80);
    QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/cloud-settings-wide.png"))));
    auto *cloudScroll = settingsWindow->findChild<QObject *>(QStringLiteral("settingsScroll")); QVERIFY(cloudScroll);
    cloudPanel->setProperty("tab", 1); QTest::qWait(50);
    QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/cloud-settings-models.png"))));
    cloudPanel->setProperty("tab", 0);
    settingsWindow->resize(760, 520); QTest::qWait(80);
    QVERIFY(cloudPanel->property("compact").toBool());
    QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/cloud-settings-compact.png"))));
    // 按物理分辨率与当前 DPR 计算客户区，覆盖固定底栏与紧凑三标签。
    for (const auto &physical : {QSize(1366, 768), QSize(1920, 1080)}) {
        const auto dpr = settingsWindow->devicePixelRatio();
        const QSize available(qFloor(physical.width() / dpr) - 32, qFloor(physical.height() / dpr) - 64);
        settingsWindow->setMinimumSize(QSize(qMin(760, available.width()), qMin(520, available.height())));
        settingsWindow->resize(qMin(900, available.width()), qMin(680, available.height()));
        for (int tab = 0; tab < 3; ++tab) {
            cloudPanel->setProperty("tab", tab); QTest::qWait(30);
            auto *footerButton = settingsWindow->findChild<QQuickItem *>(QStringLiteral("saveSettingsButton")); QVERIFY(footerButton);
            const auto bottom = footerButton->mapToScene(QPointF(0, footerButton->height())).y();
            QVERIFY(bottom <= settingsWindow->height());
            QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/ai-service-%1-%2-tab%3.png").arg(physical.width()).arg(qRound(dpr * 100)).arg(tab))));
        }
    }
    cloudPanel->setProperty("tab", 0);
    cloud->discard(); cloudPanel->setProperty("accountId", QString()); cloudPanel->setProperty("detail", false);
    QVERIFY(!settingsContent->property("dirty").toBool());

    std::function<QQuickItem *(QQuickItem *)> findQuickMenu = [&](QQuickItem *item) -> QQuickItem * {
        if (item->objectName() == QStringLiteral("cloudQuickSwitch")) return item;
        for (auto *child : item->childItems()) if (auto *found = findQuickMenu(child)) return found;
        return nullptr;
    };
    auto *quickMenu = findQuickMenu(window->contentItem()); QVERIFY(quickMenu);
    QVariant activated;
    QVERIFY(QMetaObject::invokeMethod(quickMenu, "activateChoice", Q_RETURN_ARG(QVariant, activated), Q_ARG(QVariant, QStringLiteral("speechRecognition")), Q_ARG(QVariant, QStringLiteral("local:qwen3"))));
    QVERIFY(activated.toBool());
    QVERIFY(!cloud->dirty());
    bool quickSelected = false;
    for (const auto &value : cloud->bindings()) if (value.toMap().value(QStringLiteral("taskId")) == QStringLiteral("speechRecognition")) quickSelected = value.toMap().value(QStringLiteral("target")) == QStringLiteral("local:qwen3");
    QVERIFY(quickSelected);
    auto *keyField = settingsWindow->findChild<QObject *>(QStringLiteral("fontFamilyField")); QVERIFY(keyField);
    QVERIFY(keyField->setProperty("text", QStringLiteral("仅在表单内的测试值")));
    QVERIFY(settingsContent->property("dirty").toBool());
    std::function<QQuickItem *(QQuickItem *, const QString &)> findSettingControl = [&](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name) return item;
        for (auto *child : item->childItems()) if (auto *found = findSettingControl(child, name)) return found;
        return nullptr;
    };
    for (const int category : {5, 0, 2, 3, 4}) {
        auto *categoryButton = findSettingControl(settingsWindow->contentItem(), QStringLiteral("settingsCategory%1").arg(category));
        QVERIFY(categoryButton); QVERIFY(QMetaObject::invokeMethod(categoryButton, "clicked"));
        QCOMPARE(settingsContent->property("selectedCategory").toInt(), category);
        QCOMPARE(keyField->property("text").toString(), QStringLiteral("仅在表单内的测试值"));
        settingsWindow->resize(760, 520); QTest::qWait(60);
        qInfo() << "settings_category=" << category << "client=" << settingsWindow->size() << "dpr=" << settingsWindow->devicePixelRatio();
        QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/settings-category-%1.png").arg(category))));
    }
    QVERIFY(QMetaObject::invokeMethod(settingsWindow, "requestClose"));
    auto *unsavedSettings = settingsWindow->findChild<QObject *>(QStringLiteral("unsavedSettingsDialog")); QVERIFY(unsavedSettings);
    QTRY_VERIFY(unsavedSettings->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(unsavedSettings, "reject")); QVERIFY(settingsWindow->isVisible());
    QVERIFY(QMetaObject::invokeMethod(settingsWindow, "requestClose"));
    QVERIFY(QMetaObject::invokeMethod(unsavedSettings, "discarded")); QTRY_VERIFY(!settingsWindow->isVisible());
    QVERIFY(QMetaObject::invokeMethod(window, "openSettings"));
    QTRY_COMPARE(settingsLoader->property("status").toInt(), 1);
    settingsContent = settingsLoader->property("item").value<QObject *>(); QVERIFY(settingsContent);
    QTRY_VERIFY(!settingsContent->property("dirty").toBool());
    QVERIFY(!settingsWindow->findChild<QObject *>(QStringLiteral("aiApiKeyField")));
    auto *savedFont = settingsWindow->findChild<QObject *>(QStringLiteral("fontFamilyField")); QVERIFY(savedFont);
    savedFont->setProperty("text", QStringLiteral("Arial"));
    QVariant saveResult; QVERIFY(QMetaObject::invokeMethod(settingsContent, "save", Q_RETURN_ARG(QVariant, saveResult)));
    QVERIFY(saveResult.toBool()); QVERIFY(settingsWindow->isVisible()); QVERIFY(!settingsContent->property("dirty").toBool());
    // 保存成功保留设置窗口；旧路由进入用途绑定，不再创建独立密钥表单。
    QVariant aiRoute(QStringLiteral("ai")), noField{QString()};
    QVERIFY(QMetaObject::invokeMethod(settingsWindow, "route", Q_ARG(QVariant, aiRoute), Q_ARG(QVariant, noField)));
    QCOMPARE(settingsContent->property("selectedCategory").toInt(), 5);
    QCOMPARE(settingsWindow->findChild<QObject *>(QStringLiteral("cloudSettingsPanel"))->property("tab").toInt(), 2);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-workspace-%1.png")
        .arg(QGuiApplication::platformName()))));
    settingsWindow->hide();
    window->requestActivate();
    QTest::qWait(30);
    auto *preview = window->findChild<VideoPreviewItem *>(QStringLiteral("videoPreview"));
    auto *list = window->findChild<QQuickItem *>(QStringLiteral("projectFileList"));
    auto *timeline = window->findChild<TimelineSceneItem *>(QStringLiteral("timelineScene"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("mediaImportDialog"));
    QVERIFY(preview && list && dialog && timeline);

    const QString videoPath = dir.filePath(QStringLiteral("中文 视频 #100%.mp4"));
    QVERIFY(QFile::copy(mediaPath(QStringLiteral("cfr_av.mp4")), videoPath));
    QVERIFY(dialog->setProperty("selectedFile", QUrl::fromLocalFile(videoPath)));
    QVERIFY(QMetaObject::invokeMethod(dialog, "accepted"));
    QVERIFY2(controller.hasVideo(), qPrintable(controller.statusText()));
    QTRY_VERIFY_WITH_TIMEOUT(preview->hasFrame(), 4'000);
    QCOMPARE(controller.projectFiles().size(), 1);
    QCOMPARE(timeline->durationUs(), controller.durationUs());

    auto *navigator = window->findChild<QQuickItem *>(QStringLiteral("timelineZoomBar"));
    QVERIFY(navigator);
    controller.fitTimeline(timeline->width());
    QTest::qWait(30);
    QCOMPARE(navigator->property("startRatio").toDouble(), 0.0);
    QVERIFY(qAbs(navigator->property("endRatio").toDouble() - 1.0) < 1e-9);
    const qint64 navigatorPlayhead = timeline->playheadUs();
    QSignalSpy navigatorSeeks(timeline, &TimelineSceneItem::userSeeked);
    const auto dragNavigator = [&](double from, double to) {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                          navigator->mapToScene(QPointF(from, 6)).toPoint());
        for (int step = 1; step <= 20; ++step) {
            QTest::mouseMove(window, navigator->mapToScene(QPointF(from + (to - from) * step / 20, 6)).toPoint(), 1);
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                            navigator->mapToScene(QPointF(to, 6)).toPoint());
    };
    dragNavigator(navigator->width() - 2, navigator->width() * 0.6);
    QVERIFY(navigator->property("endRatio").toDouble() < 0.65);
    const double navigatorZoom = timeline->pixelsPerMs();
    dragNavigator(navigator->width() * 0.3, navigator->width() * 0.4);
    QCOMPARE(timeline->pixelsPerMs(), navigatorZoom);
    QVERIFY(navigator->property("startRatio").toDouble() > 0.09);
    const double navigatorEnd = navigator->property("endRatio").toDouble();
    dragNavigator(navigator->width() * navigator->property("startRatio").toDouble(), navigator->width() * 0.2);
    QVERIFY(timeline->pixelsPerMs() > navigatorZoom);
    QVERIFY(qAbs(navigator->property("endRatio").toDouble() - navigatorEnd) < 1e-6);
    QCOMPARE(timeline->playheadUs(), navigatorPlayhead);
    QCOMPARE(controller.zoomPercent(), timeline->zoomPercent());
    QCOMPARE(navigatorSeeks.count(), 0);
    timeline->setProperty("followDirection", 1);
    timeline->setProperty("viewportInteracting", true);
    const double manuallyScrolled = timeline->scrollOffset();
    timeline->setPlayheadUs(controller.durationUs() - 1);
    QCOMPARE(timeline->scrollOffset(), manuallyScrolled);
    timeline->setProperty("viewportInteracting", false);
    timeline->setPlayheadUs(controller.durationUs());
    QVERIFY(timeline->scrollOffset() > manuallyScrolled);
    timeline->setProperty("followDirection", 0);
    timeline->setPlayheadUs(navigatorPlayhead);
    const double beforePlus = timeline->pixelsPerMs();
    controller.adjustZoomPercent(10, timeline->width());
    QVERIFY(timeline->pixelsPerMs() > beforePlus);
    QCOMPARE(controller.scrollOffset(), timeline->scrollOffset());
    QCOMPARE(timeline->playheadUs(), navigatorPlayhead);
    controller.fitTimeline(timeline->width());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))), QUrl::fromLocalFile(videoPath)});
    const QPoint dropPosition = preview->mapToScene(QPointF(40, 40)).toPoint();
    QDragEnterEvent enter(dropPosition, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(dropPosition, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(controller.projectFiles().size(), 2);
    QVERIFY(!controller.hasVideo());
    QTest::qWait(100);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, list->mapToScene(QPointF(40, 20)).toPoint());
    QTRY_VERIFY(controller.hasVideo());
    QTRY_VERIFY_WITH_TIMEOUT(preview->hasFrame(), 4'000);

    auto *seekSlider = window->findChild<QQuickItem *>(QStringLiteral("monitorSeek"));
    auto *playPause = window->findChild<QQuickItem *>(QStringLiteral("playPause"));
    auto *stepForward = window->findChild<QQuickItem *>(QStringLiteral("stepForward"));
    QVERIFY(seekSlider && playPause && stepForward);
    controller.seek(0);
    controller.togglePlay();
    QVERIFY(controller.playing());
    controller.seek(100);
    QVERIFY(controller.playing()); // 普通跳转保留播放状态。
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, timeline->mapToScene(QPointF(50, 30)).toPoint());
    QVERIFY(!controller.playing());
    controller.seek(0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, playPause->mapToScene(QPointF(15, 15)).toPoint());
    QVERIFY(controller.playing());
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, seekSlider->mapToScene(QPointF(seekSlider->width() / 2, 11)).toPoint());
    QVERIFY(!controller.playing());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, seekSlider->mapToScene(QPointF(seekSlider->width() / 2, 11)).toPoint());
    QVERIFY(qAbs(controller.positionMs() - controller.durationMs() / 2) < 100);
    controller.seek(0);
    QList<qint64> stepPositions;
    const auto stepConnection = connect(&controller, &AppController::positionChanged, &controller, [&] {
        stepPositions.append(controller.positionMs());
    });
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, stepForward->mapToScene(QPointF(15, 15)).toPoint());
    disconnect(stepConnection);
    QVERIFY(!controller.playing());
    // QTest 可处理随后的音频时钟校正；这里锁定逐帧请求发出的精确位置。
    QVERIFY(stepPositions.contains(controller.frameDeltaMs(1)));

    Subtitle editable;
    editable.start = MediaTime::fromMilliseconds(200);
    editable.end = MediaTime::fromMilliseconds(700);
    editable.text = QStringLiteral("待人工检查");
    editable.status = QStringLiteral("LOW_CONFIDENCE");
    controller.applySubtitles({editable});
    controller.toggleSnap();
    controller.fitTimeline(timeline->width());
    QTest::qWait(100); // 等待上一笔逐帧 Seek 完成，避免把时钟校正当作拖动 Seek。
    const qint64 beforeEditPosition = controller.positionUs();
    const auto dragCue = [&](double fromMs, double toMs) {
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
            timeline->mapToScene(QPointF(fromMs * timeline->pixelsPerMs(), 48)).toPoint());
        QTest::mouseMove(window,
            timeline->mapToScene(QPointF(toMs * timeline->pixelsPerMs(), 48)).toPoint());
        QCOMPARE(controller.commands()->stack()->count(), 0);
        QCOMPARE(controller.document()->subtitles().first().start, editable.start);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
            timeline->mapToScene(QPointF(toMs * timeline->pixelsPerMs(), 48)).toPoint());
        QCOMPARE(controller.commands()->stack()->count(), 1);
    };
    dragCue(450, 650);
    QCOMPARE(controller.selectedCue(), 0);
    QCOMPARE(controller.positionUs(), beforeEditPosition);
    QCOMPARE(controller.document()->subtitles().first().end.microseconds() - controller.document()->subtitles().first().start.microseconds(), qint64(500'000));
    QVERIFY(controller.document()->subtitles().first().start > editable.start);
    controller.undo();
    QCOMPARE(controller.document()->subtitles().first().start, editable.start);
    controller.commands()->clear();
    dragCue(201, 350);
    QCOMPARE(controller.document()->subtitles().first().end, editable.end);
    QVERIFY(controller.document()->subtitles().first().start > editable.start);
    controller.undo();
    controller.commands()->clear();
    dragCue(699, 550);
    QCOMPARE(controller.document()->subtitles().first().start, editable.start);
    QVERIFY(controller.document()->subtitles().first().end < editable.end);
    controller.undo();
    QSignalSpy editRequest(&controller, &AppController::editCueRequested);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier,
        timeline->mapToScene(QPointF(450 * timeline->pixelsPerMs(), 48)).toPoint());
    QCOMPARE(editRequest.count(), 1);
    auto *cuePopup = window->findChild<QObject *>(QStringLiteral("cueEditorPopup"));
    QVERIFY(cuePopup);
    QTRY_VERIFY(cuePopup->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!cuePopup->property("visible").toBool());
    controller.setCueText(0, QStringLiteral("人工修正"));
    QCOMPARE(controller.document()->subtitles().first().text, QStringLiteral("人工修正"));
    controller.undo();
    QCOMPARE(controller.document()->subtitles().first().text, editable.text);
    controller.redo();
    QCOMPARE(controller.document()->subtitles().first().text, QStringLiteral("人工修正"));

    for (const QSize size : {QSize(1440, 900), QSize(1100, 700)}) {
        window->resize(size);
        QTest::qWait(150);
        QVERIFY(preview->width() >= 300);
        QVERIFY(list->height() > 100);
        QVERIFY(list->mapToScene(QPointF(0, list->height())).y() <= window->height());
        const QImage screenshot = window->grabWindow();
        QVERIFY(!screenshot.isNull());
        QVERIFY(screenshot.save(sourcePath(QStringLiteral(".test_tmp/transport-ui-%1-%2.png")
            .arg(QGuiApplication::platformName()).arg(size.width()))));
    }
    // 用三分钟 PCM 媒体验证实际播放中的翻页和 Navigator 操作，不调用外部转码程序。
    QFile longAudio(dir.filePath(QStringLiteral("timeline-180s.wav")));
    QVERIFY(longAudio.open(QIODevice::WriteOnly));
    QDataStream wav(&longAudio);
    wav.setByteOrder(QDataStream::LittleEndian);
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + 16000 * 180 * 2);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << quint32(16000)
        << quint32(32000) << quint16(2) << quint16(16);
    wav.writeRawData("data", 4);
    wav << quint32(16000 * 180 * 2);
    longAudio.write(QByteArray(16000 * 180 * 2, '\0'));
    longAudio.close();
    controller.loadMediaPath(longAudio.fileName());
    QCOMPARE(controller.durationMs(), qint64(180'000));
    timeline->setVisibleRange(0, 0.10);
    controller.seek(16'300);
    controller.playForward();
    // 前面的属性直写会覆盖 QML 绑定；真实播放段显式同步控制器方向。
    timeline->setProperty("followDirection", controller.direction());
    QTRY_VERIFY_WITH_TIMEOUT(timeline->scrollOffset() > 0, 3'000);
    QVERIFY(controller.playing());
    const int seeksBeforeNavigation = navigatorSeeks.count();
    dragNavigator(navigator->property("rangeX").toDouble() + navigator->property("rangeWidth").toDouble() / 2,
        navigator->width() * 0.5);
    QVERIFY(controller.playing());
    QCOMPARE(navigatorSeeks.count(), seeksBeforeNavigation);
    controller.stop();
    Subtitle dropped;
    dropped.text = QStringLiteral("拖放定位测试");
    dropped.status = QStringLiteral("SKIPPED_NO_AUDIO");
    controller.applySubtitles({dropped});
    timeline->setVisibleRange(0, 0.02);
    QTest::qWait(50);
    auto *subtitleList = window->findChild<QQuickItem *>(QStringLiteral("subtitleList"));
    QVERIFY(subtitleList);
    QQuickItem *dropRow = nullptr;
    QVERIFY(QMetaObject::invokeMethod(subtitleList, "itemAtIndex",
        Q_RETURN_ARG(QQuickItem *, dropRow), Q_ARG(int, 0)));
    QVERIFY(dropRow);
    const QPoint dropFrom = dropRow->mapToScene(QPointF(70, 10)).toPoint();
    const QPoint dropTo = timeline->mapToScene(QPointF(500 * timeline->pixelsPerMs(), 70)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, dropFrom);
    for (int step = 1; step <= 20; ++step)
        QTest::mouseMove(window, dropFrom + (dropTo - dropFrom) * step / 20, 2);
    auto *dropMouse = dropRow->findChild<QObject *>(QStringLiteral("subtitleRowMouse"));
    QVERIFY(dropMouse);
    QVERIFY(dropMouse->property("pressed").toBool());
    QVERIFY(dropMouse->property("placing").toBool());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dropTo);
    QVERIFY(controller.document()->subtitles().first().isTimed());
    QVERIFY(qAbs(controller.document()->subtitles().first().start.milliseconds() - 500) < 10);
    controller.undo();
    QVERIFY(!controller.document()->subtitles().first().isTimed());
    controller.applySubtitles({editable});
    controller.confirmCue(0);
    QCOMPARE(controller.document()->subtitles().first().status, QStringLiteral("MANUAL"));
    controller.undo();
    QCOMPARE(controller.document()->subtitles().first().status, QStringLiteral("LOW_CONFIDENCE"));
    timeline->setVisibleRange(0, 0.02);
    QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/timeline-editor-%1.png")
        .arg(QGuiApplication::platformName()))));
    FakeAsrService waitingAsr;
    waitingAsr.blockUntilCancel = true;
    controller.setAlignmentOverrides(&waitingAsr);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    context->settings.insert(QStringLiteral("aiAssistEnabled"), false);
    controller.setScriptText(QStringLiteral("等待识别"));
    controller.startAlignment();
    QVERIFY(controller.busy());
    auto *progressArea = window->findChild<QQuickItem *>(QStringLiteral("alignmentProgressDialog"));
    auto *progressBar = window->findChild<QQuickItem *>(QStringLiteral("alignmentProgressBar"));
    auto *progressFill = window->findChild<QQuickItem *>(QStringLiteral("alignmentProgressFill"));
    QVERIFY(progressArea && progressBar && progressFill);
    QVERIFY(progressArea->isVisible());
    QVERIFY(progressBar->property("indeterminate").toBool());
    QTRY_COMPARE(controller.alignmentStage(), QStringLiteral("ASR"));
    QVERIFY(!progressBar->property("indeterminate").toBool());
    progressBar->setProperty("value", 50);
    QTRY_VERIFY(progressFill->width() >= progressBar->width() * 0.49);
    window->resize(1440, 900);
    QTest::qWait(50);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/timeline-progress-%1.png")
        .arg(QGuiApplication::platformName()))));
    controller.cancelAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 3'000);
    QVERIFY(!progressArea->isVisible());
    QCOMPARE(controller.document()->subtitles().first().id, editable.id);
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    window->close();
}

void EditorIntegrationTests::exportResultUsesIncrementingSubtitleFolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AlignmentResult result;
    Subtitle cue;
    cue.text = QStringLiteral("测试");
    cue.start = MediaTime::fromMilliseconds(0);
    cue.end = MediaTime::fromMilliseconds(1'000);
    cue.status = QStringLiteral("MATCHED");
    result.subtitles.append(cue);
    MediaInfo media;
    media.videoStreamIndex = 0;
    MediaStreamInfo stream;
    stream.type = MediaStreamType::Video;
    stream.width = 1920;
    stream.height = 1080;
    media.streams.append(stream);

    const QString mediaFile = dir.filePath(QStringLiteral("_collision_media.mp4"));
    QJsonObject settings{
        {QStringLiteral("outputDirectory"), dir.path()},
        {QStringLiteral("outputSrt"), true},
        {QStringLiteral("outputAss"), true},
        {QStringLiteral("fontFamily"), QStringLiteral("Microsoft YaHei")},
        {QStringLiteral("fontSize1080p"), 52},
        {QStringLiteral("bottomMargin1080p"), 90},
        {QStringLiteral("alignment"), QStringLiteral("bottom-center")},
    };

    const auto first = AlignmentPipeline::exportResult(mediaFile, result, media, settings);
    QVERIFY2(std::holds_alternative<QStringList>(first), "first export should succeed");
    const auto second = AlignmentPipeline::exportResult(mediaFile, result, media, settings);
    QVERIFY2(std::holds_alternative<QStringList>(second), "second export should succeed");

    const QStringList firstPaths = std::get<QStringList>(first);
    const QStringList secondPaths = std::get<QStringList>(second);
    QCOMPARE(firstPaths.size(), 2);
    QCOMPARE(QFileInfo(firstPaths.at(0)).dir().dirName(), QStringLiteral("_collision_media_字幕文件"));
    QCOMPARE(QFileInfo(firstPaths.at(0)).fileName(), QStringLiteral("_collision_media.srt"));
    QCOMPARE(QFileInfo(firstPaths.at(1)).fileName(), QStringLiteral("_collision_media.ass"));
    QCOMPARE(QFileInfo(secondPaths.at(0)).dir().dirName(), QStringLiteral("_collision_media_字幕文件_1"));
}

void EditorIntegrationTests::pipelineAlignsWithInjectedAsrOnWorkerThread()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputAss"), false);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());

    FakeAsrService asr;
    asr.transcript.words = {
        {1, QStringLiteral("Hello"), 100, 700},
        {2, QStringLiteral("world"), 700, 1'400},
    };

    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY2(controller.hasMedia(), qPrintable(controller.statusText()));
    controller.setScriptText(QStringLiteral("Hello\nworld"));

    const Qt::HANDLE guiThread = QThread::currentThreadId();
    bool sawChunks = false;
    connect(&controller, &AppController::alignmentProgressChanged, &controller, [&] {
        QVERIFY(QThread::currentThreadId() == guiThread);
        if (controller.alignmentStage() == QStringLiteral("ASR")) {
            sawChunks = true;
            QCOMPARE(controller.alignmentCompleted(), 1);
            QCOMPARE(controller.alignmentTotal(), 1);
            QVERIFY(!controller.alignmentIndeterminate());
        }
    });
    QElapsedTimer responseTimer;
    responseTimer.start();
    controller.startAlignment();
    QVERIFY(responseTimer.elapsed() < 200);
    QVERIFY(controller.busy());
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(asr.calls >= 1);
    QVERIFY(asr.workerThreadId != nullptr);
    QVERIFY(asr.workerThreadId != guiThread);
    QVERIFY(sawChunks);
    QCOMPARE(controller.document()->count(), 2);
    QVERIFY(controller.document()->subtitles().at(0).isTimed());
    QVERIFY2(controller.statusText().contains(QStringLiteral("打轴完成")),
             qPrintable(controller.statusText()));
    QVERIFY(controller.canExport());
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("cfr_av_字幕文件/cfr_av.srt"))));
    controller.exportSubtitles();
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("cfr_av_字幕文件/cfr_av.srt"))));
    asr.transcript.words.clear();
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(controller.document()->count(), 2);
    QVERIFY(!controller.document()->subtitles().first().isTimed());
    QCOMPARE(controller.document()->subtitles().first().metadata.value(QStringLiteral("diagnosticReason")).toString(),
        QStringLiteral("ASR_NO_WORDS"));
    controller.locateCueAt(controller.document()->subtitles().first().id, 100);
    QVERIFY(controller.document()->subtitles().first().isTimed());
    controller.undo();
    QVERIFY(!controller.document()->subtitles().first().isTimed());
}

void EditorIntegrationTests::alignmentAndAsrReviewDoNotCallAi()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    RecordingHttpClient http;
    QVERIFY(configureUnifiedReview(context.get()));
    context->aiHttp = &http;
    FakeAsrService asr;
    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};
    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(http.sendCount, 0);
    controller.startReview();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(http.sendCount, 0);
    QVERIFY(controller.canOmniReview());
}

void EditorIntegrationTests::wordMappingReviewIsUndoableAndRejectsStaleEvidence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    RecordingHttpClient http;
    QVERIFY(configureUnifiedReview(context.get()));
    context->aiHttp = &http;
    FakeAsrService asr;
    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    controller.startWordMappingReview();
    QCOMPARE(controller.statusText(), QStringLiteral("缺少有效词级证据，请先完成打轴。"));
    QCOMPARE(http.sendCount, 0);

    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    Subtitle cue = controller.document()->subtitles().at(0);
    const qint64 originalStart = cue.start.milliseconds();
    cue.confidence = 0.2;
    QVERIFY(controller.commands()->replaceMany({cue}, QStringLiteral("降低置信度")));
    http.responses.append({200, {}, jsonChatBody(QJsonObject{{QStringLiteral("mappings"), QJsonArray{QJsonObject{
        {QStringLiteral("line_id"), QStringLiteral("L1")},
        {QStringLiteral("status"), QStringLiteral("matched")},
        {QStringLiteral("start_word_id"), QStringLiteral("W1")},
        {QStringLiteral("end_word_id"), QStringLiteral("W1")},
        {QStringLiteral("confidence"), 0.95},
    }}}})});
    controller.startWordMappingReview();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(http.sendCount >= 1);
    QCOMPARE(http.requests.first().headers.front().value, QByteArrayLiteral("Bearer unified-review-test-key"));
    QCOMPARE(QJsonDocument::fromJson(http.requests.first().body).object().value(QStringLiteral("model")).toString(), QStringLiteral("qwen-plus"));
    QVERIFY(!http.requests.first().body.contains("enable_thinking"));
    QCOMPARE(controller.document()->subtitles().at(0).start.milliseconds(), 100);
    QVERIFY(controller.commands()->canUndo());
    controller.undo();
    QCOMPARE(controller.document()->subtitles().at(0).start.milliseconds(), originalStart);
}

void EditorIntegrationTests::subtitleContentReviewRequiresAccept()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    RecordingHttpClient http;
    QVERIFY(configureUnifiedReview(context.get()));
    context->aiHttp = &http;
    FakeAsrService asr;
    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};
    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    const QString id = controller.document()->subtitles().at(0).id;
    http.responses.append({200, {}, jsonChatBody(QJsonObject{{QStringLiteral("results"), QJsonArray{QJsonObject{
        {QStringLiteral("segment_id"), id},
        {QStringLiteral("issue_type"), QStringLiteral("MISSING_TEXT")},
        {QStringLiteral("confidence"), 0.94},
        {QStringLiteral("original_text"), QStringLiteral("Hello")},
        {QStringLiteral("suggested_text"), QStringLiteral("Hello world")},
        {QStringLiteral("reason"), QStringLiteral("漏字")},
        {QStringLiteral("decision"), QStringLiteral("REPLACE_TEXT")},
    }}}})});
    controller.startOmniSubtitleReview();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(http.sendCount >= 1);
    QCOMPARE(http.requests.first().headers.front().value, QByteArrayLiteral("Bearer unified-review-test-key"));
    QCOMPARE(QJsonDocument::fromJson(http.requests.first().body).object().value(QStringLiteral("model")).toString(), QStringLiteral("qwen3.8-omni-flash"));
    QCOMPARE(controller.document()->subtitles().at(0).text, QStringLiteral("Hello"));
    QCOMPARE(controller.document()->subtitles().at(0).metadata.value(
        QStringLiteral("omniReviewStatus")).toString(), QStringLiteral("suggested"));
    QVERIFY(controller.statusText().contains(QStringLiteral("qwen3.8-omni-flash")));
    QVERIFY(controller.statusText().contains(QStringLiteral("token")));
    QVERIFY(controller.statusText().contains(QStringLiteral("¥")));
    controller.acceptOmniSubtitleSuggestion(0);
    QCOMPARE(controller.document()->subtitles().at(0).text, QStringLiteral("Hello world"));
    controller.undo();
    QCOMPARE(controller.document()->subtitles().at(0).text, QStringLiteral("Hello"));
}

void EditorIntegrationTests::omniReviewStartsAtZeroProgress()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    RecordingHttpClient http;
    http.blockUntilCancel = true;
    QVERIFY(configureUnifiedReview(context.get()));
    context->aiHttp = &http;
    FakeAsrService asr;
    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};
    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(controller.alignmentProgress() >= 0);
    controller.startOmniSubtitleReview();
    QTRY_VERIFY(controller.busy());
    QCOMPARE(controller.busyTaskTitle(), QStringLiteral("正在复核字幕内容"));
    QCOMPARE(controller.alignmentProgress(), 0);
    QVERIFY(!controller.alignmentProgressText().contains(QStringLiteral("完成。")));
    QVERIFY(controller.alignmentIndeterminate() || controller.alignmentProgress() < 100);
    QTRY_VERIFY(http.inFlight.load());
    QString key, error; const auto snapshot = context->cloudConfig->resolveTask(QStringLiteral("subtitleAudioReview"), &key, &error);
    CloudSettingsController cloud(context.get()); cloud.begin();
    cloud.removeKey(snapshot.value(QStringLiteral("account")).toObject().value(QStringLiteral("id")).toString());
    QVERIFY2(cloud.save(), qPrintable(cloud.status())); QVERIFY(!http.inFlight.load());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 4'000);
}

void EditorIntegrationTests::destroyingControllerCancelsAiReview()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    RecordingHttpClient http;
    http.blockUntilCancel = true;
    QVERIFY(configureUnifiedReview(context.get()));
    context->aiHttp = &http;
    FakeAsrService asr;
    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};
    {
        AppController controller(context.get());
        controller.setAlignmentOverrides(&asr);
        controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
        controller.setScriptText(QStringLiteral("Hello"));
        controller.startAlignment();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
        controller.startOmniSubtitleReview();
        QTRY_VERIFY_WITH_TIMEOUT(http.inFlight.load(), 4'000);
        controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
        QVERIFY(!controller.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!http.inFlight.load(), 4'000);
        QVERIFY(controller.statusText().startsWith(QStringLiteral("已加载")));
    }
    QVERIFY(!http.inFlight.load());
}

void EditorIntegrationTests::qmlUnifiedAiSettingsAndWorkspaceScreenshots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("SubCue")));
    const QString modulePath = dir.filePath(QStringLiteral("SubCue"));
    QByteArray manifest("module SubCue\n");
    const QStringList qmlFiles = QDir(sourcePath(QStringLiteral("src/app/qml"))).entryList(
        {QStringLiteral("*.qml")}, QDir::Files);
    for (const QString &file : qmlFiles) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/") + file),
                            QDir(modulePath).filePath(file)));
        if (file == QStringLiteral("Theme.qml")) manifest += "singleton ";
        manifest += QFileInfo(file).baseName().toUtf8() + " 1.0 " + file.toUtf8() + "\n";
    }
    QVERIFY(QDir(modulePath).mkdir(QStringLiteral("icons")));
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList(
             {QStringLiteral("*.svg"), QStringLiteral("*.png")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file),
                            QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    }
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly));
    QCOMPARE(qmldir.write(manifest), manifest.size());
    qmldir.close();
    registerSubCueQmlItems();
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputAss"), false);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    WorkspaceRouter router(&editor, &roughCut);
    QStringList warnings;
    QQmlApplicationEngine engine;
    engine.addImportPath(dir.path());
    QQmlComponent nativeTheme(&engine);
    nativeTheme.setData("import QtQuick; QtObject { property bool expandedClientArea: false; function applyDarkTitleBar() {} }", QUrl());
    QObject *theme = nativeTheme.create();
    QVERIFY(theme);
    theme->setParent(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &editor);
    engine.rootContext()->setContextProperty(QStringLiteral("roughCut"), &roughCut);
    engine.rootContext()->setContextProperty(QStringLiteral("appRouter"), &router);
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(QDir(modulePath).filePath(QStringLiteral("AppRouter.qml"))));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    QVERIFY(window);
    QTest::qWait(250);
    QVERIFY(window->findChild<QObject *>(QStringLiteral("subtitleWorkspace")));
    QVERIFY(window->findChild<QObject *>(QStringLiteral("aiReviewMenuButton")));
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-workspace-router-%1.png")
        .arg(QGuiApplication::platformName()))));
    auto *importWorkspace = window->findChild<QObject *>(QStringLiteral("subtitleWorkspace"));
    auto *importWarning = window->findChild<QObject *>(QStringLiteral("roughCutUnsavedDialog"));
    QVERIFY(importWorkspace && importWarning);
    engine.globalObject().setProperty(QStringLiteral("importGuardCalls"), 0);
    const auto checkImportGuard = [&](const QStringList &files, bool timed, bool destructive) {
        const int before = engine.globalObject().property(QStringLiteral("importGuardCalls")).toInt();
        const auto result = engine.newQObject(importWorkspace).property(QStringLiteral("requestSubtitleLeave")).call({
            engine.evaluate(QStringLiteral("(function() { importGuardCalls++; })")),
            engine.toScriptValue(files), QJSValue(timed)});
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QTRY_COMPARE(importWarning->property("visible").toBool(), destructive);
        QCOMPARE(engine.globalObject().property(QStringLiteral("importGuardCalls")).toInt(), before + (destructive ? 0 : 1));
        if (destructive) {
            QVERIFY(QMetaObject::invokeMethod(importWarning, "reject"));
            QTRY_VERIFY(!importWarning->property("visible").toBool());
        }
    };
    // 互补导入不离开工程，覆盖已有文稿、媒体字幕或时间码字幕仍可取消。
    editor.setScriptText(QStringLiteral("先导入文稿"));
    checkImportGuard({mediaPath(QStringLiteral("audio.wav"))}, false, false);
    editor.newProject();
    editor.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!editor.busy(), 8'000);
    checkImportGuard({dir.filePath(QStringLiteral("first.txt"))}, false, false);
    checkImportGuard({mediaPath(QStringLiteral("cfr_av.mp4"))}, false, false);
    editor.setScriptText(QStringLiteral("已有文稿"));
    checkImportGuard({dir.filePath(QStringLiteral("replacement.docx"))}, false, true);
    Subtitle guardedCue;
    guardedCue.id = QStringLiteral("guarded-cue"); guardedCue.text = QStringLiteral("保留已有字幕");
    editor.applySubtitles({guardedCue});
    checkImportGuard({mediaPath(QStringLiteral("audio.wav"))}, false, false);
    checkImportGuard({mediaPath(QStringLiteral("cfr_av.mp4"))}, false, true);
    checkImportGuard({dir.filePath(QStringLiteral("replacement.srt"))}, true, true);
    checkImportGuard({mediaPath(QStringLiteral("audio.wav")), mediaPath(QStringLiteral("cfr_av.mp4"))}, false, false);
    QCOMPARE(editor.document()->subtitles().first().id, guardedCue.id);
    QCOMPARE(editor.scriptText(), QStringLiteral("已有文稿"));
    editor.newProject();
    editor.applySubtitles({guardedCue});
    checkImportGuard({mediaPath(QStringLiteral("audio.wav"))}, false, false);
    checkImportGuard({dir.filePath(QStringLiteral("replacement.srt"))}, true, true);
    editor.newProject();
    editor.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!editor.busy(), 8'000);
    Subtitle first;
    first.id = QStringLiteral("timeline-a"); first.text = QStringLiteral("时间轴优先：正文与时间可以一次应用，长文字可以查看完整内容。 ").repeated(3);
    first.start = MediaTime::fromMicroseconds(100'001); first.end = MediaTime::fromMicroseconds(700'001);
    first.status = QStringLiteral("LOW_CONFIDENCE");
    Subtitle second = first;
    second.id = QStringLiteral("timeline-b"); second.text = QStringLiteral("重叠字幕独立显示，工程可以保留编辑结果");
    second.start = MediaTime::fromMicroseconds(300'001); second.end = MediaTime::fromMicroseconds(800'001);
    second.status = QStringLiteral("REVIEW");
    QList<Subtitle> listSample{first, second};
    for (int index = 0; index < 20; ++index) {
        Subtitle cue;
        cue.id = QStringLiteral("list-%1").arg(index);
        cue.text = QStringList{QStringLiteral("列表优先，连续浏览多条字幕"), QStringLiteral("编号和起止时间固定在左侧"), QStringLiteral("双击正文或按 Enter 打开编辑窗"), QStringLiteral("重叠提示保留在对应字幕行")}.at(index % 4);
        cue.status = QStringLiteral("UNMATCHED");
        listSample.append(cue);
    }
    editor.applySubtitles(listSample);
    auto *subtitleList = window->findChild<QObject *>(QStringLiteral("subtitleList"));
    QVERIFY(subtitleList);
    QTRY_COMPARE(subtitleList->property("currentIndex").toInt(), 0);
    QTest::qWait(80);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-list-priority-%1.png").arg(QGuiApplication::platformName()))));
    QFile timedInput(dir.filePath(QStringLiteral("imported.srt")));
    QVERIFY(timedInput.open(QIODevice::WriteOnly));
    QVERIFY(timedInput.write("1\n00:00:00,100 --> 00:00:00,400\nImported cue\n") > 0);
    timedInput.close();
    editor.importTimedSubtitles(QUrl::fromLocalFile(timedInput.fileName()));
    QTRY_COMPARE(subtitleList->property("currentIndex").toInt(), 0);
    QCOMPARE(editor.selectedCueId(), editor.document()->subtitles().first().id);
    editor.applySubtitles({first, second});
    editor.selectCue(0, false);
    auto *timeline = window->findChild<TimelineSceneItem *>(QStringLiteral("timelineScene"));
    auto *textEditor = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("cueTextEditor")));
    auto *startEditor = window->findChild<QObject *>(QStringLiteral("cueStartTime"));
    auto *workspace = window->findChild<QObject *>(QStringLiteral("subtitleWorkspace"));
    QVERIFY(timeline && textEditor && startEditor && workspace);
    QVERIFY(!textEditor->isVisible());
    QVERIFY(!textEditor->hasActiveFocus());
    auto *cueDialog = window->findChild<QObject *>(QStringLiteral("cueEditDialog"));
    auto *monitorPanel = window->findChild<QQuickItem *>(QStringLiteral("subtitleMonitorPanel"));
    QVERIFY(cueDialog && monitorPanel);
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("persistentCueInspector")));
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("toggleCueInspector")));
    QVERIFY(monitorPanel->width() >= 360);
    auto *textHost = textEditor->parentItem();
    // 筛选和选择会销毁列表委托，编辑器必须留在隐藏弹窗中，不占用字幕列表空间。
    for (int index = 0; index < 20; ++index) {
        editor.selectCue(index % 2, false);
        editor.filteredSubtitleModel()->setProperty("searchText", QStringLiteral("不存在的字幕"));
        QCoreApplication::processEvents();
        QCOMPARE(subtitleList->property("count").toInt(), 0);
        QCOMPARE(window->findChild<QQuickItem *>(QStringLiteral("cueTextEditor")), textEditor);
        QCOMPARE(textEditor->parentItem(), textHost);
        QVERIFY(!textEditor->isVisible());
        editor.filteredSubtitleModel()->setProperty("searchText", QString());
        QCoreApplication::processEvents();
    }
    editor.selectCue(0, false);
    const auto listHeight = qobject_cast<QQuickItem *>(subtitleList)->height();
    editor.createOrEditCue();
    QTRY_VERIFY(cueDialog->property("visible").toBool() && textEditor->isVisible());
    QCOMPARE(textEditor->parentItem(), textHost);
    textEditor->setProperty("text", QStringLiteral("第一行字幕\n第二行字幕\n第三行字幕"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "commitPendingCue"));
    QCOMPARE(editor.document()->subtitles().first().text, QStringLiteral("第一行字幕\n第二行字幕\n第三行字幕"));
    QVERIFY(QMetaObject::invokeMethod(cueDialog, "close"));
    QTRY_VERIFY(!textEditor->isVisible());
    QCOMPARE(qobject_cast<QQuickItem *>(subtitleList)->height(), listHeight);
    editor.createOrEditCue();
    QTRY_VERIFY(textEditor->isVisible());
    QCOMPARE(textEditor->parentItem(), textHost);
    QCOMPARE(textEditor->property("text").toString(), QStringLiteral("第一行字幕\n第二行字幕\n第三行字幕"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "discardDraft"));
    QTRY_VERIFY(!textEditor->isVisible());
    editor.undo();
    QCOMPARE(editor.document()->subtitles().first().text, first.text);
    QVERIFY(!editor.property("canUndo").toBool());
    editor.fitTimeline(timeline->width());
    QTest::qWait(80);
    QCOMPARE(editor.overlappingCueCount(), 2);
    QCOMPARE(timeline->visibleCueCount(), 2);
    QSignalSpy dragging(timeline, &TimelineSceneItem::cueDragStarted);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        timeline->mapToScene(QPointF(timeline->width() * 0.2, 48)).toPoint());
    QCOMPARE(dragging.count(), 0);
    QVERIFY(!editor.property("canUndo").toBool());
    const QPoint dragStart = timeline->mapToScene(QPointF(timeline->width() * 0.25, 48)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, dragStart);
    QTest::mouseMove(window, dragStart + QPoint{40, 0}, 30);
    QVERIFY(dragging.count() > 0);
    QVERIFY(!timeline->dragPreview().isEmpty());
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragStart + QPoint{40, 0});
    QVERIFY(timeline->dragPreview().isEmpty());
    QVERIFY(!editor.property("canUndo").toBool());
    QTest::qWait(50);
    const QVariantMap builds = timeline->renderStats();
    timeline->setFollowDirection(0);
    for (qint64 position : {qint64{200'000}, qint64{2'000'000}, qint64{400'000}}) {
        timeline->setPlayheadUs(position);
        QTest::qWait(30);
    }
    QCOMPARE(timeline->renderStats(), builds);
    editor.seek(400);
    editor.selectCue(0, false);
    editor.createOrEditCue();
    QTest::qWait(30);
    textEditor->forceActiveFocus(Qt::OtherFocusReason);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-edit-dialog-%1.png").arg(QGuiApplication::platformName()))));
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const char key : QByteArray("atomic edit")) QTest::keyClick(window, key);
    QCOMPARE(editor.subtitleModel()->property("count").toInt(), 2);
    QCOMPARE(editor.currentSubtitleText().contains(QStringLiteral("atomic edit")), false);
    QCOMPARE(textEditor->property("text").toString(), QStringLiteral("atomic edit"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(editor.currentSubtitleText().contains(QStringLiteral("atomic edit")), true);
    editor.undo();
    QCOMPARE(editor.currentSubtitleText().contains(first.text), true);
    QCOMPARE(editor.subtitleModel()->property("count").toInt(), 2);
    editor.createOrEditCue();
    QTest::qWait(30);
    textEditor->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const char key : QByteArray("unapplied")) QTest::keyClick(window, key);
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(!workspace->property("draftChanged").toBool());
    QCOMPARE(editor.currentSubtitleText().contains(first.text), true);
    editor.createOrEditCue();
    QTest::qWait(30);
    textEditor->forceActiveFocus(Qt::OtherFocusReason);
    QInputMethodEvent composing(QStringLiteral("拼音候选"), {});
    QCoreApplication::sendEvent(textEditor, &composing);
    QVERIFY(textEditor->property("inputMethodComposing").toBool());
    QTest::keyClick(window, Qt::Key_Return);
    QVERIFY(!editor.property("canUndo").toBool());
    QInputMethodEvent completed;
    QCoreApplication::sendEvent(textEditor, &completed);
    QTest::keyClick(window, Qt::Key_Escape);
    editor.createOrEditCue();
    QTest::qWait(30);
    startEditor->setProperty("text", QStringLiteral("invalid"));
    QVERIFY(QMetaObject::invokeMethod(workspace, "commitPendingCue"));
    timeline->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(router.workspace(), QStringLiteral("subtitle"));
    QVERIFY(!workspace->property("draftError").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(workspace, "discardDraft"));
    timeline->forceActiveFocus(Qt::OtherFocusReason);
    QTest::keyClick(window, Qt::Key_F1);
    QTest::qWait(30);
    QTest::keyClick(window, Qt::Key_Delete);
    QCOMPARE(editor.subtitleModel()->property("count").toInt(), 2);
    QTest::keyClick(window, Qt::Key_Escape);
    window->setMinimumSize(QSize{880, 500});
    for (const QSize size : {QSize{1440, 900}, QSize{1100, 700}, QSize{1366, 768}, QSize{1092, 614}, QSize{911, 512}}) {
        window->resize(size);
        editor.fitTimeline(timeline->width());
        editor.seek(400);
        QTest::qWait(80);
        auto *contentEditor = window->findChild<QQuickItem *>(QStringLiteral("cueTextScroll")); QVERIFY(contentEditor);
        auto *listItem = qobject_cast<QQuickItem *>(subtitleList); QVERIFY(listItem);
        const auto listBounds = listItem->mapRectToScene(listItem->boundingRect());
        const auto panelBounds = listItem->parentItem()->mapRectToScene(listItem->parentItem()->boundingRect());
        QVERIFY(!contentEditor->isVisible());
        QVERIFY(!cueDialog->property("visible").toBool());
        QVERIFY(qAbs(listBounds.bottom() - panelBounds.bottom()) <= 1);
        QCOMPARE(textEditor->parentItem(), textHost);
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-overlap-%1-%2x%3.png")
            .arg(QGuiApplication::platformName()).arg(size.width()).arg(size.height()))));
    }
    window->resize(1440, 900);
    QTest::qWait(50);
    Subtitle plainFirst = first;
    plainFirst.end = MediaTime::fromMicroseconds(400'001);
    Subtitle plainSecond = second;
    plainSecond.start = MediaTime::fromMicroseconds(500'001);
    editor.applySubtitles({plainFirst, plainSecond});
    const double stableHeight = timeline->subtitleViewportHeight();
    plainFirst.start = MediaTime::fromMicroseconds(150'001);
    plainFirst.end = MediaTime::fromMicroseconds(450'001);
    timeline->setPreviewCue(plainFirst);
    QCOMPARE(timeline->subtitleViewportHeight(), stableHeight);
    plainFirst.end = MediaTime::fromMicroseconds(650'001);
    timeline->setPreviewCue(plainFirst);
    QVERIFY(timeline->subtitleViewportHeight() > stableHeight);
    timeline->setPreviewCue(std::nullopt);
    QCOMPARE(timeline->subtitleViewportHeight(), stableHeight);
    QSignalSpy exported(&editor, &AppController::exportFinished);
    QVERIFY(QMetaObject::invokeMethod(workspace, "requestExport"));
    QTest::qWait(50);
    auto *exportButton = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("subtitleExportConfirm")));
    QVERIFY(exportButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        exportButton->mapToScene(QPointF(exportButton->width() / 2, exportButton->height() / 2)).toPoint());
    QTRY_COMPARE(exported.count(), 1);
    QVERIFY(exported.first().first().toBool());
    QCOMPARE(QDir(dir.path()).entryList({QStringLiteral("*_字幕文件*")}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    auto *exportResult = window->findChild<QObject *>(QStringLiteral("subtitleExportResult"));
    QVERIFY(exportResult);
    QVERIFY(QMetaObject::invokeMethod(exportResult, "close"));
    const QString copiedMedia = dir.filePath(QStringLiteral("字幕视频.mp4"));
    QVERIFY(QFile::copy(mediaPath(QStringLiteral("cfr_av.mp4")), copiedMedia));
    editor.loadMedia(QUrl::fromLocalFile(copiedMedia));
    QTRY_VERIFY_WITH_TIMEOUT(!editor.busy() && editor.hasVideo(), 8'000);
    editor.applySubtitles({first, second});
    editor.selectCue(0, false);
    editor.releasePlayback();
    editor.claimPlayback();
    editor.seek(0);
    editor.navigateOverlap(1);
    QCOMPARE(editor.positionUs(), qint64(300'001));
    QVERIFY(editor.currentSubtitleText().contains(second.text));
    auto *videoPreview = window->findChild<VideoPreviewItem *>(QStringLiteral("videoPreview"));
    QVERIFY(videoPreview);
    QTRY_VERIFY2_WITH_TIMEOUT(videoPreview->hasFrame() && videoPreview->framePtsUs() >= 300'001 && videoPreview->framePtsUs() - 300'001 <= 1'000'000 / editor.fps() + 1'000, qPrintable(QStringLiteral("frame=%1 pts=%2 position=%3").arg(videoPreview->hasFrame()).arg(videoPreview->framePtsUs()).arg(editor.positionUs())), 3'000);
    editor.seek(400);
    window->resize(1440, 900);
    editor.fitTimeline(timeline->width());
    QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-video-%1.png").arg(QGuiApplication::platformName()))));
    QCOMPARE(editor.selectedCueId(), first.id);
    QSignalSpy saved(&editor, &AppController::projectSaveFinished);
    const QString subtitleProject = dir.filePath(QStringLiteral("offline.subcue"));
    QVERIFY(editor.saveProject(QUrl::fromLocalFile(subtitleProject)));
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY(saved.first().first().toBool());
    auto *subtitleResult = window->findChild<QObject *>(QStringLiteral("operationResultDialog"));
    QVERIFY(subtitleResult);
    QVERIFY(QMetaObject::invokeMethod(subtitleResult, "close"));
    editor.newProject();
    QVERIFY(QFile::rename(copiedMedia, copiedMedia + QStringLiteral(".moved")));
    editor.openProject(QUrl::fromLocalFile(subtitleProject));
    QTRY_VERIFY_WITH_TIMEOUT(!editor.busy(), 8'000);
    QVERIFY(!editor.hasMedia());
    QCOMPARE(editor.overlappingCueCount(), 2);
    QCOMPARE(editor.selectedCueId(), first.id);
    QCOMPARE(workspace->property("draftId").toString(), first.id);
    QVERIFY(QMetaObject::invokeMethod(subtitleResult, "close"));
    QTest::qWait(100);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/subtitle-offline-%1.png").arg(QGuiApplication::platformName()))));
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    QVERIFY(router.switchTo(QStringLiteral("roughcut")));
    QTest::qWait(200);
    QVERIFY(window->findChild<QObject *>(QStringLiteral("roughCutWorkspace")));
    QVERIFY(window->findChild<QObject *>(QStringLiteral("roughCutAiReviewButton")));
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/roughcut-workspace-%1.png")
        .arg(QGuiApplication::platformName()))));
    roughCut.loadMedia(QUrl::fromLocalFile(mediaPath(QStringLiteral("audio.wav"))));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy(), 8'000);
    auto *playButton = qobject_cast<QQuickItem *>(window->findChild<QObject *>(
        QStringLiteral("roughCutPlayPause")));
    QVERIFY(playButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        playButton->mapToScene(QPointF(playButton->width() / 2, playButton->height() / 2)).toPoint());
    QTRY_VERIFY_WITH_TIMEOUT(roughCut.playing(), 1'000);
    QTRY_VERIFY_WITH_TIMEOUT(roughCut.positionMs() > 100, 3'000);
    roughCut.togglePlay();

    RoughCutProject project;
    project.mediaPath = mediaPath(QStringLiteral("audio.wav"));
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(project.mediaPath);
    project.sampleRate = roughCut.sampleRate();
    project.channels = 1;
    project.sourceSampleCount = roughCut.durationMs() * project.sampleRate / 1000;
    project.recording = {{QStringLiteral("p1"), QStringLiteral("预览"), 0, project.sourceSampleCount}};
    RoughCutSegmentDecision decision;
    decision.recordingIndex = 0;
    decision.autoDecision = RoughCutDecision::Keep;
    project.decisions = {decision};
    const QString projectPath = dir.filePath(QStringLiteral("preview.subcue-roughcut"));
    QVERIFY(RoughCutProjectSerializer::save(projectPath, project));
    roughCut.openProject(QUrl::fromLocalFile(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy(), 8'000);
    auto *operationResult = window->findChild<QObject *>(QStringLiteral("operationResultDialog"));
    QVERIFY(operationResult);
    QTRY_VERIFY(operationResult->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(operationResult, "close"));
    auto *sequenceButton = qobject_cast<QQuickItem *>(window->findChild<QObject *>(
        QStringLiteral("roughCutSequencePlay")));
    QVERIFY(sequenceButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        sequenceButton->mapToScene(QPointF(sequenceButton->width() / 2,
            sequenceButton->height() / 2)).toPoint());
    QTRY_VERIFY_WITH_TIMEOUT(roughCut.timelineActive(), 1'000);
    QTRY_VERIFY_WITH_TIMEOUT(roughCut.positionMs() > 100, 3'000);
    roughCut.stopTimeline();
    project.mediaPath = mediaPath(QStringLiteral("cfr_av.mp4"));
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(project.mediaPath);
    const auto probe = MediaProbe::probe(project.mediaPath);
    QVERIFY(std::holds_alternative<MediaInfo>(probe));
    const auto info = std::get<MediaInfo>(probe);
    const auto &audioInfo = info.streams.at(info.audioStreamIndex);
    project.sampleRate = audioInfo.sampleRate;
    project.channels = audioInfo.channels;
    project.sourceSampleCount = audioInfo.duration.microseconds() * project.sampleRate / 1'000'000;
    project.recording[0].endSample = project.sourceSampleCount;
    project.state.insert(QStringLiteral("mediaInfo"), mediaInfoToJson(info));
    QVERIFY(RoughCutProjectSerializer::save(projectPath, project));
    roughCut.openProject(QUrl::fromLocalFile(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy() && roughCut.hasVideo(), 8'000);
    QTRY_VERIFY(operationResult->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(operationResult, "close"));
    roughCut.seek(100);
    QTest::qWait(200);
    for (const int width : {1440, 1100}) {
        window->resize(width, 700);
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/roughcut-video-results-%1-%2.png")
            .arg(QGuiApplication::platformName()).arg(width))));
    }
    roughCut.setPlaybackRate(1.5);
    auto *rateCombo = window->findChild<QObject *>(QStringLiteral("roughCutPlaybackRate"));
    QVERIFY(rateCombo);
    QTRY_COMPARE(rateCombo->property("currentIndex").toInt(), 4);
    FakeAsrService waitingAsr;
    waitingAsr.blockUntilCancel = true;
    roughCut.setAnalysisOverrides(&waitingAsr);
    roughCut.startAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(roughCut.busy() && waitingAsr.calls > 0, 8'000);
    QVERIFY(window->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/roughcut-video-task-%1.png")
        .arg(QGuiApplication::platformName()))));
    roughCut.cancelAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy(), 8'000);
    QCOMPARE(roughCut.resultCount(), 1);
    if (!qEnvironmentVariable("SUBCUE_LARGE_UI_REPORT").isEmpty()) {
        QVERIFY(router.switchTo(QStringLiteral("subtitle")));
        QList<Subtitle> large;
        for (int i = 0; i < 5000; ++i) {
            Subtitle cue; cue.id = QStringLiteral("large-%1").arg(i); cue.text = QStringLiteral("第 %1 条长文字幕，用于选择、搜索和滚动验证。").arg(i);
            cue.start = MediaTime::fromMilliseconds(i * 1000); cue.end = MediaTime::fromMilliseconds(i * 1000 + 800); large.append(cue);
        }
        editor.applySubtitles(large); QCoreApplication::processEvents();
        QVector<qint64> selection, search; QElapsedTimer response;
        for (int i = 0; i < 50; ++i) {
            response.start(); editor.selectCue(i * 99, false); QCoreApplication::processEvents(); selection.append(response.nsecsElapsed());
            response.restart(); editor.filteredSubtitleModel()->setProperty("searchText", QString::number(i * 99)); QCoreApplication::processEvents(); search.append(response.nsecsElapsed());
            editor.filteredSubtitleModel()->setProperty("searchText", QString()); QCoreApplication::processEvents();
        }
        std::sort(selection.begin(), selection.end()); std::sort(search.begin(), search.end());
        QFile report(qEnvironmentVariable("SUBCUE_LARGE_UI_REPORT")); QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument(QJsonObject{{QStringLiteral("subtitles"), 5000}, {QStringLiteral("samples"), 50},
            {QStringLiteral("selectionP95Ms"), double(selection[47]) / 1000000.0}, {QStringLiteral("searchP95Ms"), double(search[47]) / 1000000.0},
            {QStringLiteral("devicePixelRatio"), window->devicePixelRatio()}, {QStringLiteral("platform"), QGuiApplication::platformName()}}).toJson());
    }
}

void EditorIntegrationTests::appControllerAlignmentCancelWaitsForWorker()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputAss"), false);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());

    FakeAsrService asr;
    asr.blockUntilCancel = true;
    asr.transcript.words = {{1, QStringLiteral("Hello"), 100, 800}};

    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    Subtitle existing;
    existing.text = QStringLiteral("保留字幕");
    existing.start = MediaTime::fromMilliseconds(0);
    existing.end = MediaTime::fromMilliseconds(500);
    existing.status = QStringLiteral("MANUAL");
    controller.applySubtitles({existing});
    QVERIFY(controller.canExport());
    controller.startAlignment();
    QTRY_VERIFY_WITH_TIMEOUT(controller.busy(), 2'000);
    QTRY_COMPARE(controller.alignmentStage(), QStringLiteral("ASR"));
    controller.cancelAlignment();
    QCOMPARE(controller.statusText(), QStringLiteral("正在取消…"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(controller.statusText(), QStringLiteral("任务已取消。"));
    QVERIFY(controller.canExport());
    QCOMPARE(controller.document()->subtitles().at(0).id, existing.id);
    QCOMPARE(controller.alignmentStage(), QStringLiteral("Canceled"));
}

void EditorIntegrationTests::alignmentDoesNotModifyScriptText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.setScriptText(QStringLiteral("第一行\\n第二行"));
    controller.startAlignment();
    QCOMPARE(controller.scriptText(), QStringLiteral("第一行\\n第二行"));
}

void EditorIntegrationTests::replacingSubtitlesClearsUndoHistory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    Subtitle oldCue;
    oldCue.text = QStringLiteral("旧字幕");
    controller.applySubtitles({oldCue});
    QVERIFY(controller.commands()->remove(oldCue.id));
    QVERIFY(controller.commands()->canUndo());
    Subtitle newCue;
    newCue.text = QStringLiteral("新字幕");
    controller.applySubtitles({newCue});
    controller.commands()->undo();
    QCOMPARE(controller.document()->count(), 1);
    QCOMPARE(controller.document()->subtitles().at(0).id, newCue.id);
    QVERIFY(!controller.commands()->canUndo());
    QVERIFY(!controller.commands()->canRedo());
}

void EditorIntegrationTests::switchingMediaCancelsAlignment()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    context->settings.insert(QStringLiteral("outputSrt"), true);
    context->settings.insert(QStringLiteral("outputDirectory"), dir.path());
    FakeAsrService asr;
    asr.blockUntilCancel = true;
    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr);
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.setScriptText(QStringLiteral("Hello"));
    controller.startAlignment();
    QVERIFY(controller.busy());
    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(!controller.busy());
    QCoreApplication::processEvents();
    QCOMPARE(controller.mediaPath(), mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(controller.statusText().startsWith(QStringLiteral("已加载")));
    QCOMPARE(controller.document()->count(), 0);
}

void EditorIntegrationTests::indexedTimelineHandlesUnsortedAndChangedCues()
{
    TimelineSceneItem timeline;
    timeline.setWidth(1000);
    timeline.setHeight(200);
    timeline.setDurationUs(300'000'000);
    timeline.setView(1.0, 0);
    QList<Subtitle> cues;
    for (int index = 10'000; index > 0; --index) {
        Subtitle cue;
        cue.start = MediaTime::fromMilliseconds(index * 20);
        cue.end = MediaTime::fromMilliseconds(index * 20 + 10);
        cue.status = QStringLiteral("MATCHED");
        cues.append(cue);
    }
    timeline.setSubtitles(cues);
    QCOMPARE(timeline.visibleCueCount(), 49);
    timeline.setScrollOffset(100'000);
    QCOMPARE(timeline.visibleCueCount(), 50);
    timeline.setSubtitles(cues); // 同一文档快照可复用索引。
    QCOMPARE(timeline.visibleCueCount(), 50);
    cues.clear();
    Subtitle overlapping;
    overlapping.start = MediaTime::fromMilliseconds(0);
    overlapping.end = MediaTime::fromMilliseconds(200'000);
    overlapping.status = QStringLiteral("MATCHED");
    cues.append(overlapping);
    Subtitle unlocated;
    unlocated.status = QStringLiteral("SKIPPED_NO_AUDIO");
    cues.append(unlocated);
    timeline.setSubtitles(cues);
    QCOMPARE(timeline.visibleCueCount(), 1);
    timeline.clearCues();
    QCOMPARE(timeline.visibleCueCount(), 0);
    timeline.addCue(QStringLiteral("new"), 100'100'000, 100'600'000, QStringLiteral("新字幕"));
    QCOMPARE(timeline.visibleCueCount(), 1);
}

void EditorIntegrationTests::wheelTemporarilyDefersFollow()
{
    TimelineSceneItem timeline;
    timeline.setWidth(1000);
    timeline.setDurationUs(300'000'000);
    timeline.setView(0.1, 0);
    timeline.setProperty("followDirection", 1);
    QWheelEvent wheel(QPointF(100, 100), QPointF(100, 100), QPoint(), QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&timeline, &wheel);
    QCOMPARE(timeline.scrollOffset(), 90.0);
    timeline.setPlayheadUs(200'000'000);
    QCOMPARE(timeline.scrollOffset(), 90.0);
    QTest::qWait(200);
    timeline.setPlayheadUs(200'001'000);
    QVERIFY(timeline.scrollOffset() > 90.0);
}

void EditorIntegrationTests::reversePlaybackUsesElapsedTime()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMediaPath(mediaPath(QStringLiteral("audio.wav")));
    controller.seek(600);
    controller.playReverse();
    QTest::qWait(120);
    controller.stop();
    QVERIFY2(controller.positionMs() > 350 && controller.positionMs() < 600,
             qPrintable(QString::number(controller.positionMs())));
}

void EditorIntegrationTests::appControllerCreateNextScriptCue()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController controller(context.get());
    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));

    Subtitle unmatched;
    unmatched.text = QStringLiteral("下一句");
    unmatched.status = QStringLiteral("UNMATCHED");
    controller.applySubtitles({unmatched});
    QVERIFY(!controller.document()->subtitles().at(0).isTimed());
    controller.seek(100);
    controller.locateCue(0);
    QVERIFY(controller.document()->subtitles().at(0).isTimed());
    QCOMPARE(controller.document()->subtitles().at(0).status, QStringLiteral("MANUAL"));
    controller.undo();
    QVERIFY(!controller.document()->subtitles().at(0).isTimed());
    QVERIFY(!controller.canExport());
    controller.locateCueAt(unmatched.id, controller.durationMs() - 100);
    QVERIFY(!controller.document()->subtitles().at(0).isTimed());
    QVERIFY(controller.statusText().contains(QStringLiteral("250ms")));
    controller.locateCueAt(unmatched.id, 300);
    QCOMPARE(controller.document()->subtitles().at(0).start.milliseconds(), qint64(300));
    QCOMPARE(controller.document()->subtitles().at(0).end.milliseconds(), std::min(qint64(2300), controller.durationMs()));
    controller.undo();
    QVERIFY(!controller.document()->subtitles().at(0).isTimed());
    controller.redo();
    QVERIFY(controller.document()->subtitles().at(0).isTimed());
    controller.undo();

    QString edited;
    int editedRow = -1;
    QObject::connect(&controller, &AppController::editCueRequested, &controller,
        [&](int row, const QString &text) {
            editedRow = row;
            edited = text;
        });
    controller.seek(100);
    controller.createNextScriptCue();
    QCOMPARE(editedRow, 0);
    QCOMPARE(edited, QStringLiteral("下一句"));
    QVERIFY(!controller.document()->subtitles().at(0).isTimed());
    QVERIFY(controller.applyCueEdit(unmatched.id, edited, 100'000, controller.durationMs() * 1000));
    QCOMPARE(controller.document()->subtitles().at(0).start.milliseconds(), 100);
}

void EditorIntegrationTests::playbackAvSyncP95WithinFortyMs()
{
    PlaybackEngine engine;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(engine.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    engine.requestHwRuntimeFailure();
    (void)engine.seek(MediaTime::fromMilliseconds(0));
    engine.play();
    QVERIFY(engine.waitForDisplayedFrame(4'000));

    QVector<qint64> absErrorsUs;
    QElapsedTimer wall;
    wall.start();
    qint64 consumedMs = 0;
    MediaTime lastPts = MediaTime::fromMicroseconds(-1);
    while (wall.elapsed() < 650) {
        const qint64 nowMs = wall.elapsed();
        if (nowMs > consumedMs) {
            engine.consumeAudio(MediaTime::fromMilliseconds(nowMs - consumedMs));
            consumedMs = nowMs;
        }
        engine.pump();
        const DisplayedVideoFrame frame = engine.displayedFrame();
        const MediaTime clock = engine.position();
        if (frame.generation == engine.generation() && frame.pts.isValidRange() && clock.isValidRange()
            && frame.pts.microseconds() != lastPts.microseconds()) {
            absErrorsUs.append(qAbs(frame.pts.microseconds() - clock.microseconds()));
            lastPts = frame.pts;
        }
        QTest::qWait(5);
    }
    QVERIFY2(absErrorsUs.size() >= 3,
        qPrintable(QStringLiteral("not enough displayed frames: %1").arg(absErrorsUs.size())));
    std::sort(absErrorsUs.begin(), absErrorsUs.end());
    const qsizetype p95Index = (std::min)(absErrorsUs.size() - 1, (absErrorsUs.size() * 95) / 100);
    const qint64 p95 = absErrorsUs.at(p95Index);
    QVERIFY2(p95 <= 40'000,
        qPrintable(QStringLiteral("A/V sync p95=%1us exceeds 40ms").arg(p95)));
}

void EditorIntegrationTests::playbackOneThousandSeeksHasNoStaleFrame()
{
    PlaybackEngine engine;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(engine.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    engine.requestHwRuntimeFailure();
    (void)engine.seek(MediaTime::fromMilliseconds(40));
    engine.play();
    QVERIFY(engine.waitForDisplayedFrame(4'000));
    engine.pause();

    ProbeResult probed = MediaProbe::probe(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(std::holds_alternative<MediaInfo>(probed));
    const qint64 durationMs = std::get<MediaInfo>(probed).duration.milliseconds();
    const qint64 loMs = 40;
    const qint64 hiMs = (std::min)(qint64(400), (std::max)(qint64(120), durationMs - 200));
    QVERIFY(hiMs > loMs);
    for (int index = 0; index < 1'000; ++index) {
        const qint64 targetMs = loMs + (static_cast<qint64>(index) * 37) % (hiMs - loMs);
        const quint64 generation = engine.seek(MediaTime::fromMilliseconds(targetMs));
        QVERIFY2(engine.waitForDisplayedFrame(3'000),
            qPrintable(QStringLiteral("seek %1 to %2ms timed out").arg(index).arg(targetMs)));
        const DisplayedVideoFrame frame = engine.displayedFrame();
        QCOMPARE(frame.generation, generation);
        QVERIFY(frame.pts.isValidRange());
        QVERIFY2(qAbs(frame.pts.milliseconds() - targetMs) < 250,
            qPrintable(QStringLiteral("stale/old frame pts=%1 target=%2 gen=%3")
                           .arg(frame.pts.milliseconds())
                           .arg(targetMs)
                           .arg(generation)));
    }
}

void EditorIntegrationTests::playbackRepeatedOpenCloseDoesNotGrowUnbounded()
{
    const QString path = mediaPath(QStringLiteral("cfr_av.mp4"));
    auto cycle = [&] {
        PlaybackEngine engine;
        AppError error(ErrorDomain::Media, 0, QString());
        QVERIFY(engine.open(path, &error));
        engine.play();
        (void)engine.waitForDisplayedFrame(1'000);
        engine.seek(MediaTime::fromMilliseconds(120));
        (void)engine.waitForDisplayedFrame(1'000);
        engine.close();
    };

    for (int index = 0; index < 3; ++index) {
        cycle();
    }
    for (int index = 0; index < 40; ++index) {
        cycle();
    }
#ifdef Q_OS_WIN
    const SIZE_T baseline = privateBytes();
#endif
    for (int index = 0; index < 10; ++index) {
        cycle();
    }
#ifdef Q_OS_WIN
    const SIZE_T after = privateBytes();
    if (!qEnvironmentVariable("SUBCUE_P8_UI_REPORT").isEmpty()) {
        QFile report(QDir(QFileInfo(qEnvironmentVariable("SUBCUE_P8_UI_REPORT")).absolutePath()).filePath(QStringLiteral("media-lifecycle-memory.json")));
        QVERIFY(report.open(QIODevice::WriteOnly)); report.write(QJsonDocument(QJsonObject{{QStringLiteral("cycles"), 53},
            {QStringLiteral("lateBaselineBytes"), double(baseline)}, {QStringLiteral("lateAfterBytes"), double(after)},
            {QStringLiteral("lateGrowthBytes"), double(after) - double(baseline)}}).toJson());
    }
    QVERIFY2(after < baseline + 32ull * 1024ull * 1024ull,
        qPrintable(QStringLiteral("private bytes grew from %1 to %2")
                       .arg(static_cast<qulonglong>(baseline))
                       .arg(static_cast<qulonglong>(after))));
#endif
}

void EditorIntegrationTests::voiceClearsUnsupportedParametersAndPreservesArrangement()
{
    QTemporaryDir directory; auto context = makeContext(directory); AppController editor(context.get()); RoughCutController rough(context.get());
    ProjectSession session(context.get(), &editor, &rough); VoiceStudioController voice(context.get(), &session); session.newProject();
    auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("切换模型。保留其它句。"));
    auto items = document.value(QStringLiteral("segments")).toArray(); auto item = items.first().toObject();
    item.insert(QStringLiteral("provider"), QStringLiteral("gpt-local"));
    item.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("seed"), 7}, {QStringLiteral("speedFactor"), 0.85},
        {QStringLiteral("durationFactor"), 1.15}, {QStringLiteral("emotion"), QStringLiteral("happy")}, {QStringLiteral("emotionStrength"), 0.3}});
    item.insert(QStringLiteral("gain"), 0.8); item.insert(QStringLiteral("pauseMs"), 450);
    item.insert(QStringLiteral("selectedTake"), QJsonObject{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("sampleRate"), 24000}, {QStringLiteral("sampleCount"), QStringLiteral("24000")},
        {QStringLiteral("trimStart"), QStringLiteral("10")}, {QStringLiteral("trimEnd"), QStringLiteral("23000")},
        {QStringLiteral("manualCrop"), true}, {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex())}});
    items[0] = item; document.insert(QStringLiteral("segments"), items); QVERIFY(VoiceDocument::validate(document));
    for (const auto &engine : {QStringLiteral("qwen"), QStringLiteral("index"), QStringLiteral("gpt"), QStringLiteral("cloud"), QStringLiteral("mock")}) {
        voice.reset(document);
        QCOMPARE(voice.generationReadiness(engine, {}, {}).value(QStringLiteral("action")).toString(), QStringLiteral("parameters"));
        voice.clearUnsupportedParameters(engine);
        const auto changed = voice.snapshot().value(QStringLiteral("segments")).toArray(); const auto current = changed.first().toObject();
        const auto parameters = current.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("seed")).toInt(), 7);
        QCOMPARE(parameters.contains(QStringLiteral("speedFactor")), engine == QStringLiteral("gpt") || engine == QStringLiteral("cloud"));
        QCOMPARE(parameters.contains(QStringLiteral("durationFactor")), engine == QStringLiteral("index"));
        QCOMPARE(parameters.contains(QStringLiteral("emotion")), engine == QStringLiteral("index"));
        QCOMPARE(parameters.contains(QStringLiteral("emotionStrength")), engine == QStringLiteral("index"));
        QCOMPARE(current.value(QStringLiteral("selectedTake")), item.value(QStringLiteral("selectedTake")));
        QCOMPARE(current.value(QStringLiteral("gain")), item.value(QStringLiteral("gain"))); QCOMPARE(current.value(QStringLiteral("pauseMs")), item.value(QStringLiteral("pauseMs")));
        QCOMPARE(current.value(QStringLiteral("inputVersion")).toString(), QStringLiteral("1")); QCOMPARE(changed.last(), items.last());
        QVERIFY(voice.generationReadiness(engine, {}, {}).value(QStringLiteral("action")).toString() != QStringLiteral("parameters"));
        const auto cleared = voice.snapshot(); voice.undo(); QCOMPARE(voice.snapshot(), document); voice.redo(); QCOMPARE(voice.snapshot(), cleared);
    }
    voice.reset(document); voice.clearUnsupportedParameters(QStringLiteral("unknown")); QCOMPARE(voice.snapshot(), document);
    item.insert(QStringLiteral("locked"), true); items[0] = item; document.insert(QStringLiteral("segments"), items); voice.reset(document);
    voice.clearUnsupportedParameters(QStringLiteral("qwen")); QCOMPARE(voice.snapshot(), document);
}

QTEST_MAIN(EditorIntegrationTests)
#include "test_editor_integration.moc"
