#include "roughcut/decision_engine.h"
#include "roughcut/retake_detector.h"

#include <QtTest/QTest>

using namespace subcue;

namespace {
RecognizedPassage timedPassage(const QString &id, const QString &text,
                               qint64 start, qint64 end, int tokenEnd)
{
    RecognizedPassage passage{id, text, start, end};
    passage.boundaryTrustworthy = true;
    passage.preciseTiming = true;
    passage.scriptTokenStart = 0;
    passage.scriptTokenEnd = tokenEnd;
    return passage;
}
}

class RoughCutDecisionTests final : public QObject {
    Q_OBJECT

private slots:
    void cutsOnlyFailedTakeWithTrustedReplacement();
    void keepsUntrustedAndAmbiguousForReview();
    void cutsStandaloneFiller();
    void cutsWrongTakeWithReplacement();
    void userDecisionOverridesAndCanBeCleared();
    void neverCutsUniqueScriptContent();
    void rejectsCoarseTiming();
    void cutsStandaloneRestartCueOnlyWhenScriptIsCovered();
    void endToEndRulesKeepScriptAndCutFiveFailureTypes();
    void replacementMayInsertAsrWordButCannotOmitScriptWord();
};

void RoughCutDecisionTests::cutsOnlyFailedTakeWithTrustedReplacement()
{
    const QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("a"), QStringLiteral("开场中断"), 0, 10, 2),
        timedPassage(QStringLiteral("b"), QStringLiteral("完整开场。"), 20, 40, 4),
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
        timedPassage(QStringLiteral("a"), QStringLiteral("重复"), 0, 10, 2),
        timedPassage(QStringLiteral("b"), QStringLiteral("重复。"), 20, 40, 2),
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

void RoughCutDecisionTests::cutsStandaloneFiller()
{
    QVector<RecognizedPassage> recording{
        {QStringLiteral("filler"), QStringLiteral("呃"), 0, 4'800}};
    recording[0].boundaryTrustworthy = true;
    const QVector<ScriptMatch> matches{
        {0, -1, ScriptMatchStatus::Added, 0.0}};
    const auto decisions = RoughCutDecisionEngine::decide(recording, matches, {}, {true});
    QCOMPARE(decisions.constFirst().autoDecision, RoughCutDecision::Cut);
    QCOMPARE(decisions.constFirst().failureType, RoughCutFailureType::Filler);
}

void RoughCutDecisionTests::cutsWrongTakeWithReplacement()
{
    const QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("wrong"), QStringLiteral("错误内容。"), 0, 10, 2),
        timedPassage(QStringLiteral("good"), QStringLiteral("正确内容。"), 20, 40, 4),
    };
    const QVector<ScriptMatch> matches{
        {0, 0, ScriptMatchStatus::Retake, 60.0},
        {1, 0, ScriptMatchStatus::Match, 100.0},
    };
    RoughCutTake wrong{0, 50.0, {QStringLiteral("明显偏离文案")}};
    wrong.failureType = RoughCutFailureType::WrongTake;
    const RoughCutRetakeGroup group{{wrong,
        {1, 110.0, {QStringLiteral("句子完整")}}}, 1, false, QStringLiteral("有正确替代"), 0};
    const auto decisions = RoughCutDecisionEngine::decide(recording, matches, {group}, {true, true});
    QCOMPARE(decisions.constFirst().autoDecision, RoughCutDecision::Cut);
    QCOMPARE(decisions.constFirst().failureType, RoughCutFailureType::WrongTake);
}

void RoughCutDecisionTests::userDecisionOverridesAndCanBeCleared()
{
    RoughCutSegmentDecision decision{0, RoughCutDecision::Review};
    decision.userDecision = RoughCutDecision::Cut;
    QCOMPARE(decision.effectiveDecision(), RoughCutDecision::Cut);
    decision.userDecision.reset();
    QCOMPARE(decision.effectiveDecision(), RoughCutDecision::Review);
}

void RoughCutDecisionTests::neverCutsUniqueScriptContent()
{
    const QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("unique"), QStringLiteral("获得宝剑"), 0, 10, 4),
        timedPassage(QStringLiteral("replacement"), QStringLiteral("进入城堡"), 20, 40, 4)};
    QVector<RoughCutSegmentDecision> decisions{
        {0, RoughCutDecision::Cut}, {1, RoughCutDecision::Keep}};
    RoughCutDecisionEngine::protectCuts(recording, &decisions,
        QStringLiteral("进入城堡获得宝剑"));
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Review);
}

