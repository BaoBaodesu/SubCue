#include "cache/cache_key.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace subcue {
namespace {

QByteArray hashFileRange(QFile &file, qint64 offset, qint64 length, AppError *error)
{
    if (!file.seek(offset)) {
        if (error) {
            *error = AppError(
                ErrorDomain::Media,
                AVERROR(EIO),
                QStringLiteral("无法读取媒体缓存分块"),
                file.fileName());
        }
        return {};
    }
    const QByteArray data = file.read(length);
    if (data.size() != length) {
        if (error) {
            *error = AppError(
                ErrorDomain::Media,
                AVERROR(EIO),
                QStringLiteral("媒体缓存分块不完整"),
                file.fileName());
        }
        return {};
    }
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

} // namespace

bool CacheKey::isValid() const noexcept
{
    return !canonicalPath.isEmpty()
        && size >= 0
        && mtimeMs >= 0
        && headHash.size() == 32
        && tailHash.size() == 32;
}

QString CacheKey::id() const
{
    return canonicalPath
        + QLatin1Char('|')
        + QString::number(size)
        + QLatin1Char('|')
        + QString::number(mtimeMs)
        + QLatin1Char('|')
        + QString::number(streamIndex)
        + QLatin1Char('|')
        + QString::fromLatin1(headHash.toHex())
        + QLatin1Char('|')
        + QString::fromLatin1(tailHash.toHex())
        + QLatin1Char('|')
        + qualifier;
}

QString waveformQualifier(int sampleRate)
{
    return QStringLiteral("waveform:%1").arg(sampleRate);
}

QString thumbnailQualifier(MediaTime pts, QSize size)
{
    return QStringLiteral("thumb:%1:%2x%3")
        .arg(pts.microseconds())
        .arg(size.width())
        .arg(size.height());
}

CacheKey makeCacheKey(const QString &path, int streamIndex, const QString &qualifier, AppError *error)
{
    const QFileInfo info(path);
    const QString canonical = QFileInfo(info.canonicalFilePath()).absoluteFilePath();
    if (canonical.isEmpty() || !info.exists()) {
        if (error) {
            *error = AppError(
                ErrorDomain::Media,
                AVERROR(ENOENT),
                QStringLiteral("无法解析媒体缓存路径"),
                path);
        }
        return {};
    }

    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = AppError(
                ErrorDomain::Media,
                AVERROR(EIO),
                QStringLiteral("无法打开媒体文件以计算缓存键"),
                canonical);
        }
        return {};
    }

    CacheKey key;
    key.canonicalPath = QDir::fromNativeSeparators(canonical);
    key.size = file.size();
    key.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    key.streamIndex = streamIndex;
    key.qualifier = qualifier;

    const qint64 chunk = key.size < kCacheKeyChunkBytes ? key.size : kCacheKeyChunkBytes;
    AppError hashError(ErrorDomain::Media, 0, QString());
    key.headHash = hashFileRange(file, 0, chunk, &hashError);
    if (key.headHash.isEmpty()) {
        if (error) {
            *error = std::move(hashError);
        }
        return {};
    }

    const qint64 tailOffset = key.size > kCacheKeyChunkBytes ? key.size - kCacheKeyChunkBytes : 0;
    const qint64 tailLength = key.size - tailOffset;
    key.tailHash = hashFileRange(file, tailOffset, tailLength, &hashError);
    if (key.tailHash.isEmpty()) {
        if (error) {
            *error = std::move(hashError);
        }
        return {};
    }
    return key;
}

} // namespace subcue
