#include "rough_cut_result_model.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace subcue;

class RoughCutResultModelTests final : public QObject {
    Q_OBJECT

private slots:
    void exposesDecisionAndSourceTimes();
    void userDecisionOverridesAndRestoresAuto();
    void replaceBaseDecisionsPreservesUserOverride();
    void manualCutOfReplacementSuspendsDependentAutoCut();
};

void RoughCutResultModelTests::exposesDecisionAndSourceTimes()
{
    RoughCutResultModel model;
    RoughCutSegmentDecision decision{0, RoughCutDecision::Review, std::nullopt, 0.0,
        QStringLiteral("需要复核"), {QStringLiteral("边界不可信")}};
    decision.failureType = RoughCutFailureType::Interrupted;
    decision.modelProbability = 0.73;
    decision.decisionSource = QStringLiteral("omni");
    model.reset({{QStringLiteral("1"), QStringLiteral("测试"), 4'800, 48'000}},
                {decision}, 48'000);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
             QStringLiteral("REVIEW"));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StartMsRole).toLongLong(), 100);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::EndMsRole).toLongLong(), 1000);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::FailureTypeRole).toString(),
        QStringLiteral("INTERRUPTED"));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::ModelProbabilityRole).toDouble(), 0.73);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::DecisionSourceRole).toString(),
        QStringLiteral("omni"));
}

void RoughCutResultModelTests::userDecisionOverridesAndRestoresAuto()
{
    RoughCutResultModel model;
    model.reset({{QStringLiteral("1"), QStringLiteral("测试"), 0, 48'000}},
                {{0, RoughCutDecision::Review}}, 48'000);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QVERIFY(model.setUserDecision(0, RoughCutDecision::Cut));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
             QStringLiteral("CUT"));
    QVERIFY(model.data(model.index(0), RoughCutResultModel::UserOverrideRole).toBool());
    QVERIFY(model.setUserDecision(0, std::nullopt));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
             QStringLiteral("REVIEW"));
    QCOMPARE(changed.count(), 2);
}

void RoughCutResultModelTests::replaceBaseDecisionsPreservesUserOverride()
{
    RoughCutResultModel model;
    RoughCutSegmentDecision autoCut{0, RoughCutDecision::Review};
    model.reset({{QStringLiteral("1"), QStringLiteral("测试"), 0, 48'000}}, {autoCut}, 48'000);
    QVERIFY(model.setUserDecision(0, RoughCutDecision::Keep));
    RoughCutSegmentDecision updated{0, RoughCutDecision::Cut};
    updated.decisionSource = QStringLiteral("omni");
    model.replaceBaseDecisions({updated}, true);
    QVERIFY(model.data(model.index(0), RoughCutResultModel::UserOverrideRole).toBool());
    QVERIFY(model.baseDecisions().at(0).userDecision.has_value());
    QCOMPARE(*model.baseDecisions().at(0).userDecision, RoughCutDecision::Keep);
    QCOMPARE(model.baseDecisions().at(0).autoDecision, RoughCutDecision::Cut);
    model.replaceBaseDecisions({updated}, false);
    QVERIFY(!model.baseDecisions().at(0).userDecision.has_value());
    QCOMPARE(model.baseDecisions().at(0).autoDecision, RoughCutDecision::Cut);
}

void RoughCutResultModelTests::manualCutOfReplacementSuspendsDependentAutoCut()
{
    RoughCutResultModel model;
    RecognizedPassage failed{QStringLiteral("failed"), QStringLiteral("今天介绍"), 0, 500};
    failed.scriptTokenStart = 0;
    failed.scriptTokenEnd = 4;
    failed.boundaryTrustworthy = true;
    failed.preciseTiming = true;
    RecognizedPassage replacement{QStringLiteral("good"), QStringLiteral("今天介绍自动粗剪"), 600, 1'200};
    replacement.scriptTokenStart = 0;
    replacement.scriptTokenEnd = 8;
    replacement.boundaryTrustworthy = true;
    replacement.preciseTiming = true;
    RoughCutSegmentDecision cut{0, RoughCutDecision::Cut};
    cut.failureType = RoughCutFailureType::Interrupted;
    cut.replacementRecordingIndex = 1;
    model.reset({failed, replacement}, {cut, {1, RoughCutDecision::Keep}}, 1'000,
        QStringLiteral("今天介绍自动粗剪"));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
        QStringLiteral("CUT"));
    QVERIFY(model.setUserDecision(1, RoughCutDecision::Cut));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
        QStringLiteral("REVIEW"));
    QVERIFY(model.setUserDecision(1, std::nullopt));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
        QStringLiteral("CUT"));
}

QTEST_MAIN(RoughCutResultModelTests)
#include "test_rough_cut_result_model.moc"
