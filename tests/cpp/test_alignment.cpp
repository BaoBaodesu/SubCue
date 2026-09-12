#include "alignment/alignment_engine.h"
#include "alignment/anchors.h"
#include "alignment/forced_align.h"
#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"
#include "alignment/python_round.h"
#include "alignment/transcript.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QStringList>

#include <QtTest/QTest>

#include <algorithm>

using namespace subcue;

namespace {

QJsonObject goldenDocument()
{
    QFile file(QStringLiteral(SUBCUE_TEST_GOLDEN_DIR "/alignment_cases.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("无法打开 alignment_cases.json：%s", qPrintable(file.errorString()));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qFatal("alignment_cases.json 无效：%s", qPrintable(error.errorString()));
    }
    return document.object();
}

QStringList jsonStringList(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray()) {
        out.append(item.toString());
    }
    return out;
}

QVector<TranscriptWord> jsonWords(const QJsonValue &value)
{
    QVector<TranscriptWord> out;
    for (const QJsonValue &item : value.toArray()) {
        const QJsonObject object = item.toObject();
        out.append(TranscriptWord{
            object.value(QStringLiteral("id")).toInteger(-1),
            object.value(QStringLiteral("text")).toString(),
            object.value(QStringLiteral("startMs")).toInteger(0),
            object.value(QStringLiteral("endMs")).toInteger(0),
        });
    }
    return out;
}

QVector<TranscriptWord> timedWords(const QStringList &values, qint64 stepMs = 500)
{
    QVector<TranscriptWord> out;
    qint64 index = 1;
    for (const QString &text : values) {
        out.append(TranscriptWord{index, text, (index - 1) * stepMs, index * stepMs});
        ++index;
    }
    return out;
}

void verifySubtitleFields(const Subtitle &actual, const QJsonObject &expected, const QString &label)
{
    const QJsonValue startWordId = expected.value(QStringLiteral("startWordId"));
    const QJsonValue endWordId = expected.value(QStringLiteral("endWordId"));
    QVERIFY2(actual.text == expected.value(QStringLiteral("text")).toString(),
        qPrintable(label + QStringLiteral(" text")));
    QVERIFY2(actual.start.milliseconds() == expected.value(QStringLiteral("startMs")).toInteger(-1),
        qPrintable(label + QStringLiteral(" startMs")));
    QVERIFY2(actual.end.milliseconds() == expected.value(QStringLiteral("endMs")).toInteger(-1),
        qPrintable(label + QStringLiteral(" endMs")));
    QVERIFY2(actual.confidence == expected.value(QStringLiteral("confidence")).toDouble(0.0),
        qPrintable(label + QStringLiteral(" confidence")));
    QVERIFY2(actual.source == expected.value(QStringLiteral("source")).toString(QStringLiteral("local")),
        qPrintable(label + QStringLiteral(" source")));
    QVERIFY2(actual.status == expected.value(QStringLiteral("status")).toString(),
        qPrintable(label + QStringLiteral(" status")));
    QVERIFY2(actual.startWordId == (startWordId.isNull() ? -1 : startWordId.toInteger()),
        qPrintable(label + QStringLiteral(" startWordId")));
    QVERIFY2(actual.endWordId == (endWordId.isNull() ? -1 : endWordId.toInteger()),
        qPrintable(label + QStringLiteral(" endWordId")));
    QVERIFY2(actual.candidateText == expected.value(QStringLiteral("candidateText")).toString(),
        qPrintable(label + QStringLiteral(" candidateText")));
    QVERIFY2(actual.ambiguity == expected.value(QStringLiteral("ambiguity")).toDouble(1.0),
        qPrintable(label + QStringLiteral(" ambiguity")));
    QVERIFY2(actual.skipReason == expected.value(QStringLiteral("skipReason")).toString(),
        qPrintable(label + QStringLiteral(" skipReason")));
}

} // namespace

