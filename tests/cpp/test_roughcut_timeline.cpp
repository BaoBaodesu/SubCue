#include "roughcut/timeline_engine.h"

#include <QtTest/QTest>

using namespace subcue;

class RoughCutTimelineTests final : public QObject {
    Q_OBJECT

private slots:
    void excludesCutAndKeepsSourceOrder();
    void clampsGapsAndProtectsCuts();
    void retainsSentenceEndingAfterAlignedWord();
};

void RoughCutTimelineTests::excludesCutAndKeepsSourceOrder()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("A"), 4'800, 48'000},
        {QStringLiteral("b"), QStringLiteral("B"), 96'000, 144'000},
        {QStringLiteral("c"), QStringLiteral("C"), 192'000, 240'000},
    };
    const QVector<RoughCutSegmentDecision> decisions{
        {0, RoughCutDecision::Keep}, {1, RoughCutDecision::Cut}, {2, RoughCutDecision::Review}};
    const auto timeline = RoughCutTimelineEngine::build(recording, decisions, 48'000, 288'000);
    QCOMPARE(timeline.size(), 2);
    QCOMPARE(timeline.at(0).text, QStringLiteral("A"));
    QCOMPARE(timeline.at(1).text, QStringLiteral("C"));
    QCOMPARE(timeline.at(0).recordingIndex, 0);
    QCOMPARE(timeline.at(1).recordingIndex, 2);
    QCOMPARE(timeline.at(1).decision, RoughCutDecision::Review);
}

void RoughCutTimelineTests::clampsGapsAndProtectsCuts()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("A"), 4'800, 48'000},
        {QStringLiteral("bad"), QStringLiteral("失败"), 50'000, 70'000},
        {QStringLiteral("b"), QStringLiteral("B"), 240'000, 280'000},
    };
    const QVector<RoughCutSegmentDecision> decisions{
        {0, RoughCutDecision::Keep}, {1, RoughCutDecision::Cut}, {2, RoughCutDecision::Keep}};
    const auto timeline = RoughCutTimelineEngine::build(
        recording, decisions, 48'000, 300'000, {RoughCutGapKind::Sentence});
    QCOMPARE(timeline.at(0).sourceEndSample, 50'000);
    QCOMPARE(timeline.at(1).sourceStartSample, 236'160);
    const qint64 firstDuration = timeline.at(0).sourceEndSample - timeline.at(0).sourceStartSample;
    QCOMPARE(timeline.at(1).timelineStartSample - firstDuration, 12'000);
}

void RoughCutTimelineTests::retainsSentenceEndingAfterAlignedWord()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("一句话"), 48'000, 96'000}};
    const QVector<RoughCutSegmentDecision> decisions{{0, RoughCutDecision::Keep}};
    const auto timeline = RoughCutTimelineEngine::build(recording, decisions, 48'000, 144'000);
    QCOMPARE(timeline.size(), 1);
    QCOMPARE(timeline.constFirst().sourceEndSample, 108'000);
}

QTEST_MAIN(RoughCutTimelineTests)
#include "test_roughcut_timeline.moc"
