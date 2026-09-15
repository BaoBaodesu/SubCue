#include "roughcut/alignment_evidence.h"

#include <QtTest/QTest>

using namespace subcue;

class RoughCutAlignmentTests final : public QObject {
    Q_OBJECT

private slots:
    void convertsAndDeduplicatesOverlap();
    void rejectsUntrustedBoundaries();
    void detectsEnergySilenceInSourceCoordinates();
};

void RoughCutAlignmentTests::convertsAndDeduplicatesOverlap()
{
    Transcript transcript{{
        {1, QStringLiteral("开场"), 100, 500},
        {2, QStringLiteral("开场"), 450, 700},
        {3, QStringLiteral("继续"), 800, 1000},
    }};
    const RoughCutAlignmentResult result = RoughCutAlignmentEvidence::validate(
        transcript, 48'000, 96'000, true);
    QVERIFY(result.trustworthy);
    QCOMPARE(result.words.size(), 2);
    QCOMPARE(result.words.at(0).startSample, 4'800);
    QCOMPARE(result.words.at(0).endSample, 33'600);
}

void RoughCutAlignmentTests::rejectsUntrustedBoundaries()
{
    const Transcript transcript{{{1, QStringLiteral("粗定位"), 0, 1000}}};
    const RoughCutAlignmentResult coarse = RoughCutAlignmentEvidence::validate(
        transcript, 44'100, 44'100, false);
    QVERIFY(!coarse.trustworthy);
    QVERIFY(coarse.reviewReason.contains(QStringLiteral("粗定位")));

    const Transcript overlapping{{
        {1, QStringLiteral("甲"), 0, 600},
        {2, QStringLiteral("乙"), 500, 900},
    }};
    QVERIFY(!RoughCutAlignmentEvidence::validate(overlapping, 48'000, 48'000, true).trustworthy);
}

void RoughCutAlignmentTests::detectsEnergySilenceInSourceCoordinates()
{
    QVector<float> pcm(8'000, 0.0F);
    std::fill(pcm.begin() + 2'000, pcm.begin() + 4'000, 0.5F);
    const auto silence = RoughCutAlignmentEvidence::detectSilence(
        pcm, 16'000, 48'000, 4'800, 0.01, 100);
    QCOMPARE(silence.size(), 2);
    QCOMPARE(silence.at(0).startSample, 4'800);
    QCOMPARE(silence.at(0).endSample, 10'800);
    QCOMPARE(silence.at(1).startSample, 16'800);
    QCOMPARE(silence.at(1).endSample, 28'800);
}

QTEST_MAIN(RoughCutAlignmentTests)
#include "test_roughcut_alignment.moc"