class AlignmentTests final : public QObject {
    Q_OBJECT

private slots:
    void fuzzRatioMatchesGolden();
    void normalizeMatchesGolden();
    void pythonRoundMatchesGolden();
    void alignMatchesGolden();
    void finalizeMatchesGolden();
    void forcedAlignMatchesGolden();
    void alignmentGoldenFixtureStructureIsStable();
    void transcriptSortAndReindex();
    void confidenceAndReviewThresholds();
    void subtitleAlignmentFieldsHavePythonDefaults();
    void pythonAlignmentTestSemantics();
    void forcedAlignSemantics();
    void audioFirstSemantics();
};

void AlignmentTests::fuzzRatioMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("ratioCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const QString left = object.value(QStringLiteral("a")).toString();
        const QString right = object.value(QStringLiteral("b")).toString();
        const double expected = object.value(QStringLiteral("score")).toDouble();
        const double actual = FuzzRatio::ratio(left, right);
        QVERIFY2(actual == expected,
            qPrintable(QStringLiteral("ratio[%1] %2 vs %3: %4 != %5")
                .arg(index)
                .arg(left, right)
                .arg(actual, 0, 'g', 17)
                .arg(expected, 0, 'g', 17)));
    }
}

void AlignmentTests::normalizeMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("normalizeCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const QString input = object.value(QStringLiteral("input")).toString();
        const QString expected = object.value(QStringLiteral("output")).toString();
        const QString actual = AlignmentEngine::normalizeText(input);
        QVERIFY2(actual == expected,
            qPrintable(QStringLiteral("normalize[%1] %2: %3 != %4")
                .arg(index)
                .arg(input, expected, actual)));
    }
}

void AlignmentTests::pythonRoundMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("roundCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const double input = object.value(QStringLiteral("input")).toDouble();
        const qint64 expected = object.value(QStringLiteral("output")).toInteger();
        const qint64 actual = pythonRound(input);
        QCOMPARE(actual, expected);
    }
}

void AlignmentTests::alignMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("alignCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        const AlignmentResult result = AlignmentEngine::alignSubtitleLines(
            jsonStringList(object.value(QStringLiteral("lines"))),
            jsonWords(object.value(QStringLiteral("words"))));
        const QJsonArray expected = object.value(QStringLiteral("subtitles")).toArray();
        QVERIFY2(result.subtitles.size() == expected.size(),
            qPrintable(QStringLiteral("%1 字幕数量 %2 != %3")
                .arg(name).arg(result.subtitles.size()).arg(expected.size())));
        for (qsizetype k = 0; k < expected.size(); ++k) {
            verifySubtitleFields(result.subtitles[k], expected.at(k).toObject(),
                QStringLiteral("%1[%2]").arg(name).arg(k));
        }
    }
}

void AlignmentTests::finalizeMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("finalizeCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        AlignmentResult result = AlignmentEngine::alignSubtitleLines(
            jsonStringList(object.value(QStringLiteral("lines"))),
            jsonWords(object.value(QStringLiteral("words"))));
        const QVector<Subtitle> candidates = result.subtitles;
        AlignmentEngine::finalizeAudioFirst(result.subtitles);
        const QJsonArray expected = object.value(QStringLiteral("subtitles")).toArray();
        QVERIFY2(result.subtitles.size() == expected.size(),
            qPrintable(QStringLiteral("%1 字幕数量 %2 != %3")
                .arg(name).arg(result.subtitles.size()).arg(expected.size())));
        for (qsizetype k = 0; k < expected.size(); ++k) {
            // 旧 golden 会清零低分候选；新契约保留已有连续音频区间。
            if (candidates[k].isTimed() && candidates[k].confidence < 0.70
                && candidates[k].startWordId >= 0 && candidates[k].endWordId >= candidates[k].startWordId) {
                QCOMPARE(result.subtitles[k].start, candidates[k].start);
                QCOMPARE(result.subtitles[k].end, candidates[k].end);
                QCOMPARE(result.subtitles[k].text, candidates[k].text);
                QCOMPARE(result.subtitles[k].status, QStringLiteral("LOW_CONFIDENCE"));
                continue;
            }
            verifySubtitleFields(result.subtitles[k], expected.at(k).toObject(),
                QStringLiteral("%1[%2]").arg(name).arg(k));
        }
    }
}

