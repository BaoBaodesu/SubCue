#include "subtitle/txt_importer.h"

#include <QtCore/QRegularExpression>

namespace subcue {

QStringList TxtImporter::parse(const QString &content)
{
    QString normalized = content;
    normalized.remove(QChar::ByteOrderMark);
    QStringList result;
    for (QString line : normalized.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
        line = line.trimmed();
        if (!line.isEmpty()) result.append(line);
    }
    return result;
}

} // namespace subcue
