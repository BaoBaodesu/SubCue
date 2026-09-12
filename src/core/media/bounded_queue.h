#pragma once

#include <QtCore/QtGlobal>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace subcue {

template<typename T>
class BoundedQueue final {
public:
    struct Item final {
        T value;
        qsizetype bytes = 0;
        quint64 generation = 0;
    };

    explicit BoundedQueue(qsizetype maximumItems, qsizetype maximumBytes)
        : maximumItems_(maximumItems), maximumBytes_(maximumBytes)
    {
    }

    BoundedQueue(const BoundedQueue &) = delete;
    BoundedQueue &operator=(const BoundedQueue &) = delete;

    [[nodiscard]] bool push(T value, qsizetype bytes, quint64 generation)
    {
        if (bytes < 0 || bytes > maximumBytes_) {
            return false;
        }
        std::unique_lock lock(mutex_);
        notFull_.wait(lock, [this, bytes, generation] {
            return aborted_ || generation != generation_
                || (items_.size() < static_cast<std::size_t>(maximumItems_)
                    && totalBytes_ + bytes <= maximumBytes_);
        });
        if (aborted_ || generation != generation_) {
            return false;
        }
        totalBytes_ += bytes;
        items_.push_back(Item{std::move(value), bytes, generation});
        notEmpty_.notify_one();
        return true;
    }

    [[nodiscard]] bool pop(Item &item)
    {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [this] { return aborted_ || !items_.empty(); });
        if (aborted_) {
            return false;
        }
        item = std::move(items_.front());
        totalBytes_ -= item.bytes;
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }

    [[nodiscard]] bool tryPop(Item &item)
    {
        std::lock_guard lock(mutex_);
        if (aborted_ || items_.empty()) {
            return false;
        }
        item = std::move(items_.front());
        totalBytes_ -= item.bytes;
        items_.pop_front();
        notFull_.notify_one();
        return true;
    }

    void setGeneration(quint64 generation)
    {
        std::lock_guard lock(mutex_);
        generation_ = generation;
        items_.clear();
        totalBytes_ = 0;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        items_.clear();
        totalBytes_ = 0;
        notFull_.notify_all();
    }

    void abort()
    {
        std::lock_guard lock(mutex_);
        aborted_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void resetAbort()
    {
        std::lock_guard lock(mutex_);
        aborted_ = false;
    }

    void wake()
    {
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    [[nodiscard]] qsizetype size() const
    {
        std::lock_guard lock(mutex_);
        return static_cast<qsizetype>(items_.size());
    }

    [[nodiscard]] qsizetype totalBytes() const
    {
        std::lock_guard lock(mutex_);
        return totalBytes_;
    }

    [[nodiscard]] quint64 generation() const
    {
        std::lock_guard lock(mutex_);
        return generation_;
    }

private:
    qsizetype maximumItems_;
    qsizetype maximumBytes_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Item> items_;
    qsizetype totalBytes_ = 0;
    quint64 generation_ = 0;
    bool aborted_ = false;
};

} // namespace subcue
