#include "roughcut/rough_cut_project.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <atomic>

using namespace subcue;

class RoughCutProjectTests final : public QObject {
    Q_OBJECT

private slots:
    void roundTripPreservesManualDecision();
    void mediaFingerprintDetectsContentChange();
    void savesUnanalyzedMediaAndScript();
    void savesAnalysisWithoutModelVersion();
    void mediaSha256CancelStopsBeforeCompletion();
};

void RoughCutProjectTests::roundTripPreservesManualDecision()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString mediaPath = directory.filePath(QStringLiteral("旁白.wav"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    QCOMPARE(media.write("source-audio"), 12);
    media.close();

    RoughCutProject project;
    project.mediaPath = mediaPath;
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(mediaPath);
    project.sampleRate = 48'000;
    project.channels = 2;
    project.sourceSampleCount = 96'000;
    project.recording = {{QStringLiteral("p1"), QStringLiteral("第一句。"), 100, 10'000}};
    project.recording[0].silenceBeforeSamples = 100;
    project.recording[0].silenceAfterSamples = 500;
    project.recording[0].vadConfidence = 0.92;
    project.recording[0].boundaryTrustworthy = true;
    project.recording[0].scriptLineIndex = 3;
    project.recording[0].scriptLineEndIndex = 5;
    project.recording[0].textSimilarity = 93.0;
    project.recording[0].editSimilarity = 91.0;
    project.recording[0].continuousCoverage = 88.0;
    project.recording[0].scriptTokenStart = 12;
    project.recording[0].scriptTokenEnd = 20;
    project.recording[0].preciseTiming = true;
    project.recording[0].takeGroupId = 2;
    RoughCutSegmentDecision decision;
    decision.recordingIndex = 0;
    decision.autoDecision = RoughCutDecision::Review;
    decision.userDecision = RoughCutDecision::Keep;
    decision.reason = QStringLiteral("人工确认");
    decision.evidence = {QStringLiteral("边界可信")};
    decision.takeGroupId = 2;
    decision.replacementRecordingIndex = 2;
    decision.bestTake = true;
    decision.failureType = RoughCutFailureType::Interrupted;
    decision.modelProbability = 0.96;
    decision.modelVersion = QStringLiteral("model-v1");
    decision.decisionSource = QStringLiteral("rule+model");
    project.decisions = {decision};
    project.auxiliaryResults = {{0, QStringLiteral("第一句。"), QStringLiteral("第一句"),
        QStringLiteral("第一句"), false, false, false}};

    const QString projectPath = directory.filePath(QStringLiteral("工程.subcue-roughcut"));
    QString error;
    QVERIFY2(RoughCutProjectSerializer::save(projectPath, project, &error), qPrintable(error));
    const std::optional<RoughCutProject> loaded = RoughCutProjectSerializer::load(projectPath, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->mediaPath, mediaPath);
    QCOMPARE(loaded->mediaSha256, project.mediaSha256);
    QCOMPARE(loaded->recording.constFirst().text, QStringLiteral("第一句。"));
    QCOMPARE(loaded->recording.constFirst().silenceBeforeSamples, 100);
    QCOMPARE(loaded->recording.constFirst().silenceAfterSamples, 500);
    QCOMPARE(loaded->recording.constFirst().vadConfidence, 0.92);
    QVERIFY(loaded->recording.constFirst().boundaryTrustworthy);
    QCOMPARE(loaded->recording.constFirst().takeGroupId, 2);
    QCOMPARE(loaded->recording.constFirst().scriptLineEndIndex, 5);
    QCOMPARE(loaded->recording.constFirst().textSimilarity, 93.0);
    QCOMPARE(loaded->recording.constFirst().editSimilarity, 91.0);
    QCOMPARE(loaded->recording.constFirst().continuousCoverage, 88.0);
    QCOMPARE(loaded->recording.constFirst().scriptTokenStart, 12);
    QCOMPARE(loaded->recording.constFirst().scriptTokenEnd, 20);
    QVERIFY(loaded->recording.constFirst().preciseTiming);
    QCOMPARE(loaded->decisions.constFirst().replacementRecordingIndex, 2);
    QVERIFY(loaded->decisions.constFirst().bestTake);
    QCOMPARE(loaded->decisions.constFirst().failureType, RoughCutFailureType::Interrupted);
    QCOMPARE(loaded->decisions.constFirst().modelProbability, 0.96);
    QCOMPARE(loaded->decisions.constFirst().modelVersion, QStringLiteral("model-v1"));
    QCOMPARE(loaded->decisions.constFirst().decisionSource, QStringLiteral("rule+model"));
    QCOMPARE(loaded->decisions.constFirst().userDecision, std::optional(RoughCutDecision::Keep));
    QCOMPARE(loaded->decisions.constFirst().evidence, QStringList{QStringLiteral("边界可信")});
    QCOMPARE(loaded->auxiliaryResults.constFirst().whisperText, QStringLiteral("第一句"));

    project.analysisVersion = 2;
    project.recording[0].text = QStringLiteral("第二次分析");
    QVERIFY2(RoughCutProjectSerializer::save(projectPath, project, &error), qPrintable(error));
    const std::optional<RoughCutProject> latest = RoughCutProjectSerializer::load(projectPath, &error);
    QVERIFY2(latest.has_value(), qPrintable(error));
    QCOMPARE(latest->analysisVersion, 2);
    QCOMPARE(latest->recording.constFirst().text, QStringLiteral("第二次分析"));
}

void RoughCutProjectTests::mediaFingerprintDetectsContentChange()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("voice.wav"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("old");
    file.close();
    const QByteArray before = RoughCutProjectSerializer::mediaSha256(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("new");
    file.close();
    const QByteArray after = RoughCutProjectSerializer::mediaSha256(path);
    QCOMPARE(before.size(), 32);
    QCOMPARE(after.size(), 32);
    QVERIFY(before != after);
}

void RoughCutProjectTests::savesUnanalyzedMediaAndScript()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString mediaPath = directory.filePath(QStringLiteral("旁白.wav"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    QCOMPARE(media.write("source-audio"), 12);
    media.close();

    RoughCutProject project;
    project.mediaPath = mediaPath;
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(mediaPath);
    project.scriptText = QStringLiteral("第一句。");
    project.sampleRate = 48'000;
    project.channels = 2;
    project.sourceSampleCount = 96'000;
    const QString projectPath = directory.filePath(QStringLiteral("未分析.subcue-roughcut"));
    QString error;
    QVERIFY2(RoughCutProjectSerializer::save(projectPath, project, &error), qPrintable(error));
    const std::optional<RoughCutProject> loaded = RoughCutProjectSerializer::load(projectPath, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->scriptText, QStringLiteral("第一句。"));
    QVERIFY(loaded->recording.isEmpty());
    QVERIFY(loaded->decisions.isEmpty());
}

void RoughCutProjectTests::savesAnalysisWithoutModelVersion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString mediaPath = directory.filePath(QStringLiteral("旁白.wav"));
    QFile media(mediaPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    QCOMPARE(media.write("source-audio"), 12);
    media.close();

    RoughCutProject project;
    project.mediaPath = mediaPath;
    project.mediaSha256 = RoughCutProjectSerializer::mediaSha256(mediaPath);
    project.sampleRate = 16'000;
    project.channels = 1;
    project.sourceSampleCount = 16'000;
    project.analysisVersion = 1;
    project.recording = {{QStringLiteral("p1"), QStringLiteral("第一句。"), 0, 16'000}};
    RoughCutSegmentDecision decision;
    decision.recordingIndex = 0;
    decision.autoDecision = RoughCutDecision::Review;
    decision.userDecision = RoughCutDecision::Keep;
    project.decisions = {decision};
    const QString projectPath = directory.filePath(QStringLiteral("工程.subcue-roughcut"));
    QString error;
    QVERIFY2(RoughCutProjectSerializer::save(projectPath, project, &error), qPrintable(error));
    const std::optional<RoughCutProject> loaded = RoughCutProjectSerializer::load(projectPath, &error);
    QVERIFY2(loaded.has_value(), qPrintable(error));
    QCOMPARE(loaded->decisions.constFirst().userDecision, std::optional(RoughCutDecision::Keep));
    QVERIFY(loaded->decisions.constFirst().modelVersion.isEmpty());
}

void RoughCutProjectTests::mediaSha256CancelStopsBeforeCompletion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("long.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QByteArray(2 * 1024 * 1024, 'a')) == 2 * 1024 * 1024);
    file.close();

    std::atomic<bool> cancel{true};
    int progressCalls = 0;
    QString error;
    const QByteArray hash = RoughCutProjectSerializer::mediaSha256(path, &error, &cancel,
        [&progressCalls](qint64, qint64) { ++progressCalls; });
    QVERIFY(hash.isEmpty());
    QCOMPARE(error, QStringLiteral("校验已取消。"));

    cancel = false;
    const QByteArray full = RoughCutProjectSerializer::mediaSha256(path, &error);
    QCOMPARE(full.size(), 32);
}

QTEST_GUILESS_MAIN(RoughCutProjectTests)
#include "test_roughcut_project.moc"
