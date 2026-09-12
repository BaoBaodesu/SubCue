#include "subtitle/srt_writer.h"

#include "subtitle/timecode.h"

#include <QtCore/QSaveFile>

namespace subcue {

QString SrtWriter::build(const QList<Subtitle> &subtitles)
{
    QStringList blocks;
    int outputIndex = 1;
    for (const Subtitle &subtitle : subtitles) {
        if (!subtitle.isExportable()) continue;
        blocks.append(QStringLiteral("%1\n%2 --> %3\n%4")
            .arg(outputIndex++)
            .arg(timecode::formatSrt(subtitle.start), timecode::formatSrt(subtitle.end),
                 subtitle.text));
    }
    return blocks.isEmpty() ? QString() : blocks.join(QStringLiteral("\n\n")) + QChar(u'\n');
}

bool SrtWriter::save(const QString &path, const QList<Subtitle> &subtitles, QString *errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    if (file.write(build(subtitles).toUtf8()) < 0 || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

} // namespace subcue
