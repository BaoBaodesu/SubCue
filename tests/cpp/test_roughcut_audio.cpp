#include "roughcut/speech_segment_analyzer.h"
#include "roughcut/wav_exporter.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

using namespace subcue;

class RoughCutAudioTests final : public QObject {
    Q_OBJECT

private slots:
    void detectsSpeechBeforeAsrInSourceCoordinates();
    void boundsContinuousSpeechSegments();
    void exportsFloatWaveWithProtectedEdges();
};

void RoughCutAudioTests::detectsSpeechBeforeAsrInSourceCoordinates()
{
    QVector<float> pcm(16'000 * 2);
    for (int index = 4'000; index < 12'000; ++index) pcm[index] = 0.1f;
    const auto segments = SpeechSegmentAnalyzer::analyzePcm(pcm, 48'000, 96'000);
    QCOMPARE(segments.size(), 1);
    QVERIFY(segments.first().startSample >= 11'000);
    QVERIFY(segments.first().startSample <= 13'000);
    QVERIFY(segments.first().endSample >= 35'000);
    QVERIFY(segments.first().endSample <= 37'000);
    QVERIFY(segments.first().silenceBeforeSamples > 0);
    QVERIFY(segments.first().silenceAfterSamples > 0);
    QVERIFY(segments.first().boundaryTrustworthy);
}

void RoughCutAudioTests::boundsContinuousSpeechSegments()
{
    QVector<float> pcm(16'000 * 4, 0.1f);
    SpeechAnalysisSettings settings;
    settings.maximumSpeechMs = 1'000;
    settings.mergeGapMs = 0;
    const auto segments = SpeechSegmentAnalyzer::analyzePcm(pcm, 48'000, 192'000, settings);
    QVERIFY(segments.size() >= 3);
    for (const auto &segment : segments)
        QVERIFY(segment.durationSamples() <= 49'000);
}

void RoughCutAudioTests::exportsFloatWaveWithProtectedEdges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = directory.filePath(QStringLiteral("rough.wav"));
    QVector<RoughCutTimelineClip> clips{{4'410, 17'640, 0}, {22'050, 35'280, 17'640}};
    QString error;
    QVERIFY2(RoughCutWavExporter::save(output,
        QStringLiteral(SUBCUE_TEST_MEDIA_DIR "/audio.wav"), clips, 44'100, 1,
        nullptr, &error), qPrintable(error));
    QFile file(output);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray header = file.read(44);
    QCOMPARE(header.left(4), QByteArray("RIFF"));
    QCOMPARE(header.mid(8, 4), QByteArray("WAVE"));
    QVERIFY(file.size() > 44);
}

QTEST_APPLESS_MAIN(RoughCutAudioTests)
#include "test_roughcut_audio.moc"
