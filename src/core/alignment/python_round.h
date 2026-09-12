#pragma once

#include <QtCore/QtGlobal>

#include <cmath>

namespace subcue {

// CPython round() 语义：四舍六入五取偶（ties-to-even），返回整数。
// 对负数同样取“离零更远方向不成立、离值最近的偶数”，与 CPython 一致
// （round(-0.5) == 0，round(-1.5) == -2）。
[[nodiscard]] inline qint64 pythonRound(double value) noexcept
{
    const double floored = std::floor(value);
    const double fraction = value - floored;
    const qint64 lower = static_cast<qint64>(floored);
    if (fraction < 0.5) {
        return lower;
    }
    if (fraction > 0.5) {
        return lower + 1;
    }
    // fraction == 0.5：取最近的偶数。0.5 与这些商都是精确二进制值，
    // 因此浮点环境不会影响这里的判断。
    return (lower & 1) == 0 ? lower : lower + 1;
}

} // namespace subcue
