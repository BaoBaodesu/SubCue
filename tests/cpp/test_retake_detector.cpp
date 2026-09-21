#include "roughcut/retake_detector.h"

#include <QtTest/QTest>

using namespace subcue;

class RetakeDetectorTests final : public QObject {
    Q_OBJECT

private slots:
    void recommendsCompleteReplacement();
    void keepsAmbiguousAndWorseLastForReview();
    void limitsNeighbourWindow();
    void classifiesRetakeInterruptedDuplicateAndWrongTake();
    void doesNotGroupAdjacentDifferentContentOnSameLine();
    void selectsLaterIdenticalCompleteTake();
    void prefersMultiLineCoverageOverShortPerfectPrefix();
};

void RetakeDetectorTests::recommendsCompleteReplacement()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("我们先去城堡"), 0, 48'000},
        {QStringLiteral("b"), QStringLiteral("我们先去城堡。"), 96'000, 160'000},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Retake, 82.0, 82.0, 60.0, 0, 0, 3},
        {1, 0, ScriptMatchStatus::Match, 98.0, 98.0, 100.0, 0, 0, 6},
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
        {0, 0, ScriptMatchStatus::Match, 95.0, 95.0, 100.0, 0, 0, 4},
        {1, 0, ScriptMatchStatus::Retake, 75.0, 75.0, 70.0, 0, 0, 4},
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
        {0, 0, ScriptMatchStatus::Match, 100.0, 100.0, 100.0, 0, 0, 4},
        {1, 0, ScriptMatchStatus::Retake, 100.0, 100.0, 100.0, 0, 0, 4},
    };
    QVERIFY(RetakeDetector::detect(recording, matches, 48'000).isEmpty());
}

void RetakeDetectorTests::classifiesRetakeInterruptedDuplicateAndWrongTake()
{
    const auto classify = [](const QString &failedText, double similarity,
                             double coverage) -> RoughCutFailureType {
        const QVector<RecognizedPassage> recording{
            {QStringLiteral("failed"), failedText, 0, 48'000},
            {QStringLiteral("good"), QStringLiteral("今天我们介绍自动粗剪。"), 50'000, 100'000},
        };
        const QVector<ScriptMatch> matches{
            {0, 0, ScriptMatchStatus::Retake, similarity, similarity, coverage, 0, 0, 4},
            {1, 0, ScriptMatchStatus::Match, 100.0, 100.0, 100.0, 0, 0, 12},
        };
        const auto groups = RetakeDetector::detect(recording, matches, 48'000);
        if (groups.size() != 1 || groups.constFirst().recommendedRecordingIndex != 1)
            return RoughCutFailureType::None;
        return groups.constFirst().takes.constFirst().failureType;
    };

    QCOMPARE(classify(QStringLiteral("不对，重来"), 70.0, 50.0),
        RoughCutFailureType::Retake);
    QCOMPARE(classify(QStringLiteral("今天我们介绍"), 80.0, 55.0),
        RoughCutFailureType::Interrupted);
    QCOMPARE(classify(QStringLiteral("今天我们介绍自动粗剪。"), 82.0, 82.0),
        RoughCutFailureType::Duplicate);
    QCOMPARE(classify(QStringLiteral("今天我们去吃饭。"), 70.0, 60.0),
        RoughCutFailureType::WrongTake);
}

void RetakeDetectorTests::doesNotGroupAdjacentDifferentContentOnSameLine()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("今天我们介绍"), 0, 48'000},
        {QStringLiteral("b"), QStringLiteral("自动粗剪"), 50'000, 90'000}};
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Match, 100.0, 100.0, 55.0, 0, 0, 6},
        {1, 0, ScriptMatchStatus::Match, 100.0, 100.0, 45.0, 0, 6, 10}};
    QVERIFY(RetakeDetector::detect(recording, matches, 48'000).isEmpty());
}

void RetakeDetectorTests::selectsLaterIdenticalCompleteTake()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("今天介绍自动粗剪。"), 0, 48'000},
        {QStringLiteral("b"), QStringLiteral("今天介绍自动粗剪。"), 50'000, 98'000}};
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Match, 100.0, 100.0, 100.0, 0, 0, 8},
        {1, 0, ScriptMatchStatus::Retake, 100.0, 100.0, 100.0, 0, 0, 8}};
    const auto groups = RetakeDetector::detect(recording, matches, 48'000);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.constFirst().recommendedRecordingIndex, 1);
}

void RetakeDetectorTests::prefersMultiLineCoverageOverShortPerfectPrefix()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("short"), QStringLiteral("下一个镜头是相杀小陀"), 0, 48'000},
        {QStringLiteral("full"), QStringLiteral("下一个镜头是相杀小陀螺成功后恢复了鬼人槽"),
            50'000, 110'000}};
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Match, 100.0, 100.0, 90.0, 0, 0, 10},
        {1, 0, ScriptMatchStatus::Match, 90.0, 90.0, 95.0, 1, 0, 23}};
    const auto groups = RetakeDetector::detect(recording, matches, 48'000);
    QCOMPARE(groups.size(), 1);
    QCOMPARE(groups.constFirst().recommendedRecordingIndex, 1);
}

QTEST_MAIN(RetakeDetectorTests)
#include "test_retake_detector.moc"
