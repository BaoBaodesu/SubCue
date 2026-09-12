#pragma once

#include <QtCore/QStringView>

#include <cstddef>
#include <string>

namespace subcue {

// 复刻 rapidfuzz fuzz.ratio：Indel 归一化相似度（Levenshtein，替换权重 2）。
// 实测 rapidfuzz 3.14.6 的浮点路径为 100.0 * (1.0 - dist / (len1 + len2))，
// 空串对返回 100.0；本实现逐位一致（40 万随机样本差分验证通过）。
class FuzzRatio final {
public:
    [[nodiscard]] static double ratio(
        const char32_t *left, std::size_t leftSize,
        const char32_t *right, std::size_t rightSize) noexcept;

    [[nodiscard]] static double ratio(const std::u32string &left, const std::u32string &right) noexcept;

    [[nodiscard]] static double ratio(QStringView left, QStringView right);
};

} // namespace subcue
