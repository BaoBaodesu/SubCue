#pragma once

#include "cache/cache_key.h"
#include "cache/lru_cache.h"
#include "media/media_types.h"
#include "media/thumbnail_loader.h"
#include "waveform/waveform_generator.h"

#include <QtCore/QSize>
#include <QtCore/QVector>
#include <QtGui/QImage>

#include <atomic>
#include <memory>

namespace subcue {

class PreviewCache final {
public:
    explicit PreviewCache(qsizetype maximumBytes = 64 * 1024 * 1024);

    PreviewCache(const PreviewCache &) = delete;
    PreviewCache &operator=(const PreviewCache &) = delete;

    [[nodiscard]] quint64 generation() const noexcept;
    quint64 bumpGeneration();

    [[nodiscard]] MediaResult<CacheKey> keyFor(
        const QString &path,
        int streamIndex,
        const QString &qualifier) const;

    [[nodiscard]] WaveformGenerator::Result waveform(
        const QString &path,
        int streamIndex = -1,
        int sampleRate = WaveformGenerator::kDefaultSampleRate,
        quint64 expectedGeneration = 0);
    [[nodiscard]] ThumbnailLoader::Result thumbnail(
        const QString &path,
        MediaTime pts,
        QSize size,
        int streamIndex = -1,
        quint64 expectedGeneration = 0);
    [[nodiscard]] QVector<QImage> thumbnailsInRange(
        const QString &path,
        MediaTime start,
        MediaTime end,
        MediaTime interval,
        QSize size,
        int streamIndex = -1,
        quint64 expectedGeneration = 0);

    [[nodiscard]] std::shared_ptr<const WaveformPyramid> peekWaveform(const CacheKey &key);
    [[nodiscard]] QImage peekThumbnail(const CacheKey &key);

    [[nodiscard]] LruCache &store() noexcept { return cache_; }
    [[nodiscard]] const LruCache &store() const noexcept { return cache_; }

private:
    [[nodiscard]] quint64 resolveGeneration(quint64 expectedGeneration) const noexcept;
    [[nodiscard]] bool accept(quint64 expectedGeneration) const noexcept;
    [[nodiscard]] AppError staleError() const;
    [[nodiscard]] int resolveAudioStream(const QString &path, int streamIndex, AppError *error) const;
    [[nodiscard]] int resolveVideoStream(const QString &path, int streamIndex, AppError *error) const;

    LruCache cache_;
    WaveformGenerator waveformGenerator_;
    ThumbnailLoader thumbnailLoader_;
    std::atomic<quint64> generation_{1};
};

} // namespace subcue
