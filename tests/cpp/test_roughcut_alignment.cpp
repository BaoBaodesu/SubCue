#include "roughcut/alignment_evidence.h"

#include <QtTest/QTest>

using namespace subcue;

class RoughCutAlignmentTests final : public QObject {
    Q_OBJECT

private slots:
    void convertsAndDeduplicatesOverlap();
    void rejectsUntrustedBoundaries();
    void detectsEnergySilenceInSourceCoordinates();
    void splitsRestartInsideOneSpeechRegion();
    void joinsWordsAcrossVadRegions();
    void coarseTimingCannotAuthorizeCut();
    void splitsRepeatedPrefixAfterPause();
    void isolatesFillerWithoutCuttingScriptWords();
    void touchingRestartBoundaryRequiresReview();
    void keepsTrustedPassagesAroundBadWordTimes();
    void internalZeroTimeDoesNotInvalidateCutBoundaries();
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
    const auto localOverlap = RoughCutAlignmentEvidence::validate(overlapping, 48'000, 48'000, true);
    QVERIFY(localOverlap.trustworthy);
    QVERIFY(!localOverlap.words.at(0).timingTrustworthy);
    QVERIFY(!localOverlap.words.at(1).timingTrustworthy);
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

void RoughCutAlignmentTests::splitsRestartInsideOneSpeechRegion()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天"), 100, 240},
        {2, QStringLiteral("我们"), 250, 400},
        {3, QStringLiteral("说错了"), 410, 650},
        {4, QStringLiteral("今天"), 780, 920},
        {5, QStringLiteral("我们"), 930, 1080},
        {6, QStringLiteral("介绍自动粗剪"), 1090, 1450},
    }};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 2'000}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 2'000);
    QCOMPARE(passages.size(), 2);
    QCOMPARE(passages.at(0).text, QStringLiteral("今天我们说错了"));
    QCOMPARE(passages.at(1).text, QStringLiteral("今天我们介绍自动粗剪"));
    QVERIFY(passages.at(0).endSample <= passages.at(1).startSample);
    QVERIFY(passages.at(0).boundaryTrustworthy);
}

void RoughCutAlignmentTests::joinsWordsAcrossVadRegions()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天我们"), 100, 380},
        {2, QStringLiteral("介绍自动粗剪"), 430, 900},
    }};
    const QVector<RecognizedPassage> speech{
        {QStringLiteral("vad1"), {}, 0, 400},
        {QStringLiteral("vad2"), {}, 420, 1'000}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 1'000);
    QCOMPARE(passages.size(), 1);
    QCOMPARE(passages.constFirst().text, QStringLiteral("今天我们介绍自动粗剪"));
}

void RoughCutAlignmentTests::coarseTimingCannotAuthorizeCut()
{
    const Transcript transcript{{{1, QStringLiteral("今天我们介绍自动粗剪"), 0, 1'000, false}}};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 1'000}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 1'000);
    QCOMPARE(passages.size(), 1);
    QVERIFY(!passages.constFirst().boundaryTrustworthy);
    QVERIFY(!passages.constFirst().preciseTiming);
}

void RoughCutAlignmentTests::splitsRepeatedPrefixAfterPause()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天"), 100, 200},
        {2, QStringLiteral("我们"), 210, 320},
        {3, QStringLiteral("介绍"), 330, 450},
        {4, QStringLiteral("今天"), 590, 700},
        {5, QStringLiteral("我们"), 710, 820},
        {6, QStringLiteral("介绍自动粗剪"), 830, 1'160},
    }};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 1'500}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 1'500);
    QCOMPARE(passages.size(), 2);
    QCOMPARE(passages.at(0).text, QStringLiteral("今天我们介绍"));
    QCOMPARE(passages.at(1).text, QStringLiteral("今天我们介绍自动粗剪"));
}

void RoughCutAlignmentTests::isolatesFillerWithoutCuttingScriptWords()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天我们"), 100, 420},
        {2, QStringLiteral("呃"), 570, 650},
        {3, QStringLiteral("介绍自动粗剪"), 810, 1'180},
    }};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 1'500}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 1'500);
    QCOMPARE(passages.size(), 3);
    QCOMPARE(passages.at(0).text, QStringLiteral("今天我们"));
    QCOMPARE(passages.at(1).text, QStringLiteral("呃"));
    QCOMPARE(passages.at(2).text, QStringLiteral("介绍自动粗剪"));
    QVERIFY(passages.at(0).endSample <= passages.at(1).startSample);
    QVERIFY(passages.at(1).endSample <= passages.at(2).startSample);
}

void RoughCutAlignmentTests::touchingRestartBoundaryRequiresReview()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天说错了"), 100, 450},
        {2, QStringLiteral("今天我们介绍"), 460, 900},
    }};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 1'000}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 1'000);
    QCOMPARE(passages.size(), 2);
    QVERIFY(!passages.at(0).boundaryTrustworthy);
    QVERIFY(!passages.at(1).boundaryTrustworthy);
}

void RoughCutAlignmentTests::keepsTrustedPassagesAroundBadWordTimes()
{
    const Transcript transcript{{
        {1, QStringLiteral("开场完整"), 100, 400},
        {2, QStringLiteral("呃"), 950, 950},
        {3, QStringLiteral("口胡"), 950, 1'100},
        {4, QStringLiteral("甲"), 1'200, 1'450},
        {5, QStringLiteral("乙"), 1'400, 1'600},
        {6, QStringLiteral("结尾完整"), 2'200, 2'600},
    }};
    const QVector<RecognizedPassage> speech{{QStringLiteral("vad"), {}, 0, 2'800}};
    const auto passages = RoughCutAlignmentEvidence::buildPassages(speech, transcript, 1'000, 2'800);
    QCOMPARE(passages.size(), 3);
    QVERIFY(passages.at(0).boundaryTrustworthy);
    QVERIFY(!passages.at(1).boundaryTrustworthy);
    QVERIFY(passages.at(2).boundaryTrustworthy);
    QCOMPARE(passages.at(1).text, QStringLiteral("呃口胡甲乙"));
}

void RoughCutAlignmentTests::internalZeroTimeDoesNotInvalidateCutBoundaries()
{
    const Transcript transcript{{
        {1, QStringLiteral("今天"), 100, 300},
        {2, QStringLiteral("我"), 300, 300},
        {3, QStringLiteral("们介绍"), 300, 700},
        {4, QStringLiteral("自动粗剪"), 710, 1'100},
    }};
    const auto passages = RoughCutAlignmentEvidence::buildPassages({}, transcript, 1'000, 1'200);
    QCOMPARE(passages.size(), 1);
    QCOMPARE(passages.constFirst().text, QStringLiteral("今天我们介绍自动粗剪"));
    QVERIFY(passages.constFirst().boundaryTrustworthy);
}

QTEST_MAIN(RoughCutAlignmentTests)
#include "test_roughcut_alignment.moc"
