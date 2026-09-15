#include "roughcut/script_document.h"
#include "roughcut/script_matcher.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <variant>

using namespace subcue;

class RoughCutScriptTests final : public QObject {
    Q_OBJECT

private slots:
    void txtRequiresUtf8AndPreservesLines();
    void docxWorkerResultPreservesParagraphAndTable();
    void orderedMatcherClassifiesAllRelations();
};

void RoughCutScriptTests::txtRequiresUtf8AndPreservesLines()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("文案.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QStringLiteral("第一行\n\n第三行").toUtf8());
    file.close();
    const ScriptDocumentResult result = ScriptDocumentImporter::loadTxt(path);
    QVERIFY(std::holds_alternative<ScriptDocument>(result));
    const ScriptDocument document = std::get<ScriptDocument>(result);
    QCOMPARE(document.lines.size(), 3);
    QCOMPARE(document.lines.at(0).lineNumber, 1);
    QCOMPARE(document.lines.at(1).text, QString());
    QCOMPARE(document.lines.at(2).text, QStringLiteral("第三行"));
}

void RoughCutScriptTests::docxWorkerResultPreservesParagraphAndTable()
{
    const QJsonObject result{{QStringLiteral("lines"), QJsonArray{
        QJsonObject{{QStringLiteral("lineNumber"), 1}, {QStringLiteral("paragraph"), 1},
            {QStringLiteral("table"), false}, {QStringLiteral("text"), QStringLiteral("正文")}},
        QJsonObject{{QStringLiteral("lineNumber"), 2}, {QStringLiteral("paragraph"), 2},
            {QStringLiteral("table"), true}, {QStringLiteral("text"), QStringLiteral("表格")}}
    }}};
    const ScriptDocumentResult parsed = ScriptDocumentImporter::fromWorkerResult(result);
    QVERIFY(std::holds_alternative<ScriptDocument>(parsed));
    const ScriptDocument document = std::get<ScriptDocument>(parsed);
    QCOMPARE(document.lines.size(), 2);
    QCOMPARE(document.lines.at(0).paragraph, 1);
    QVERIFY(document.lines.at(1).table);
}

void RoughCutScriptTests::orderedMatcherClassifiesAllRelations()
{
    ScriptDocument script;
    script.lines = {
        {1, 1, false, QStringLiteral("开始游戏")},
        {2, 2, false, QStringLiteral("进入城堡")},
        {3, 3, false, QStringLiteral("获得宝剑")},
    };
    const QVector<RecognizedPassage> recording{
        {QStringLiteral("r1"), QStringLiteral("开始游戏"), 0, 100},
        {QStringLiteral("r2"), QStringLiteral("获得一把宝剑"), 100, 200},
        {QStringLiteral("r3"), QStringLiteral("获得一把宝剑"), 200, 300},
        {QStringLiteral("r4"), QStringLiteral("临时补充说明"), 300, 400},
    };
    const QVector<ScriptMatch> matches = ScriptMatcher::match(script, recording);
    QCOMPARE(matches.size(), 5);
    QCOMPARE(matches.at(0).status, ScriptMatchStatus::Match);
    QCOMPARE(matches.at(1).status, ScriptMatchStatus::Skipped);
    QCOMPARE(matches.at(2).status, ScriptMatchStatus::Modified);
    QCOMPARE(matches.at(3).status, ScriptMatchStatus::Retake);
    QCOMPARE(matches.at(4).status, ScriptMatchStatus::Added);
    QCOMPARE(matches.at(2).scriptLineIndex, 2);
    QCOMPARE(matches.at(3).scriptLineIndex, 2);
}

QTEST_APPLESS_MAIN(RoughCutScriptTests)

#include "test_roughcut_script.moc"
