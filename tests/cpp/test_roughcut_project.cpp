#include "roughcut/rough_cut_project.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

using namespace subcue;

class RoughCutProjectTests final : public QObject {
    Q_OBJECT

private slots:
    void roundTripPreservesManualDecision();
    void mediaFingerprintDetectsContentChange();
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
    project.recording[0].takeGroupId = 2;
    RoughCutSegmentDecision decision;
    decision.recordingIndex = 0;
    decision.autoDecision = RoughCutDecision::Review;
    decision.userDecision = RoughCutDecision::Keep;
    decision.reason = QStringLiteral("人工确认");
    decision.evidence = {QStringLiteral("边界可信")};
    decision.takeGroupId = 2;
    decision.bestTake = true;
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
    QVERIFY(loaded->decisions.constFirst().bestTake);
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

QTEST_GUILESS_MAIN(RoughCutProjectTests)
#include "test_roughcut_project.moc"
