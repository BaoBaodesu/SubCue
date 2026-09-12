#include "cache/lru_cache.h"

#include <iterator>
#include <utility>

namespace subcue {

LruCache::LruCache(qsizetype maximumBytes)
    : maximumBytes_(maximumBytes < 1 ? 1 : maximumBytes)
{
}

bool LruCache::insertWaveform(const CacheKey &key, std::shared_ptr<const WaveformPyramid> pyramid)
{
    if (!key.isValid() || !pyramid) {
        return false;
    }
    Entry entry;
    entry.key = key;
    entry.kind = Kind::Waveform;
    entry.bytes = pyramid->byteSize();
    entry.waveform = std::move(pyramid);
    if (entry.bytes < 1) {
        entry.bytes = 1;
    }
    return insert(std::move(entry));
}

bool LruCache::insertThumbnail(const CacheKey &key, const QImage &image)
{
    if (!key.isValid() || image.isNull()) {
        return false;
    }
    Entry entry;
    entry.key = key;
    entry.kind = Kind::Thumbnail;
    entry.bytes = image.sizeInBytes();
    entry.thumbnail = image;
    if (entry.bytes < 1) {
        entry.bytes = 1;
    }
    return insert(std::move(entry));
}

std::shared_ptr<const WaveformPyramid> LruCache::waveform(const CacheKey &key)
{
    std::lock_guard lock(mutex_);
    const auto found = index_.constFind(key.id());
    if (found == index_.cend() || found.value()->kind != Kind::Waveform) {
        ++misses_;
        return {};
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found.value());
    return found.value()->waveform;
}

QImage LruCache::thumbnail(const CacheKey &key)
{
    std::lock_guard lock(mutex_);
    const auto found = index_.constFind(key.id());
    if (found == index_.cend() || found.value()->kind != Kind::Thumbnail) {
        ++misses_;
        return {};
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found.value());
    return found.value()->thumbnail;
}

bool LruCache::contains(const CacheKey &key) const
{
    std::lock_guard lock(mutex_);
    return index_.contains(key.id());
}

bool LruCache::remove(const CacheKey &key)
{
    std::lock_guard lock(mutex_);
    const auto found = index_.constFind(key.id());
    if (found == index_.cend()) {
        return false;
    }
    erase(found.value());
    return true;
}

void LruCache::clear()
{
    std::lock_guard lock(mutex_);
    entries_.clear();
    index_.clear();
    totalBytes_ = 0;
}

qsizetype LruCache::maximumBytes() const noexcept
{
    return maximumBytes_;
}

qsizetype LruCache::totalBytes() const
{
    std::lock_guard lock(mutex_);
    return totalBytes_;
}

int LruCache::size() const
{
    std::lock_guard lock(mutex_);
    return static_cast<int>(entries_.size());
}

LruCache::Stats LruCache::stats() const
{
    std::lock_guard lock(mutex_);
    Stats stats;
    stats.entries = static_cast<int>(entries_.size());
    stats.bytes = totalBytes_;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.evictions = evictions_;
    return stats;
}

bool LruCache::insert(Entry entry)
{
    if (entry.bytes > maximumBytes_) {
        return false;
    }
    std::lock_guard lock(mutex_);
    const QString id = entry.key.id();
    const auto existing = index_.constFind(id);
    if (existing != index_.cend()) {
        erase(existing.value());
    }
    evictUntilFits(entry.bytes);
    if (totalBytes_ + entry.bytes > maximumBytes_) {
        return false;
    }
    totalBytes_ += entry.bytes;
    entries_.push_front(std::move(entry));
    index_.insert(id, entries_.begin());
    return true;
}

void LruCache::evictUntilFits(qsizetype incomingBytes)
{
    while (!entries_.empty() && totalBytes_ + incomingBytes > maximumBytes_) {
        erase(std::prev(entries_.end()));
        ++evictions_;
    }
}

void LruCache::erase(std::list<Entry>::iterator iterator)
{
    totalBytes_ -= iterator->bytes;
    index_.remove(iterator->key.id());
    entries_.erase(iterator);
}

} // namespace subcue