void RoughCutDecisionTests::rejectsCoarseTiming()
{
    QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("failed"), QStringLiteral("今天介绍"), 0, 10, 4),
        timedPassage(QStringLiteral("replacement"), QStringLiteral("今天介绍完整内容"), 20, 40, 8)};
    recording[0].preciseTiming = false;
    QVector<RoughCutSegmentDecision> decisions{
        {0, RoughCutDecision::Cut}, {1, RoughCutDecision::Keep}};
    RoughCutDecisionEngine::protectCuts(recording, &decisions);
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Review);
}

void RoughCutDecisionTests::cutsStandaloneRestartCueOnlyWhenScriptIsCovered()
{
    QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("cue"), QStringLiteral("不对重来"), 0, 10, 0),
        timedPassage(QStringLiteral("good"), QStringLiteral("今天介绍自动粗剪"), 20, 40, 8)};
    recording[0].scriptTokenStart = -1;
    recording[0].scriptTokenEnd = -1;
    const QVector<ScriptMatch> matches{
        {0, -1, ScriptMatchStatus::Added},
        {1, 0, ScriptMatchStatus::Match, 100.0, 100.0, 100.0, 0, 0, 8}};
    const auto decisions = RoughCutDecisionEngine::decide(recording, matches, {},
        {true, true}, QStringLiteral("今天介绍自动粗剪"));
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Cut);
    QCOMPARE(decisions.at(0).failureType, RoughCutFailureType::Retake);
    QCOMPARE(decisions.at(1).autoDecision, RoughCutDecision::Keep);
}

void RoughCutDecisionTests::endToEndRulesKeepScriptAndCutFiveFailureTypes()
{
    const QString scriptText = QStringLiteral("今天我们介绍自动粗剪");
    const ScriptDocument script{{{1, 0, false, scriptText}}};
    const QVector<QString> failedTexts{
        QStringLiteral("不对重来"),
        QStringLiteral("今天我们介绍"),
        scriptText,
        QStringLiteral("今天我们介绍人工粗剪"),
        QStringLiteral("呃")};
    const QVector<RoughCutFailureType> types{
        RoughCutFailureType::Retake, RoughCutFailureType::Interrupted,
        RoughCutFailureType::Duplicate, RoughCutFailureType::WrongTake,
        RoughCutFailureType::Filler};
    for (int caseIndex = 0; caseIndex < failedTexts.size(); ++caseIndex) {
        QVector<RecognizedPassage> recording{
            timedPassage(QStringLiteral("failed"), failedTexts.at(caseIndex), 0, 500, -1),
            timedPassage(QStringLiteral("good"), scriptText, 600, 1'200, -1)};
        const QVector<ScriptMatch> matches = ScriptMatcher::match(script, recording);
        for (const ScriptMatch &match : matches) {
            if (match.recordingIndex < 0) continue;
            recording[match.recordingIndex].scriptLineIndex = match.scriptLineIndex;
            recording[match.recordingIndex].scriptLineEndIndex = match.scriptLineEndIndex;
            recording[match.recordingIndex].scriptTokenStart = match.scriptTokenStart;
            recording[match.recordingIndex].scriptTokenEnd = match.scriptTokenEnd;
        }
        const auto groups = RetakeDetector::detect(recording, matches, 1'000);
        const auto decisions = RoughCutDecisionEngine::decide(recording, matches, groups,
            {true, true}, scriptText);
        QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Cut);
        QCOMPARE(decisions.at(0).failureType, types.at(caseIndex));
        QCOMPARE(decisions.at(1).autoDecision, RoughCutDecision::Keep);
    }
}

void RoughCutDecisionTests::replacementMayInsertAsrWordButCannotOmitScriptWord()
{
    QVector<RecognizedPassage> recording{
        timedPassage(QStringLiteral("old"), QStringLiteral("已经出现呆猫也能打相杀"), 0, 500, 11),
        timedPassage(QStringLiteral("new"), QStringLiteral("已经出现了呆猫也能打相杀"), 600, 1'100, 11)};
    QVector<RoughCutSegmentDecision> decisions{{0, RoughCutDecision::Cut},
        {1, RoughCutDecision::Keep}};
    RoughCutDecisionEngine::protectCuts(recording, &decisions,
        QStringLiteral("已经出现呆猫也能打相杀"));
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Cut);
    recording[1].text = QStringLiteral("已经出现了呆猫也能打相");
    decisions[0].autoDecision = RoughCutDecision::Cut;
    RoughCutDecisionEngine::protectCuts(recording, &decisions,
        QStringLiteral("已经出现呆猫也能打相杀"));
    QCOMPARE(decisions.at(0).autoDecision, RoughCutDecision::Review);
}

QTEST_MAIN(RoughCutDecisionTests)
#include "test_roughcut_decision.moc"
