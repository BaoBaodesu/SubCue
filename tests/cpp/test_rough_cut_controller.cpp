#include "app_controller.h"
#include "application_context.h"
#include "asr/asr_service.h"
#include "rough_cut_controller.h"
#include "roughcut/rough_cut_project.h"
#include "workspace_router.h"

#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtTest/QTest>

#include <cstdio>
#include <memory>

using namespace subcue;

namespace {

class FakeAsrService final : public IAsrService {
public:
    Transcript transcript;
    bool blockUntilCancel = false;
    bool fail = false;
    int calls = 0;

    [[nodiscard]] QString providerId() const override
    {
        return QStringLiteral("fake");
    }

    [[nodiscard]] AsrResult transcribe(const AsrRequest &request) override
    {
        ++calls;
        while (blockUntilCancel && !asrCancelled(request.cancel)) {
            QThread::msleep(15);
        }
        if (asrCancelled(request.cancel)) return asrCancelledError();
        if (fail || transcript.words.isEmpty()) {
            return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EmptyTranscript),
                QStringLiteral("没有识别到词"));
        }
        Transcript copy = transcript;
        copy.sortAndReindex();
        return copy;
    }
};

} // namespace

class RoughCutControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void reanalysisKeepsOldResultsUntilSuccess();
    void unsavedStateTracksEditsSaveAndUndo();
    void saveFailsDoesNotClearModified();
    void workspaceSwitchTransfersPlaybackWithoutAutoPlay();
    void reviewFilterAndExportGuards();

private:
    [[nodiscard]] QString writeWav(const QString &path) const
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        constexpr int sampleRate = 16'000;
        constexpr int samples = sampleRate;
        QByteArray pcm(samples * 2, 0);
        auto *data = reinterpret_cast<qint16 *>(pcm.data());
        for (int index = 2'000; index < 10'000; ++index)
            data[index] = qint16(((index % 40) < 20 ? 1 : -1) * 8'000);
        QDataStream wav(&file);
        wav.setByteOrder(QDataStream::LittleEndian);
        wav.writeRawData("RIFF", 4);
        wav << quint32(36 + pcm.size());
        wav.writeRawData("WAVEfmt ", 8);
        wav << quint32(16) << quint16(1) << quint16(1) << quint32(sampleRate)
            << quint32(sampleRate * 2) << quint16(2) << quint16(16);
        wav.writeRawData("data", 4);
        wav << quint32(pcm.size());
        file.write(pcm);
        return path;
    }

    [[nodiscard]] std::unique_ptr<ApplicationContext> makeContext(const QTemporaryDir &dir) const
    {
        return std::make_unique<ApplicationContext>(
            dir.filePath(QStringLiteral("settings.json")),
            dir.filePath(QStringLiteral("credentials.dat")),
            AudioDeviceKind::Virtual);
    }

    [[nodiscard]] QString writeProject(const QTemporaryDir &dir, const QString &wav,
        RoughCutDecision userDecision = RoughCutDecision::Keep) const
    {
        RoughCutProject project;
        project.mediaPath = wav;
        QString hashError;
        project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(wav, &hashError);
        project.scriptText = QStringLiteral("第一句。");
        project.sampleRate = 16'000;
        project.channels = 1;
        project.sourceSampleCount = 16'000;
        project.analysisVersion = 1;
        project.recording = {{QStringLiteral("p1"), QStringLiteral("第一句。"), 0, 16'000}};
        RoughCutSegmentDecision decision;
        decision.recordingIndex = 0;
        decision.autoDecision = RoughCutDecision::Review;
        decision.userDecision = userDecision;
        decision.reason = QStringLiteral("人工确认");
        project.decisions = {decision};
        const QString path = dir.filePath(QStringLiteral("工程.subcue-roughcut"));
        QString error;
        if (!RoughCutProjectSerializer::save(path, project, &error)) {
            qWarning("writeProject failed hash=%s shaSize=%d rec=%d dec=%d save=%s",
                qPrintable(hashError), int(project.mediaSha256.size()),
                project.recording.size(), project.decisions.size(), qPrintable(error));
            return {};
        }
        return path;
    }
};

void RoughCutControllerTests::initTestCase()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

void RoughCutControllerTests::reanalysisKeepsOldResultsUntilSuccess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString wav = writeWav(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!wav.isEmpty());
    const QString projectPath = writeProject(dir, wav);
    QVERIFY(!projectPath.isEmpty());

    auto context = makeContext(dir);
    RoughCutController controller(context.get());
    controller.openProject(QUrl::fromLocalFile(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.decisions().constFirst().userDecision, RoughCutDecision::Keep);
    controller.setDecision(0, QStringLiteral("CUT"));
    QVERIFY(controller.canUndo());
    const QString oldText = controller.recording().constFirst().text;

    FakeAsrService asr;
    asr.blockUntilCancel = true;
    controller.setAnalysisOverrides(&asr);
    controller.startAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(controller.busy(), 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(asr.calls >= 1, 8'000);
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.recording().constFirst().text, oldText);
    QCOMPARE(controller.decisions().constFirst().effectiveDecision(), RoughCutDecision::Cut);
    controller.setDecision(0, QStringLiteral("KEEP"));
    QCOMPARE(controller.decisions().constFirst().effectiveDecision(), RoughCutDecision::Cut);
    QVERIFY(controller.canUndo() == false);
    controller.cancelAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(controller.statusText(), QStringLiteral("分析已取消。"));
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.recording().constFirst().text, oldText);
    QCOMPARE(controller.decisions().constFirst().effectiveDecision(), RoughCutDecision::Cut);
    QVERIFY(controller.canUndo());

    asr.blockUntilCancel = false;
    asr.fail = true;
    controller.startAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.recording().constFirst().text, oldText);
    QCOMPARE(controller.decisions().constFirst().effectiveDecision(), RoughCutDecision::Cut);
    QVERIFY(controller.canUndo());

    asr.fail = false;
    asr.transcript.words = {{1, QStringLiteral("第一句"), 0, 400}};
    controller.startAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(controller.statusText().startsWith(QStringLiteral("分析完成")));
    QVERIFY(!controller.canUndo());
}

