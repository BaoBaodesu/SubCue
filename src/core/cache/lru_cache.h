#pragma once

#include "cache/cache_key.h"
#include "waveform/waveform_pyramid.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <list>
#include <memory>
#include <mutex>

namespace subcue {

class LruCache final {
public:
    struct Stats final {
        int entries = 0;
        qsizetype bytes = 0;
        int hits = 0;
        int misses = 0;
        int evictions = 0;
    };

    explicit LruCache(qsizetype maximumBytes);

    LruCache(const LruCache &) = delete;
    LruCache &operator=(const LruCache &) = delete;

    [[nodiscard]] bool insertWaveform(const CacheKey &key, std::shared_ptr<const WaveformPyramid> pyramid);
    [[nodiscard]] bool insertThumbnail(const CacheKey &key, const QImage &image);

    [[nodiscard]] std::shared_ptr<const WaveformPyramid> waveform(const CacheKey &key);
    [[nodiscard]] QImage thumbnail(const CacheKey &key);

    [[nodiscard]] bool contains(const CacheKey &key) const;
    bool remove(const CacheKey &key);
    void clear();

    [[nodiscard]] qsizetype maximumBytes() const noexcept;
    [[nodiscard]] qsizetype totalBytes() const;
    [[nodiscard]] int size() const;
    [[nodiscard]] Stats stats() const;

private:
    enum class Kind {
        Waveform,
        Thumbnail
    };

    struct Entry final {
        CacheKey key;
        Kind kind = Kind::Waveform;
        qsizetype bytes = 0;
        std::shared_ptr<const WaveformPyramid> waveform;
        QImage thumbnail;
    };

    [[nodiscard]] bool insert(Entry entry);
    void evictUntilFits(qsizetype incomingBytes);
    void erase(std::list<Entry>::iterator iterator);

    const qsizetype maximumBytes_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    QHash<QString, std::list<Entry>::iterator> index_;
    qsizetype totalBytes_ = 0;
    int hits_ = 0;
    int misses_ = 0;
    int evictions_ = 0;
};

} // namespace subcue
