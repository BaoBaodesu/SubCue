#pragma once

#include <QtCore/QtGlobal>

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>

namespace subcue {

class MediaTime final {
public:
    constexpr MediaTime() noexcept = default;

    [[nodiscard]] static constexpr MediaTime fromMicroseconds(qint64 value) noexcept
    {
        return MediaTime(value);
    }

    [[nodiscard]] static constexpr MediaTime fromMilliseconds(qint64 value) noexcept
    {
        if (value > std::numeric_limits<qint64>::max() / 1000) {
            return MediaTime(std::numeric_limits<qint64>::max());
        }
        if (value < std::numeric_limits<qint64>::min() / 1000) {
            return MediaTime(std::numeric_limits<qint64>::min());
        }
        return MediaTime(value * 1000);
    }

    [[nodiscard]] static MediaTime fromSeconds(double value) noexcept;

    [[nodiscard]] constexpr qint64 microseconds() const noexcept { return microseconds_; }
    [[nodiscard]] constexpr qint64 milliseconds() const noexcept { return microseconds_ / 1000; }
    [[nodiscard]] constexpr double seconds() const noexcept
    {
        return static_cast<double>(microseconds_) / 1'000'000.0;
    }

    [[nodiscard]] constexpr bool isValidRange() const noexcept { return microseconds_ >= 0; }

    constexpr auto operator<=>(const MediaTime &) const noexcept = default;

private:
    explicit constexpr MediaTime(qint64 value) noexcept : microseconds_(value) {}

    qint64 microseconds_ = 0;
};

inline MediaTime MediaTime::fromSeconds(double value) noexcept
{
    constexpr double maximum = static_cast<double>(std::numeric_limits<qint64>::max()) / 1'000'000.0;
    constexpr double minimum = static_cast<double>(std::numeric_limits<qint64>::min()) / 1'000'000.0;
    if (value >= maximum) {
        return fromMicroseconds(std::numeric_limits<qint64>::max());
    }
    if (value <= minimum) {
        return fromMicroseconds(std::numeric_limits<qint64>::min());
    }
    return fromMicroseconds(static_cast<qint64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(value)).count()));
}

} // namespace subcue
