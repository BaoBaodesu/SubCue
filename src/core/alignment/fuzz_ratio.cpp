#include "alignment/fuzz_ratio.h"

#include "alignment/normalizer.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace subcue {
namespace {

[[nodiscard]] std::size_t indelDistance(
    const char32_t *left, std::size_t rows, const char32_t *right, std::size_t columns) noexcept
{
    std::vector<std::size_t> previous(columns + 1);
    std::vector<std::size_t> current(columns + 1);
    for (std::size_t column = 0; column <= columns; ++column) {
        previous[column] = column;
    }
    for (std::size_t row = 1; row <= rows; ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= columns; ++column) {
            const std::size_t substitutionCost = left[row - 1] == right[column - 1] ? 0 : 2;
            current[column] = std::min({
                previous[column] + 1,
                current[column - 1] + 1,
                previous[column - 1] + substitutionCost,
            });
        }
        std::swap(previous, current);
    }
    return previous[columns];
}

} // namespace

double FuzzRatio::ratio(
    const char32_t *left, std::size_t leftSize,
    const char32_t *right, std::size_t rightSize) noexcept
{
    const std::size_t maximum = leftSize + rightSize;
    if (maximum == 0) {
        return 100.0;
    }
    const std::size_t distance = indelDistance(left, leftSize, right, rightSize);
    return 100.0 * (1.0 - static_cast<double>(distance) / static_cast<double>(maximum));
}

double FuzzRatio::ratio(const std::u32string &left, const std::u32string &right) noexcept
{
    return ratio(left.data(), left.size(), right.data(), right.size());
}

double FuzzRatio::ratio(QStringView left, QStringView right)
{
    return ratio(Normalizer::toCodepoints(left), Normalizer::toCodepoints(right));
}

} // namespace subcue
