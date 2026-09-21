#include "roughcut/auxiliary_recognition.h"

#include <QtTest/QTest>

using namespace subcue;

class RoughCutAuxiliaryTests final : public QObject {
    Q_OBJECT

private slots:
    void plansOnlyReviewAndMergesOverlap();
    void conflictCannotBecomeCut();
    void agreementCanResolveReview();
};

void RoughCutAuxiliaryTests::plansOnlyReviewAndMergesOverlap()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("1"), QStringLiteral("保留"), 1'000, 2'000},
        {QStringLiteral("2"), QStringLiteral("可疑一"), 3'000, 4'000},
        {QStringLiteral("3"), QStringLiteral("可疑二"), 4'050, 5'000},
    };
    QVector<RoughCutSegmentDecision> decisions(3);
    decisions[0].autoDecision = RoughCutDecision::Keep;
    decisions[1].autoDecision = RoughCutDecision::Review;
    decisions[2].autoDecision = RoughCutDecision::Review;
    const QVector<RoughCutSuspiciousRange> ranges = RoughCutAuxiliaryRecognition::plan(
        recording, decisions, 1'000, 10'000, 100, 100);
    QCOMPARE(ranges.size(), 1);
    QCOMPARE(ranges.constFirst().startSample, 2'900);
    QCOMPARE(ranges.constFirst().endSample, 5'100);
    QCOMPARE(ranges.constFirst().recordingIndexes, QVector<int>({1, 2}));
}

void RoughCutAuxiliaryTests::conflictCannotBecomeCut()
{
    RoughCutAuxiliaryResult conflict{0, QStringLiteral("正确版本"), QStringLiteral("另一版本")};
    QCOMPARE(RoughCutAuxiliaryRecognition::reconcile(RoughCutDecision::Cut, &conflict),
        RoughCutDecision::Review);
    QVERIFY(conflict.conflict);

    RoughCutAuxiliaryResult agreement{0, QStringLiteral("正确，版本。"), QStringLiteral("正确版本")};
    QCOMPARE(RoughCutAuxiliaryRecognition::reconcile(RoughCutDecision::Keep, &agreement),
        RoughCutDecision::Keep);
    QVERIFY(!agreement.conflict);
}

void RoughCutAuxiliaryTests::agreementCanResolveReview()
{
    RoughCutAuxiliaryResult agreement{0, QStringLiteral("重来"), QStringLiteral("重来")};
    agreement.agreementDecision = RoughCutDecision::Cut;
    QCOMPARE(RoughCutAuxiliaryRecognition::reconcile(RoughCutDecision::Review, &agreement),
        RoughCutDecision::Cut);
    QVERIFY(!agreement.conflict);
}

QTEST_GUILESS_MAIN(RoughCutAuxiliaryTests)
#include "test_roughcut_auxiliary.moc"
