#include "alignment/anchors.h"

#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"
#include "alignment/python_round.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace subcue {
namespace {

// 复刻 core/anchors.py::_monotonic_subset：按行号与位置递增选择权重最大的锚点子集。
[[nodiscard]] QVector<Anchor> monotonicSubset(const QVector<Anchor> &anchors)
{
    if (anchors.isEmpty()) {
        return {};
    }
    const qsizetype count = anchors.size();
    QVector<double> bestScore(count);
    QVector<int> previous(count, -1);
    for (qsizetype current = 0; current < count; ++current) {
        const double weight = static_cast<double>(anchors[current].end - anchors[current].start + 1)
            + anchors[current].confidence;
        bestScore[current] = weight;
        for (qsizetype prior = 0; prior < current; ++prior) {
            if (anchors[prior].lineIndex < anchors[current].lineIndex
                && anchors[prior].end < anchors[current].start
                && bestScore[prior] + weight > bestScore[current]) {
                bestScore[current] = bestScore[prior] + weight;
                previous[current] = static_cast<int>(prior);
            }
        }
    }
    qsizetype index = 0;
    for (qsizetype i = 1; i < count; ++i) {
        if (bestScore[i] > bestScore[index]) {
            index = i;
        }
    }
    QVector<Anchor> selected;
    while (index >= 0) {
        selected.append(anchors[index]);
        index = previous[index];
    }
    std::reverse(selected.begin(), selected.end());
    return selected;
}

} // namespace

QVector<Anchor> AnchorFinder::findAnchors(const QStringList &lines, const std::u32string &stream)
{
    QVector<Anchor> candidates;
    const qsizetype lineCount = lines.size();
    const std::size_t streamSize = stream.size();
    for (qsizetype lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        const std::u32string target = Normalizer::normalizeCodepoints(lines.at(lineIndex));
        if (target.size() < 3) {
            continue;
        }

        std::vector<std::size_t> positions;
        for (std::size_t pos = stream.find(target); pos != std::u32string::npos;
             pos = stream.find(target, pos + 1)) {
            positions.push_back(pos);
        }
        if (positions.size() == 1) {
            candidates.append(Anchor{static_cast<int>(lineIndex), positions[0],
                positions[0] + target.size() - 1, 1.0});
            continue;
        }
        if (!positions.empty() || streamSize > 80'000) {
            continue;
        }

        const std::size_t lineDivisor = static_cast<std::size_t>(std::max<qsizetype>(1, lineCount));
        const std::size_t expected = static_cast<std::size_t>(pythonRound(
            static_cast<double>(lineIndex) / static_cast<double>(lineDivisor)
            * static_cast<double>(streamSize)));
        const std::size_t radius = std::max<std::size_t>(200, streamSize / lineDivisor * 3);
        const std::size_t left = expected > radius ? expected - radius : 0;
        const std::size_t right = std::min(streamSize, expected + radius);

        struct ScoredPosition {
            double similarity;
            std::size_t position;
        };
        std::vector<ScoredPosition> scored;
        const std::size_t lastPosition = right >= target.size() ? right - target.size() + 1 : 0;
        const std::size_t scanEnd = std::max(left, lastPosition);
        for (std::size_t pos = left; pos < scanEnd; ++pos) {
            scored.push_back({FuzzRatio::ratio(
                target.data(), target.size(), stream.data() + pos, target.size()) / 100.0, pos});
        }
        std::sort(scored.begin(), scored.end(),
            [](const ScoredPosition &a, const ScoredPosition &b) noexcept {
                if (a.similarity != b.similarity) {
                    return a.similarity > b.similarity;
                }
                return a.position > b.position;
            });
        if (!scored.empty() && scored[0].similarity >= 0.92
            && (scored.size() == 1 || scored[0].similarity - scored[1].similarity >= 0.04)) {
            candidates.append(Anchor{static_cast<int>(lineIndex), scored[0].position,
                scored[0].position + target.size() - 1, scored[0].similarity});
        }
    }
    return monotonicSubset(candidates);
}

} // namespace subcue
