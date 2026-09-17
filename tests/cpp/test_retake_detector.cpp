#include "roughcut/retake_detector.h"

#include <QtTest/QTest>

using namespace subcue;

class RetakeDetectorTests final : public QObject {
    Q_OBJECT

private slots:
    void recommendsCompleteReplacement();
    void keepsAmbiguousAndWorseLastForReview();
    void limitsNeighbourWindow();
};

void RetakeDetectorTests::recommendsCompleteReplacement()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("我们先去城堡"), 0, 48'000},
        {QStringLiteral("b"), QStringLiteral("我们先去城堡。"), 96'000, 160'000},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Retake, 82.0},
        {1, 0, ScriptMatchStatus::Match, 98.0},
    };
    const auto groups = RetakeDetector::detect(recording, matches, 48'000);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().recommendedRecordingIndex, 1);
    QVERIFY(!groups.first().needsReview);
}

void RetakeDetectorTests::keepsAmbiguousAndWorseLastForReview()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("获得宝剑。"), 0, 40'000},
        {QStringLiteral("b"), QStringLiteral("获得宝剑呃"), 50'000, 90'000},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Match, 95.0},
        {1, 0, ScriptMatchStatus::Retake, 75.0},
    };
    const auto groups = RetakeDetector::detect(recording, matches, 48'000);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.first().recommendedRecordingIndex, 0);
    QVERIFY(!groups.first().needsReview);

    QVector<ScriptMatch> tied = matches;
    tied[1].similarity = 100.0;
    const auto stillWorse = RetakeDetector::detect(recording, tied, 48'000);
    QCOMPARE(stillWorse.first().recommendedRecordingIndex, 0);
    QVERIFY(!stillWorse.first().needsReview);
}

void RetakeDetectorTests::limitsNeighbourWindow()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("重复文本。"), 0, 48'000},
        {QStringLiteral("b"), QStringLiteral("重复文本。"), 100LL * 48'000, 101LL * 48'000},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Match, 100.0},
        {1, 0, ScriptMatchStatus::Retake, 100.0},
    };
    QVERIFY(RetakeDetector::detect(recording, matches, 48'000).isEmpty());
}

QTEST_MAIN(RetakeDetectorTests)
#include "test_retake_detector.moc"
