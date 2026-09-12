#include "common/app_error.h"
#include "common/media_time.h"

#include <QtTest/QTest>

#include <limits>

class MediaTimeTests final : public QObject {
    Q_OBJECT

private slots:
    void convertsWithoutAccumulatedFrameError()
    {
        const auto value = subcue::MediaTime::fromMicroseconds(3'424'567);
        QCOMPARE(value.microseconds(), 3'424'567);
        QCOMPARE(value.milliseconds(), 3'424);
        QCOMPARE(value.seconds(), 3.424567);
    }

    void convertsMillisecondsExactly()
    {
        QCOMPARE(subcue::MediaTime::fromMilliseconds(2'080).microseconds(), 2'080'000);
    }

    void saturatesOverflow()
    {
        QCOMPARE(
            subcue::MediaTime::fromMilliseconds(std::numeric_limits<qint64>::max()).microseconds(),
            std::numeric_limits<qint64>::max());
    }

    void exposesStructuredErrors()
    {
        const subcue::AppError error(
            subcue::ErrorDomain::Media,
            7,
            QStringLiteral("无法打开媒体"),
            QStringLiteral("decoder unavailable"));
        QCOMPARE(error.code(), 7);
        QCOMPARE(error.userMessage(), QStringLiteral("无法打开媒体"));
        QCOMPARE(error.technicalDetails(), QStringLiteral("decoder unavailable"));
    }
};

QTEST_GUILESS_MAIN(MediaTimeTests)

#include "test_media_time.moc"
