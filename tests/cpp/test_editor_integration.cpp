#include "ai/ai_provider.h"
#include "alignment/alignment_pipeline.h"
#include "app_controller.h"
#include "application_context.h"
#include "asr/asr_service.h"
#include "media/media_probe.h"
#include "playback/playback_engine.h"
#include "subtitle/subtitle.h"
#include "timeline_scene_item.h"
#include "video_preview_item.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QWheelEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlComponent>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>

#include <algorithm>
#include <memory>
#include <variant>

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
    bool blockUntilCancel = false;
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

class FakeAiProvider final : public IAiProvider {
public:
    int calls = 0;
    Qt::HANDLE workerThreadId = nullptr;

    [[nodiscard]] QString providerId() const override
    {
        return QStringLiteral("fake-ai");
    }

    [[nodiscard]] AiReviewResult review(
        QVector<Subtitle> &subtitles,
        const QVector<TranscriptWord> &words,
        const std::atomic<bool> *cancel,
        const AiProgress &progress) override
    {
        Q_UNUSED(subtitles);
        Q_UNUSED(words);
        ++calls;
        workerThreadId = QThread::currentThreadId();
        if (progress) {
            progress(1, 1);
        }
        if (aiCancelled(cancel)) {
            return aiCancelledError();
        }
        return 1;
    }

    [[nodiscard]] QVector<AppError> errors() const override
    {
        return {};
    }
};

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
        QStringLiteral("T"),
        QStringLiteral("["),
        QStringLiteral("]"),
        QStringLiteral("C"),
        QStringLiteral("Return"),
        QStringLiteral("Enter"),
        QStringLiteral("Shift+Return"),
        QStringLiteral("Shift+Enter"),
        QStringLiteral("Up"),
        QStringLiteral("Down"),
        QStringLiteral("Tab"),
        QStringLiteral("I"),
        QStringLiteral("O"),
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
    QRegularExpression sequence(QStringLiteral("sequence:\\s*\"([^\"]+)\""));
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

#ifdef Q_OS_WIN
[[nodiscard]] SIZE_T workingSetBytes()
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.WorkingSetSize;
}
#endif

} // namespace

class EditorIntegrationTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void qmlShortcutsMatchFeatureMatrix();
    void qmlImportDropAndLayout();
    void exportResultUsesIncrementingSubtitleFolder();
    void pipelineAlignsWithInjectedAsrOnWorkerThread();
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
    [[nodiscard]] std::unique_ptr<ApplicationContext> makeContext(const QTemporaryDir &dir) const;
};

void EditorIntegrationTests::initTestCase()
{
    // 截图用例写源码树下的 .test_tmp/，而 QImage::save 不会自建上层目录。
    const QString screenshotDir = sourcePath(QStringLiteral(".test_tmp"));
    QVERIFY2(QDir().mkpath(screenshotDir),
             qPrintable(QStringLiteral("无法创建截图目录: %1").arg(screenshotDir)));
}

QString EditorIntegrationTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

QString EditorIntegrationTests::sourcePath(const QString &relative) const
{
    return QDir(QString::fromUtf8(SUBCUE_SOURCE_DIR)).filePath(relative);
}

std::unique_ptr<ApplicationContext> EditorIntegrationTests::makeContext(const QTemporaryDir &dir) const
{
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
    for (const QString &file : QDir(sourcePath(QStringLiteral("src/app/qml/icons"))).entryList({QStringLiteral("*.svg")}, QDir::Files)) {
        QVERIFY(QFile::copy(sourcePath(QStringLiteral("src/app/qml/icons/") + file), QDir(modulePath).filePath(QStringLiteral("icons/") + file)));
    }
    QFile qmldir(QDir(modulePath).filePath(QStringLiteral("qmldir")));
    QVERIFY(qmldir.open(QIODevice::WriteOnly));
    QCOMPARE(qmldir.write(manifest), manifest.size());
    qmldir.close();
    qmlRegisterType<TimelineSceneItem>("SubCue", 1, 0, "TimelineSceneItem");
    qmlRegisterType<VideoPreviewItem>("SubCue", 1, 0, "VideoPreviewItem");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts")).toUtf8());
#endif

    auto context = makeContext(dir);
    AppController controller(context.get());
    QQmlApplicationEngine engine;
    engine.addImportPath(dir.path());
    QQmlComponent nativeTheme(&engine);
    nativeTheme.setData("import QtQuick; QtObject { property bool expandedClientArea: false; function applyDarkTitleBar() {} }", QUrl());
    QObject *theme = nativeTheme.create();
    QVERIFY(theme);
    theme->setParent(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("nativeTheme"), theme);
    engine.rootContext()->setContextProperty(QStringLiteral("editor"), &controller);
    QStringList warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors) warnings.append(error.toString());
    });
    engine.load(QUrl::fromLocalFile(QDir(modulePath).filePath(QStringLiteral("Main.qml"))));
    QVERIFY2(!engine.rootObjects().isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
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
    QVERIFY(settingsWindow->grabWindow().save(sourcePath(QStringLiteral(".test_tmp/settings-%1.png")
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
    controller.seek(16'100);
    controller.playForward();
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
    context->settings.insert(QStringLiteral("aiAssistEnabled"), true);

    FakeAsrService asr;
    asr.transcript.words = {
        {1, QStringLiteral("Hello"), 100, 700},
        {2, QStringLiteral("world"), 700, 1'400},
    };
    FakeAiProvider ai;

    AppController controller(context.get());
    controller.setAlignmentOverrides(&asr, &ai);
    controller.loadMediaPath(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(controller.hasMedia());
    controller.setScriptText(QStringLiteral("Hello\nworld"));

    const Qt::HANDLE guiThread = QThread::currentThreadId();
    bool sawChunks = false;
    connect(&controller, &AppController::alignmentProgressChanged, &controller, [&] {
        QCOMPARE(QThread::currentThreadId(), guiThread);
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
    QVERIFY(controller.statusText().contains(QStringLiteral("打轴完成")));
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
    QVERIFY(controller.document()->subtitles().at(0).isTimed());
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
#ifdef Q_OS_WIN
    const SIZE_T baseline = workingSetBytes();
#endif
    for (int index = 0; index < 50; ++index) {
        cycle();
    }
#ifdef Q_OS_WIN
    const SIZE_T after = workingSetBytes();
    QVERIFY2(after < baseline + 32ull * 1024ull * 1024ull,
        qPrintable(QStringLiteral("working set grew from %1 to %2")
                       .arg(static_cast<qulonglong>(baseline))
                       .arg(static_cast<qulonglong>(after))));
#endif
}

QTEST_MAIN(EditorIntegrationTests)
#include "test_editor_integration.moc"
