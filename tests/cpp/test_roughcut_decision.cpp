#include "roughcut/decision_engine.h"

#include <QtTest/QTest>

using namespace subcue;

class RoughCutDecisionTests final : public QObject {
    Q_OBJECT

private slots:
    void cutsOnlyFailedTakeWithTrustedReplacement();
    void keepsUntrustedAndAmbiguousForReview();
    void userDecisionOverridesAndCanBeCleared();
};

void RoughCutDecisionTests::cutsOnlyFailedTakeWithTrustedReplacement()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("开场中断"), 0, 10},
        {QStringLiteral("b"), QStringLiteral("完整开场。"), 20, 40},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Retake, 75.0},
        {1, 0, ScriptMatchStatus::Match, 98.0},
    };
    const QVector<RoughCutRetakeGroup> groups{{
        {{0, 75.0, {QStringLiteral("疑似中断")}},
         {1, 108.0, {QStringLiteral("句子完整")}}},
        1, false, QStringLiteral("完整版本有明确优势")}};
    const auto decisions = RoughCutDecisionEngine::decide(recording, matches, groups, {true, true});
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Cut);
    QCOMPARE(decisions.at(1).autoDecision, RoughCutDecision::Keep);
}

void RoughCutDecisionTests::keepsUntrustedAndAmbiguousForReview()
{
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("a"), QStringLiteral("重复"), 0, 10},
        {QStringLiteral("b"), QStringLiteral("重复。"), 20, 40},
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Retake, 80.0},
        {1, 0, ScriptMatchStatus::Match, 98.0},
    };
    const RoughCutRetakeGroup resolved{{
        {0, 80.0, {QStringLiteral("疑似中断")}},
        {1, 108.0, {QStringLiteral("句子完整")}}}, 1, false, QStringLiteral("有替代")};
    QCOMPARE(RoughCutDecisionEngine::decide(recording, matches, {resolved}, {false, true})
        .at(0).autoDecision, RoughCutDecision::Review);
    RoughCutRetakeGroup ambiguous = resolved;
    ambiguous.needsReview = true;
    ambiguous.recommendedRecordingIndex = -1;
    const auto decisions = RoughCutDecisionEngine::decide(recording, matches, {ambiguous}, {true, true});
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Review);
    QCOMPARE(decisions.at(1).autoDecision, RoughCutDecision::Review);
}

void RoughCutDecisionTests::userDecisionOverridesAndCanBeCleared()
{
    RoughCutSegmentDecision decision{0, RoughCutDecision::Review};
    decision.userDecision = RoughCutDecision::Cut;
    QCOMPARE(decision.effectiveDecision(), RoughCutDecision::Cut);
    decision.userDecision.reset();
    QCOMPARE(decision.effectiveDecision(), RoughCutDecision::Review);
}

QTEST_MAIN(RoughCutDecisionTests)
#include "test_roughcut_decision.moc"
