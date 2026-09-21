#include "ai/ai_review_settings.h"
#include "roughcut/safe_cut_boundary.h"

#include <QtTest/QTest>

using namespace subcue;

class SafeCutBoundaryTests final : public QObject {
    Q_OBJECT

private slots:
    void neverCutsAtExactWordEndWithoutSilence();
    void prefersSilenceAfterTail();
    void keepWinsOverOverlappingCut();
    void timelineAddsRecoverableHandle();
};

void SafeCutBoundaryTests::neverCutsAtExactWordEndWithoutSilence()
{
    OmniReviewSettings settings;
    const qint64 wordEnd = 16'000;
    const qint64 tail = SafeCutBoundary::resolveKeepTail(wordEnd, {}, 16'000, 160'000, settings);
    QVERIFY(tail >= wordEnd + 16'000 * settings.minTailMs / 1000);
    QVERIFY(tail != wordEnd);
}

void SafeCutBoundaryTests::prefersSilenceAfterTail()
{
    OmniReviewSettings settings;
    const qint64 wordEnd = 16'000;
    const QVector<RoughCutSilenceEvidence> silences{{18'400, 22'400, 0.001}};
    const qint64 tail = SafeCutBoundary::resolveKeepTail(wordEnd, silences, 16'000, 160'000, settings);
    QVERIFY(tail > wordEnd);
    QVERIFY(tail >= silences.at(0).startSample);
    QVERIFY(tail <= silences.at(0).endSample);
}

void SafeCutBoundaryTests::keepWinsOverOverlappingCut()
{
    OmniReviewSettings settings;
    SafeCutRange cut{15'000, 30'000};
    SafeCutRange keep{0, 16'000};
    const SafeCutRange resolved = SafeCutBoundary::resolve(cut, keep, {}, 16'000, 160'000, settings);
    QVERIFY(resolved.startSample >= keep.endSample);
    QVERIFY(resolved.startSample > keep.endSample || resolved.endSample == 0);
}

void SafeCutBoundaryTests::timelineAddsRecoverableHandle()
{
    OmniReviewSettings settings;
    settings.handleMs = 150;
    settings.preferredTailMs = 250;
    QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("保留"), 0, 16'000},
        {QStringLiteral("b"), QStringLiteral("剪掉"), 16'000, 32'000},
        {QStringLiteral("c"), QStringLiteral("再保留"), 32'000, 48'000},
    };
    QVector<RoughCutSegmentDecision> decisions{
        {0, RoughCutDecision::Keep},
        {1, RoughCutDecision::Cut},
        {2, RoughCutDecision::Keep},
    };
    const auto clips = SafeCutBoundary::buildTimeline(recording, decisions, 16'000, 64'000, settings);
    QCOMPARE(clips.size(), 2);
    QVERIFY(clips.at(0).sourceEndSample > recording.at(0).endSample
            || clips.at(0).sourceStartSample <= recording.at(0).startSample);
}

QTEST_MAIN(SafeCutBoundaryTests)
#include "test_safe_cut_boundary.moc"
