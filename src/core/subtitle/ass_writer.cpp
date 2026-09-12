#include "subtitle/ass_writer.h"

#include "subtitle/timecode.h"

#include <QtCore/QHash>
#include <QtCore/QSaveFile>

namespace subcue {
namespace {

QString assColor(QString value, const QString &alpha = QStringLiteral("00"))
{
    while (value.startsWith(QChar(u'#'))) value.remove(0, 1);
    if (value.size() != 6) value = QStringLiteral("FFFFFF");
    return QStringLiteral("&H%1%2%3%4")
        .arg(alpha, value.mid(4, 2), value.mid(2, 2), value.mid(0, 2));
}

int scaledTiesToEven(int value, int height)
{
    const qint64 numerator = static_cast<qint64>(value) * height;
    qint64 quotient = numerator / 1080;
    const qint64 remainder = numerator % 1080;
    if (remainder * 2 > 1080 || (remainder * 2 == 1080 && quotient % 2 != 0)) {
        ++quotient;
    }
    return static_cast<int>(quotient);
}

} // namespace

QString AssWriter::build(const QList<Subtitle> &subtitles, int width, int height,
                         const QJsonObject &settings)
{
    static const QHash<QString, int> alignments{
        {QStringLiteral("bottom-left"), 1},
        {QStringLiteral("bottom-center"), 2},
        {QStringLiteral("bottom-right"), 3},
    };
    const int fontSize = scaledTiesToEven(
        settings.value(QStringLiteral("fontSize1080p")).toInt(52), height);
    const int margin = scaledTiesToEven(
        settings.value(QStringLiteral("bottomMargin1080p")).toInt(90), height);
    const int alignment = alignments.value(
        settings.value(QStringLiteral("alignment")).toString(), 2);
    const QString font = settings.value(QStringLiteral("fontFamily"))
        .toString(QStringLiteral("Microsoft YaHei"));
    const QString primary = assColor(settings.value(QStringLiteral("fontColor"))
        .toString(QStringLiteral("#FFFFFF")));
    const QString outline = assColor(settings.value(QStringLiteral("outlineColor"))
        .toString(QStringLiteral("#000000")));
    const int outlineSize = settings.value(QStringLiteral("outlineSize")).toInt(2);
    const int shadow = settings.value(QStringLiteral("shadow")).toInt(0);

    QString output = QStringLiteral(
        "[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\nScaledBorderAndShadow: yes\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,%3,%4,%5,&H000000FF,%6,&H00000000,0,0,0,0,100,100,0,0,1,%7,%8,%9,20,20,%10,1\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n")
        .arg(width).arg(height).arg(font).arg(fontSize).arg(primary).arg(outline)
        .arg(outlineSize).arg(shadow).arg(alignment).arg(margin);
    for (const Subtitle &subtitle : subtitles) {
        if (!subtitle.isExportable()) continue;
        QString text = subtitle.text;
        text.replace(QChar(u'\r'), QString());
        text.replace(QChar(u'\n'), QStringLiteral("\\N"));
        output += QStringLiteral("Dialogue: 0,%1,%2,Default,,0,0,0,,%3\n")
            .arg(timecode::formatAss(subtitle.start), timecode::formatAss(subtitle.end), text);
    }
    return output;
}

bool AssWriter::save(const QString &path, const QList<Subtitle> &subtitles, int width, int height,
                     const QJsonObject &settings, QString *errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    const QByteArray data = QByteArray::fromHex("EFBBBF")
        + build(subtitles, width, height, settings).toUtf8();
    if (file.write(data) < 0 || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

} // namespace subcue
