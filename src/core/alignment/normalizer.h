#pragma once

#include <QtCore/QString>
#include <QtCore/QStringView>

#include <string>

namespace subcue {

// 复刻 core/normalizer.py::normalize_text：
//   1. Unicode NFKC 规范化（QString::normalized）；
//   2. 转小写，语义对齐 CPython str.lower()（含 İ→i̇ 与 Final_Sigma）；
//   3. 删除 str.isspace() 为真或 Unicode 一般类别为 P*/S* 的字符。
// 过滤与 Cased 判断使用 tools/generate_alignment_golden.py 生成的表，
// 与 CPython 3.11 (unicodedata 14.0.0) 逐码点一致，不依赖 Qt 的 Unicode 版本。
class Normalizer final {
public:
    // QString 转码点序列（surrogate pair 合并为一个码点），
    // 码点索引与 Python 的 str 索引一致。
    [[nodiscard]] static std::u32string toCodepoints(QStringView text);

    // 规范化后的码点序列。
    [[nodiscard]] static std::u32string normalizeCodepoints(QStringView text);

    // 规范化后的 QString，用于文本展示与长度统计。
    [[nodiscard]] static QString normalizeText(QStringView text);
};

} // namespace subcue