void RoughCutControllerTests::unsavedStateTracksEditsSaveAndUndo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    RoughCutController controller(context.get());
    QVERIFY(!controller.modified());
    QVERIFY(!controller.canSave());

    const QString wav = writeWav(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!wav.isEmpty());
    controller.loadMedia(QUrl::fromLocalFile(wav));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(controller.canSave());
    QVERIFY(controller.modified());
    controller.setScriptText(QStringLiteral("第一句。"));
    QVERIFY(controller.modified());

    const QString unsavedPath = dir.filePath(QStringLiteral("未分析.subcue-roughcut"));
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(unsavedPath)));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(!controller.modified());
    QCOMPARE(controller.resultCount(), 0);

    const QString projectPath = writeProject(dir, wav);
    controller.openProject(QUrl::fromLocalFile(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(!controller.modified());
    controller.setDecision(0, QStringLiteral("CUT"));
    QVERIFY(controller.modified());
    controller.undo();
    QVERIFY(!controller.modified());
    controller.setDecision(0, QStringLiteral("CUT"));
    QVERIFY(controller.saveCurrentProject());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(!controller.modified());
}

void RoughCutControllerTests::saveFailsDoesNotClearModified()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    RoughCutController controller(context.get());
    const QString wav = writeWav(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!wav.isEmpty());
    controller.loadMedia(QUrl::fromLocalFile(wav));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    controller.setScriptText(QStringLiteral("文案"));
    QVERIFY(controller.modified());
    QVERIFY(controller.saveProject(QUrl::fromLocalFile(dir.path())));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(controller.modified());
}

void RoughCutControllerTests::workspaceSwitchTransfersPlaybackWithoutAutoPlay()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto context = makeContext(dir);
    AppController editor(context.get());
    RoughCutController roughCut(context.get());
    WorkspaceRouter router(&editor, &roughCut);

    const QString wav = writeWav(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!wav.isEmpty());
    editor.loadMediaPath(wav);
    editor.playForward();
    QVERIFY(editor.playing());
    const qint64 editorPosition = editor.positionMs();
    QVERIFY(router.switchTo(QStringLiteral("roughcut")));
    QVERIFY(!editor.playing());
    QVERIFY(!roughCut.playing());
    QCOMPARE(editor.positionMs(), editorPosition);

    roughCut.loadMedia(QUrl::fromLocalFile(wav));
    QTRY_VERIFY_WITH_TIMEOUT(!roughCut.busy(), 8'000);
    roughCut.togglePlay();
    QVERIFY(roughCut.playing());
    QVERIFY(router.switchTo(QStringLiteral("subtitle")));
    QVERIFY(!roughCut.playing());
    QVERIFY(!editor.playing());
    QVERIFY(router.switchTo(QStringLiteral("roughcut")));
    QVERIFY(!roughCut.playing());
}

void RoughCutControllerTests::reviewFilterAndExportGuards()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString wav = writeWav(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!wav.isEmpty());
    const QString projectPath = writeProject(dir, wav);
    QVERIFY(!projectPath.isEmpty());

    auto context = makeContext(dir);
    RoughCutController controller(context.get());
    controller.openProject(QUrl::fromLocalFile(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(controller.canExport());
    QVERIFY(controller.canAiReview());
    auto *model = qobject_cast<RoughCutResultModel *>(controller.resultModel());
    QVERIFY(model);
    QCOMPARE(model->keepCount(), 1);
    QCOMPARE(controller.filterRowForSource(0), 0);
    QCOMPARE(controller.sourceResultRow(0), 0);
    controller.setStatusFilter(QStringLiteral("CUT"));
    QCOMPARE(controller.filterRowForSource(0), -1);
    controller.setStatusFilter(QStringLiteral("ALL"));

    controller.setDecision(0, QStringLiteral("CUT"));
    QVERIFY(!controller.canExport());
    QCOMPARE(model->cutCount(), 1);
    QCOMPARE(controller.nextReviewRow(-1), -1);

    controller.undo();
    QVERIFY(controller.canExport());
    controller.setDecision(0, QStringLiteral("REVIEW"));
    QVERIFY(controller.canExport());
    QCOMPARE(controller.nextReviewRow(-1), 0);
    QCOMPARE(controller.previousReviewRow(0), 0);

    FakeAsrService asr;
    asr.blockUntilCancel = true;
    asr.transcript.words = {{1, QStringLiteral("第一句"), 0, 400}};
    controller.setAnalysisOverrides(&asr);
    controller.startAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(controller.busy(), 2'000);
    QCOMPARE(controller.busyTaskTitle(), QStringLiteral("正在分析"));
    QVERIFY(!controller.canExport());
    QVERIFY(!controller.canAiReview());
    controller.cancelAnalysis();
    QVERIFY(controller.cancelling());
    QVERIFY(controller.progressIndeterminate());
    controller.cancelAnalysis();
    QVERIFY(controller.cancelling());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 8'000);
    QVERIFY(!controller.cancelling());
    QVERIFY(controller.busyTaskTitle().isEmpty());
    QVERIFY(controller.canExport());
}

QTEST_MAIN(RoughCutControllerTests)
#include "test_rough_cut_controller.moc"