void AlignmentTests::forcedAlignMatchesGolden()
{
    const QJsonArray cases = goldenDocument().value(QStringLiteral("forcedCases")).toArray();
    QVERIFY(!cases.isEmpty());
    for (qsizetype index = 0; index < cases.size(); ++index) {
        const QJsonObject object = cases.at(index).toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            jsonStringList(object.value(QStringLiteral("lines"))),
            jsonWords(object.value(QStringLiteral("words"))),
            object.value(QStringLiteral("durationMs")).toInteger(0));
        const QJsonArray expected = object.value(QStringLiteral("subtitles")).toArray();
        QVERIFY2(result.subtitles.size() == expected.size(),
            qPrintable(QStringLiteral("%1 字幕数量 %2 != %3")
                .arg(name).arg(result.subtitles.size()).arg(expected.size())));
        for (qsizetype k = 0; k < expected.size(); ++k) {
            verifySubtitleFields(result.subtitles[k], expected.at(k).toObject(),
                QStringLiteral("%1[%2]").arg(name).arg(k));
        }
    }
}

void AlignmentTests::alignmentGoldenFixtureStructureIsStable()
{
    // Phase 0 冻结的脱敏对齐结果样例：锁定字段名、类型与关键值，防止格式漂移。
    QFile file(QStringLiteral(SUBCUE_TEST_GOLDEN_DIR "/alignment_result.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(error.errorString()));
    const QJsonArray subtitles = document.object().value(QStringLiteral("subtitles")).toArray();
    QCOMPARE(subtitles.size(), qsizetype(2));

    const QJsonObject first = subtitles.at(0).toObject();
    QVERIFY(!first.value(QStringLiteral("id")).toString().isEmpty());
    QCOMPARE(first.value(QStringLiteral("text")).toString(), QStringLiteral("Hello 各位观众朋友们好"));
    QCOMPARE(first.value(QStringLiteral("startUs")).toInteger(), qint64(120000));
    QCOMPARE(first.value(QStringLiteral("endUs")).toInteger(), qint64(1680000));
    QCOMPARE(first.value(QStringLiteral("confidence")).toDouble(), 0.96);
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("MATCHED"));
    const QJsonArray firstWordIds = first.value(QStringLiteral("wordIds")).toArray();
    QCOMPARE(firstWordIds.size(), qsizetype(2));
    QCOMPARE(firstWordIds.at(0).toInteger(), qint64(0));
    QCOMPARE(firstWordIds.at(1).toInteger(), qint64(1));

    const QJsonObject second = subtitles.at(1).toObject();
    QCOMPARE(second.value(QStringLiteral("text")).toString(), QStringLiteral("未找到音频"));
    QCOMPARE(second.value(QStringLiteral("startUs")).toInteger(), qint64(0));
    QCOMPARE(second.value(QStringLiteral("endUs")).toInteger(), qint64(0));
    QCOMPARE(second.value(QStringLiteral("status")).toString(), QStringLiteral("SKIPPED_NO_AUDIO"));
    const QJsonArray secondWordIds = second.value(QStringLiteral("wordIds")).toArray();
    QCOMPARE(secondWordIds.size(), qsizetype(2));
    QVERIFY(secondWordIds.at(0).isNull());
    QVERIFY(secondWordIds.at(1).isNull());
}

void AlignmentTests::transcriptSortAndReindex()
{
    Transcript transcript;
    transcript.words = {
        TranscriptWord{10, QStringLiteral("b"), 500, 900},
        TranscriptWord{20, QStringLiteral("a"), 500, 700},
        TranscriptWord{30, QStringLiteral("c"), 100, 400},
    };
    transcript.sortAndReindex();
    QCOMPARE(transcript.words.size(), qsizetype(3));
    QCOMPARE(transcript.words[0].text, QStringLiteral("c"));
    QCOMPARE(transcript.words[0].id, qint64(1));
    QCOMPARE(transcript.words[1].text, QStringLiteral("a"));
    QCOMPARE(transcript.words[1].id, qint64(2));
    QCOMPARE(transcript.words[2].text, QStringLiteral("b"));
    QCOMPARE(transcript.words[2].id, qint64(3));
}

void AlignmentTests::confidenceAndReviewThresholds()
{
    QVERIFY(AlignmentEngine::calculateConfidence(1.0, 1.0, 1.0) == 1.0);
    QVERIFY(AlignmentEngine::calculateConfidence(0.0, 0.0, 0.0) == 0.0);
    QVERIFY(AlignmentEngine::calculateConfidence(1.0, 0.0, 0.5) == 0.905);
    QVERIFY(AlignmentEngine::needsAiReview(0.74, 0.5));
    QVERIFY(!AlignmentEngine::needsAiReview(0.76, 0.5));
    QVERIFY(AlignmentEngine::needsAiReview(0.90, 0.03));
}

void AlignmentTests::subtitleAlignmentFieldsHavePythonDefaults()
{
    const Subtitle subtitle;
    QVERIFY(subtitle.candidateText.isEmpty());
    QCOMPARE(subtitle.ambiguity, 1.0);
    QVERIFY(subtitle.skipReason.isEmpty());
}

void AlignmentTests::pythonAlignmentTestSemantics()
{
    // test_alignment.py::test_line_can_span_multiple_asr_words
    {
        const AlignmentResult result = AlignmentEngine::alignSubtitleLines(
            {QStringLiteral("今天我们讨论《怪物猎人：荒野》DLC"), QStringLiteral("内容")},
            timedWords({QStringLiteral("今天"), QStringLiteral("我们"), QStringLiteral("讨论"),
                QStringLiteral("怪物猎人"), QStringLiteral("荒野"), QStringLiteral("DLC"),
                QStringLiteral("内容")}));
        QCOMPARE(result.subtitles.size(), qsizetype(2));
        QCOMPARE(result.subtitles[0].text, QStringLiteral("今天我们讨论《怪物猎人：荒野》DLC"));
        QCOMPARE(result.subtitles[0].startWordId, qint64(1));
        QCOMPARE(result.subtitles[0].endWordId, qint64(6));
        QVERIFY(result.subtitles[0].end <= result.subtitles[1].start);
    }
    // test_alignment.py::test_asr_segmentation_does_not_define_subtitles
    {
        const AlignmentResult result = AlignmentEngine::alignSubtitleLines(
            {QStringLiteral("这个角色目前的整体表现还是比较不错的")},
            timedWords({QStringLiteral("这个角色目前的整体表现"), QStringLiteral("还是比较不错的")}));
        QCOMPARE(result.subtitles[0].startWordId, qint64(1));
        QCOMPARE(result.subtitles[0].endWordId, qint64(2));
        QVERIFY(result.subtitles[0].confidence >= 0.85);
    }
    // test_alignment.py::test_output_order_is_monotonic
    {
        const QStringList lines = {
            QStringLiteral("开场"), QStringLiteral("相同内容"), QStringLiteral("中间"),
            QStringLiteral("相同内容"), QStringLiteral("结尾"),
        };
        const AlignmentResult result = AlignmentEngine::alignSubtitleLines(lines, timedWords(lines));
        QVector<qint64> starts;
        for (const Subtitle &subtitle : result.subtitles) {
            starts.append(subtitle.start.milliseconds());
        }
        QVERIFY(std::is_sorted(starts.begin(), starts.end()));
        bool hasLowConfidenceDuplicate = false;
        for (const Subtitle &subtitle : result.subtitles) {
            if (subtitle.text == QStringLiteral("相同内容")
                && subtitle.status == QStringLiteral("LOW_CONFIDENCE")) {
                hasLowConfidenceDuplicate = true;
            }
        }
        QVERIFY(hasLowConfidenceDuplicate);
    }
}

void AlignmentTests::forcedAlignSemantics()
{
    // test_forced_align.py::test_all_lines_are_placed
    {
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            {QStringLiteral("今天我们讨论"), QStringLiteral("这是额外的行"), QStringLiteral("内容")},
            timedWords({QStringLiteral("今天"), QStringLiteral("我们"), QStringLiteral("讨论"),
                QStringLiteral("内容")}),
            2000);
        QCOMPARE(result.subtitles.size(), qsizetype(3));
        for (const Subtitle &subtitle : result.subtitles) {
            QVERIFY2(subtitle.end > subtitle.start,
                qPrintable(QStringLiteral("subtitle %1 时长为零").arg(subtitle.id)));
        }
    }
    // test_forced_align.py::test_no_zero_duration
    {
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            {QStringLiteral("第一行"), QStringLiteral("第二行"), QStringLiteral("第三行")},
            timedWords({QStringLiteral("完全不匹配的文字")}),
            1500);
        QCOMPARE(result.subtitles.size(), qsizetype(3));
        for (const Subtitle &subtitle : result.subtitles) {
            QVERIFY(subtitle.end.milliseconds() - subtitle.start.milliseconds() >= 250);
        }
    }
    // test_forced_align.py::test_monotonic_order
    {
        const QStringList lines = {
            QStringLiteral("开场"), QStringLiteral("相同内容"), QStringLiteral("中间"),
            QStringLiteral("相同内容"), QStringLiteral("结尾"),
        };
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            lines, timedWords(lines), 2500);
        QVector<qint64> starts;
        for (const Subtitle &subtitle : result.subtitles) {
            starts.append(subtitle.start.milliseconds());
        }
        QVERIFY(std::is_sorted(starts.begin(), starts.end()));
    }
    // test_forced_align.py::test_count_equals_lines
    {
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            {QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"),
                QStringLiteral("四"), QStringLiteral("五")},
            timedWords({QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三")}),
            3000);
        QCOMPARE(result.subtitles.size(), qsizetype(5));
    }
    // test_forced_align.py::test_empty_words
    {
        const AlignmentResult result = ForcedAligner::forcedAlignSubtitleLines(
            {QStringLiteral("行A"), QStringLiteral("行B")}, {}, 2000);
        QCOMPARE(result.subtitles.size(), qsizetype(2));
        for (const Subtitle &subtitle : result.subtitles) {
            QVERIFY(subtitle.end > subtitle.start);
        }
    }
}

