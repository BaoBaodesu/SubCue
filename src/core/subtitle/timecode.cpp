#include "subtitle/timecode.h"

#include <QtCore/QRegularExpression>

namespace subcue::timecode {
namespace {

qint64 nonNegativeMilliseconds(MediaTime time)
{
    return qMax<qint64>(0, time.microseconds() / 1000);
}

qint64 roundCentisecondsTiesToEven(qint64 milliseconds)
{
    qint64 centiseconds = milliseconds / 10;
    const qint64 remainder = milliseconds % 10;
    if (remainder > 5 || (remainder == 5 && centiseconds % 2 != 0)) {
        ++centiseconds;
    }
    return centiseconds;
}

} // namespace

QString formatSrt(MediaTime time)
{
    qint64 milliseconds = nonNegativeMilliseconds(time);
    const qint64 hours = milliseconds / 3'600'000;
    milliseconds %= 3'600'000;
    const qint64 minutes = milliseconds / 60'000;
    milliseconds %= 60'000;
    const qint64 seconds = milliseconds / 1000;
    const qint64 millis = milliseconds % 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(hours, 2, 10, QChar(u'0'))
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'))
        .arg(millis, 3, 10, QChar(u'0'));
}

QString formatAss(MediaTime time)
{
    qint64 milliseconds = nonNegativeMilliseconds(time);
    qint64 hours = milliseconds / 3'600'000;
    milliseconds %= 3'600'000;
    qint64 minutes = milliseconds / 60'000;
    milliseconds %= 60'000;
    qint64 seconds = milliseconds / 1000;
    qint64 centiseconds = roundCentisecondsTiesToEven(milliseconds % 1000);
    if (centiseconds == 100) {
        ++seconds;
        centiseconds = 0;
    }
    if (seconds == 60) {
        ++minutes;
        seconds = 0;
    }
    if (minutes == 60) {
        ++hours;
        minutes = 0;
    }
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours)
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'))
        .arg(centiseconds, 2, 10, QChar(u'0'));
}

std::optional<MediaTime> parseSrt(const QString &value)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^(\d{1,3}):([0-5]\d):([0-5]\d)[,.](\d{3})$)"));
    const QRegularExpressionMatch match = expression.match(value.trimmed());
    if (!match.hasMatch()) return std::nullopt;
    const qint64 milliseconds = match.captured(1).toLongLong() * 3'600'000
        + match.captured(2).toLongLong() * 60'000
        + match.captured(3).toLongLong() * 1000
        + match.captured(4).toLongLong();
    return MediaTime::fromMilliseconds(milliseconds);
}

} // namespace subcue::timecode
