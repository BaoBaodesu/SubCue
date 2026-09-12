#pragma once

#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <cstddef>
#include <string>

namespace subcue {

// 对应 core/anchors.py::Anchor。start/end 为规范化码点流上的闭区间索引。
struct Anchor final {
    int lineIndex = -1;
    std::size_t start = 0;
    std::size_t end = 0;
    double confidence = 0.0;
};

class AnchorFinder final {
public:
    // 复刻 core/anchors.py::find_anchors：先找精确唯一匹配，
    // 短流上无匹配时做 RapidFuzz 模糊匹配，最后求单调子集。
    [[nodiscard]] static QVector<Anchor> findAnchors(
        const QStringList &lines, const std::u32string &stream);
};

} // namespace subcue
