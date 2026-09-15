#include "rough_cut_result_model.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace subcue;

class RoughCutResultModelTests final : public QObject {
    Q_OBJECT

private slots:
    void exposesDecisionAndSourceTimes();
    void userDecisionOverridesAndRestoresAuto();
};

void RoughCutResultModelTests::exposesDecisionAndSourceTimes()
{
    RoughCutResultModel model;
    model.reset({{QStringLiteral("1"), QStringLiteral("测试"), 4'800, 48'000}},
                {{0, RoughCutDecision::Review, std::nullopt, 0.0,
                  QStringLiteral("需要复核"), {QStringLiteral("边界不可信")}}}, 48'000);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StatusRole).toString(),
             QStringLiteral("REVIEW"));
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::StartMsRole).toLongLong(), 100);
    QCOMPARE(model.data(model.index(0), RoughCutResultModel::EndMsRole).toLongLong(), 1000);
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

QTEST_MAIN(RoughCutResultModelTests)
#include "test_rough_cut_result_model.moc"
