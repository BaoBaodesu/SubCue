#include "subtitle/subtitle_time_index.h"

#include <QtCore/QElapsedTimer>
#include <QtTest/QTest>

using namespace subcue;

namespace {

Subtitle makeCue(const QString &id, qint64 startMs, qint64 endMs, const QString &text)
{
    Subtitle subtitle;
    subtitle.id = id;
    subtitle.text = text;
    subtitle.start = MediaTime::fromMilliseconds(startMs);
    subtitle.end = MediaTime::fromMilliseconds(endMs);
    subtitle.source = QStringLiteral("asr");
    subtitle.status = QStringLiteral("MATCHED");
    return subtitle;
}

int naiveIndex(const QList<Subtitle> &subtitles, qint64 positionMs)
{
    for (int index = 0; index < subtitles.size(); ++index) {
        const Subtitle &subtitle = subtitles.at(index);
        if (subtitle.isTimed() && subtitle.start.milliseconds() <= positionMs
            && positionMs < subtitle.end.milliseconds()) {
            return index;
        }
    }
    return -1;
}

} // namespace

class SubtitleTimeIndexTests final : public QObject {
    Q_OBJECT

private slots:
    void overlapPrefersDocumentOrder();
    void gapAndBoundaryAreHalfOpen();
    void unorderedTimesStillFollowDocumentOrder();
    void sequentialLookupMatchesRelocate();
    void reverseAndSeekRelocate();
    void documentChangeInvalidatesCache();
    void tenThousandCuesStayFasterThanLinearScan();
};

void SubtitleTimeIndexTests::overlapPrefersDocumentOrder()
{
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("a"), 0, 2'000, QStringLiteral("A")),
        makeCue(QStringLiteral("b"), 500, 1'500, QStringLiteral("B"))};
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QCOMPARE(index.documentIndexAt(0), 0);
    QCOMPARE(index.documentIndexAt(500), 0);
    QCOMPARE(index.documentIndexAt(1'499), 0);
    QCOMPARE(index.documentIndexAt(1'500), 0);
}

void SubtitleTimeIndexTests::gapAndBoundaryAreHalfOpen()
{
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("a"), 0, 1'000, QStringLiteral("A")),
        makeCue(QStringLiteral("b"), 1'200, 2'000, QStringLiteral("B"))};
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QCOMPARE(index.documentIndexAt(-1), -1);
    QCOMPARE(index.documentIndexAt(0), 0);
    QCOMPARE(index.documentIndexAt(999), 0);
    QCOMPARE(index.documentIndexAt(1'000), -1);
    QCOMPARE(index.documentIndexAt(1'100), -1);
    QCOMPARE(index.documentIndexAt(1'200), 1);
    QCOMPARE(index.documentIndexAt(2'000), -1);
}

void SubtitleTimeIndexTests::unorderedTimesStillFollowDocumentOrder()
{
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("late"), 1'000, 2'000, QStringLiteral("Late")),
        makeCue(QStringLiteral("early"), 0, 3'000, QStringLiteral("Early"))};
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QCOMPARE(index.documentIndexAt(100), 1);
    QCOMPARE(index.documentIndexAt(1'500), 0);
    QCOMPARE(index.documentIndexAt(2'500), 1);
}

void SubtitleTimeIndexTests::sequentialLookupMatchesRelocate()
{
    QList<Subtitle> cues;
    for (int index = 0; index < 50; ++index) {
        cues.append(makeCue(QString::number(index), index * 100, index * 100 + 80,
            QString::number(index)));
    }
    SubtitleTimeIndex index;
    index.rebuild(cues);
    for (qint64 position = 0; position < 5'000; position += 7)
        QCOMPARE(index.lookup(position, 1), naiveIndex(cues, position));
}

void SubtitleTimeIndexTests::reverseAndSeekRelocate()
{
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("a"), 0, 1'000, QStringLiteral("A")),
        makeCue(QStringLiteral("b"), 2'000, 3'000, QStringLiteral("B")),
        makeCue(QStringLiteral("c"), 4'000, 5'000, QStringLiteral("C"))};
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QCOMPARE(index.lookup(4'500, 1), 2);
    QCOMPARE(index.lookup(2'500, -1), 1);
    QCOMPARE(index.lookup(100, 1), 0);
    QCOMPARE(index.lookup(3'500, 1), -1);
}

void SubtitleTimeIndexTests::documentChangeInvalidatesCache()
{
    QList<Subtitle> cues{makeCue(QStringLiteral("a"), 0, 1'000, QStringLiteral("A"))};
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QCOMPARE(index.lookup(100, 1), 0);
    cues[0].end = MediaTime::fromMilliseconds(50);
    index.rebuild(cues);
    QCOMPARE(index.lookup(100, 1), -1);
    QCOMPARE(index.lookup(10, 1), 0);
}

void SubtitleTimeIndexTests::tenThousandCuesStayFasterThanLinearScan()
{
    QList<Subtitle> cues;
    cues.reserve(10'000);
    for (int index = 0; index < 10'000; ++index) {
        cues.append(makeCue(QString::number(index), index * 40, index * 40 + 30,
            QString::number(index)));
    }
    SubtitleTimeIndex index;
    index.rebuild(cues);
    QElapsedTimer timer;
    timer.start();
    int hits = 0;
    for (qint64 position = 0; position < 400'000; position += 20)
        hits += index.lookup(position, 1) >= 0 ? 1 : 0;
    const qint64 indexedMs = timer.restart();
    int naiveHits = 0;
    for (qint64 position = 0; position < 400'000; position += 20)
        naiveHits += naiveIndex(cues, position) >= 0 ? 1 : 0;
    const qint64 naiveMs = timer.elapsed();
    QCOMPARE(hits, naiveHits);
    qInfo() << "subtitle_lookup indexed_ms=" << indexedMs << "naive_ms=" << naiveMs
            << "lookups=" << 20'000;
    QVERIFY(indexedMs <= naiveMs + 20);
}

QTEST_APPLESS_MAIN(SubtitleTimeIndexTests)
#include "test_subtitle_time_index.moc"
