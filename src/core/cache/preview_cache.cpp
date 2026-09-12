#include "cache/preview_cache.h"

#include "common/logging.h"
#include "media/ffmpeg_error.h"
#include "media/media_probe.h"

#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace subcue {

PreviewCache::PreviewCache(qsizetype maximumBytes)
    : cache_(maximumBytes)
{
}

quint64 PreviewCache::generation() const noexcept
{
    return generation_.load(std::memory_order_acquire);
}

quint64 PreviewCache::bumpGeneration()
{
    return generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

MediaResult<CacheKey> PreviewCache::keyFor(
    const QString &path,
    int streamIndex,
    const QString &qualifier) const
{
    AppError error(ErrorDomain::Media, 0, QString());
    CacheKey key = makeCacheKey(path, streamIndex, qualifier, &error);
    if (!key.isValid()) {
        return error;
    }
    return key;
}

WaveformGenerator::Result PreviewCache::waveform(
    const QString &path,
    int streamIndex,
    int sampleRate,
    quint64 expectedGeneration)
{
    expectedGeneration = resolveGeneration(expectedGeneration);
    if (!accept(expectedGeneration)) {
        return staleError();
    }

    AppError error(ErrorDomain::Media, 0, QString());
    const int audioStream = resolveAudioStream(path, streamIndex, &error);
    if (audioStream < 0) {
        return error;
    }
    CacheKey key = makeCacheKey(path, audioStream, waveformQualifier(sampleRate), &error);
    if (!key.isValid()) {
        return error;
    }
    if (std::shared_ptr<const WaveformPyramid> hit = cache_.waveform(key)) {
        return hit;
    }
    if (!accept(expectedGeneration)) {
        return staleError();
    }

    WaveformGenerator::Result generated = waveformGenerator_.generate(
        path,
        audioStream,
        sampleRate,
        nullptr,
        &generation_,
        expectedGeneration);
    if (std::holds_alternative<AppError>(generated)) {
        return generated;
    }
    if (!accept(expectedGeneration)) {
        return staleError();
    }
    std::shared_ptr<const WaveformPyramid> pyramid = std::get<std::shared_ptr<const WaveformPyramid>>(generated);
    if (!cache_.insertWaveform(key, pyramid)) {
        qCDebug(subcueCacheLog) << "Waveform not cached" << key.canonicalPath;
    }
    return pyramid;
}

ThumbnailLoader::Result PreviewCache::thumbnail(
    const QString &path,
    MediaTime pts,
    QSize size,
    int streamIndex,
    quint64 expectedGeneration)
{
    expectedGeneration = resolveGeneration(expectedGeneration);
    if (!accept(expectedGeneration)) {
        return staleError();
    }

    AppError error(ErrorDomain::Media, 0, QString());
    const int videoStream = resolveVideoStream(path, streamIndex, &error);
    if (videoStream < 0) {
        return error;
    }
    CacheKey key = makeCacheKey(path, videoStream, thumbnailQualifier(pts, size), &error);
    if (!key.isValid()) {
        return error;
    }
    QImage hit = cache_.thumbnail(key);
    if (!hit.isNull()) {
        return hit;
    }
    if (!accept(expectedGeneration)) {
        return staleError();
    }

    ThumbnailLoader::Result generated = thumbnailLoader_.decode(
        path,
        pts,
        size,
        videoStream,
        nullptr,
        &generation_,
        expectedGeneration);
    if (std::holds_alternative<AppError>(generated)) {
        return generated;
    }
    if (!accept(expectedGeneration)) {
        return staleError();
    }
    QImage image = std::get<QImage>(generated);
    if (!cache_.insertThumbnail(key, image)) {
        qCDebug(subcueCacheLog) << "Thumbnail not cached" << key.canonicalPath << pts.microseconds();
    }
    return image;
}

QVector<QImage> PreviewCache::thumbnailsInRange(
    const QString &path,
    MediaTime start,
    MediaTime end,
    MediaTime interval,
    QSize size,
    int streamIndex,
    quint64 expectedGeneration)
{
    QVector<QImage> images;
    if (interval.microseconds() <= 0 || end.microseconds() <= start.microseconds()) {
        return images;
    }
    expectedGeneration = resolveGeneration(expectedGeneration);
    for (qint64 time = start.microseconds(); time < end.microseconds(); time += interval.microseconds()) {
        if (!accept(expectedGeneration)) {
            break;
        }
        ThumbnailLoader::Result result = thumbnail(
            path,
            MediaTime::fromMicroseconds(time),
            size,
            streamIndex,
            expectedGeneration);
        if (std::holds_alternative<QImage>(result)) {
            images.push_back(std::get<QImage>(result));
        } else {
            images.push_back(QImage());
        }
    }
    return images;
}

std::shared_ptr<const WaveformPyramid> PreviewCache::peekWaveform(const CacheKey &key)
{
    return cache_.waveform(key);
}

QImage PreviewCache::peekThumbnail(const CacheKey &key)
{
    return cache_.thumbnail(key);
}

quint64 PreviewCache::resolveGeneration(quint64 expectedGeneration) const noexcept
{
    return expectedGeneration == 0 ? generation_.load(std::memory_order_acquire) : expectedGeneration;
}

bool PreviewCache::accept(quint64 expectedGeneration) const noexcept
{
    return expectedGeneration == generation_.load(std::memory_order_acquire);
}

AppError PreviewCache::staleError() const
{
    return AppError(ErrorDomain::Media, AVERROR(ECANCELED), QStringLiteral("预览任务已被更新的请求替换"));
}

int PreviewCache::resolveAudioStream(const QString &path, int streamIndex, AppError *error) const
{
    if (streamIndex >= 0) {
        return streamIndex;
    }
    ProbeResult probe = MediaProbe::probe(path);
    if (std::holds_alternative<AppError>(probe)) {
        if (error) {
            *error = std::get<AppError>(std::move(probe));
        }
        return -1;
    }
    const int audio = std::get<MediaInfo>(probe).audioStreamIndex;
    if (audio < 0) {
        if (error) {
            *error = makeFfmpegError(
                ErrorDomain::Decoder,
                audio,
                QStringLiteral("媒体中没有可解码的音频流"),
                path);
        }
        return -1;
    }
    return audio;
}

int PreviewCache::resolveVideoStream(const QString &path, int streamIndex, AppError *error) const
{
    if (streamIndex >= 0) {
        return streamIndex;
    }
    ProbeResult probe = MediaProbe::probe(path);
    if (std::holds_alternative<AppError>(probe)) {
        if (error) {
            *error = std::get<AppError>(std::move(probe));
        }
        return -1;
    }
    const int video = std::get<MediaInfo>(probe).videoStreamIndex;
    if (video < 0) {
        if (error) {
            *error = makeFfmpegError(
                ErrorDomain::Decoder,
                video,
                QStringLiteral("媒体中没有可解码的视频流"),
                path);
        }
        return -1;
    }
    return video;
}

} // namespace subcue