void AlignmentTests::audioFirstSemantics()
{
    // test_audio_first_alignment.py::test_missing_script_line_is_kept_but_not_timed_or_exported
    const QVector<TranscriptWord> words = {
        TranscriptWord{1, QStringLiteral("开场"), 0, 500},
        TranscriptWord{2, QStringLiteral("实际内容"), 500, 1200},
        TranscriptWord{3, QStringLiteral("结尾"), 1200, 1700},
    };
    AlignmentResult result = AlignmentEngine::alignSubtitleLines(
        {QStringLiteral("开场"), QStringLiteral("音频里完全没有的广告描述"),
            QStringLiteral("实际内容"), QStringLiteral("结尾")},
        words);
    AlignmentEngine::finalizeAudioFirst(result.subtitles);
    int skipped = 0;
    for (const Subtitle &subtitle : result.subtitles) {
        if (subtitle.status == QStringLiteral("SKIPPED_NO_AUDIO")) {
            ++skipped;
            QCOMPARE(subtitle.start.milliseconds(), qint64(0));
            QCOMPARE(subtitle.end.milliseconds(), qint64(0));
        }
    }
    QVERIFY(skipped > 0);
    QCOMPARE(result.subtitles.size(), qsizetype(4));
    QVERIFY(result.exportableSubtitles().size() < 4);
}

QTEST_APPLESS_MAIN(AlignmentTests)

#include "test_alignment.moc"
