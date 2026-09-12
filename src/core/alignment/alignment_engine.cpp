#include "alignment/alignment_engine.h"

#include "alignment/anchors.h"
#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"
#include "alignment/python_round.h"

#include <QtCore/QString>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

namespace subcue {
namespace {

struct CharToken final {
    char32_t codepoint;
    qint64 wordId;
    qint64 startMs;
    qint64 endMs;
};

struct CharTimeline final {
    QVector<CharToken> tokens;
    std::u32string stream;
};

struct Candidate final {
    std::size_t start;
    std::size_t end;
    double similarity;
    double score;
    double ambiguity = 1.0;
};

// 复刻 core/alignment.py::build_char_timeline。
[[nodiscard]] CharTimeline buildCharTimeline(const QVector<TranscriptWord> &words)
{
    CharTimeline out;
    for (const TranscriptWord &word : words) {
        const std::u32string normalized = Normalizer::normalizeCodepoints(word.text);
        if (normalized.empty()) {
            continue;
        }
        const qint64 duration = std::max<qint64>(0, word.endMs - word.startMs);
        const double count = static_cast<double>(normalized.size());
        for (std::size_t index = 0; index < normalized.size(); ++index) {
            const qint64 start = word.startMs + pythonRound(
                static_cast<double>(duration * static_cast<qint64>(index)) / count);
            const qint64 end = word.startMs + pythonRound(
                static_cast<double>(duration * static_cast<qint64>(index + 1)) / count);
            const qint64 cappedEnd = std::min(word.endMs, std::max(start, end));
            out.tokens.append(CharToken{normalized[index], word.id, start, cappedEnd});
            out.stream.push_back(normalized[index]);
        }
    }
    return out;
}

// 复刻 core/alignment.py::_find_candidates。
[[nodiscard]] QVector<Candidate> findCandidates(
    const std::u32string &target, const std::u32string &stream,
    std::size_t left, std::size_t right, std::size_t expected, std::size_t regionLength)
{
    if (target.empty() || right <= left) {
        return {};
    }
    const std::size_t targetSize = target.size();

    std::vector<std::size_t> lengths;
    for (const double ratio : {0.72, 0.86, 1.0, 1.14, 1.30}) {
        const std::size_t length = std::max<std::size_t>(
            1, static_cast<std::size_t>(pythonRound(static_cast<double>(targetSize) * ratio)));
        if (std::find(lengths.begin(), lengths.end(), length) == lengths.end()) {
            lengths.push_back(length);
        }
    }
    std::sort(lengths.begin(), lengths.end());

    struct ScoredItem final {
        double score;
        std::size_t start;
        std::size_t end;
        double similarity;
    };
    // Python heapq 是最小堆，元素为 (score, start, end, similarity)。
    // 满 240 后只在新 score 严格大于堆顶（当前最差）时 heapreplace。
    struct ScoredItemMinHeap final {
        bool operator()(const ScoredItem &a, const ScoredItem &b) const noexcept
        {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.start != b.start) {
                return a.start > b.start;
            }
            if (a.end != b.end) {
                return a.end > b.end;
            }
            return a.similarity > b.similarity;
        }
    };
    std::priority_queue<ScoredItem, std::vector<ScoredItem>, ScoredItemMinHeap> scored;

    for (std::size_t start = left; start < right; ++start) {
        for (const std::size_t length : lengths) {
            const std::size_t end = start + length;
            if (end > right) {
                continue;
            }
            const double similarity = FuzzRatio::ratio(
                target.data(), target.size(), stream.data() + start, length) / 100.0;
            const double lengthPenalty = static_cast<double>(
                std::abs(static_cast<std::ptrdiff_t>(length - targetSize)))
                / static_cast<double>(std::max<std::size_t>(1, targetSize)) * 0.12;
            const double orderPenalty = static_cast<double>(
                std::abs(static_cast<std::ptrdiff_t>(start - expected)))
                / static_cast<double>(std::max<std::size_t>(1, regionLength)) * 0.08;
            const double score = similarity - lengthPenalty - orderPenalty;
            const ScoredItem item{score, start, end - 1, similarity};
            if (scored.size() < 240) {
                scored.push(item);
            } else if (item.score > scored.top().score) {
                scored.pop();
                scored.push(item);
            }
        }
    }

    std::vector<ScoredItem> ordered;
    ordered.reserve(scored.size());
    while (!scored.empty()) {
        ordered.push_back(scored.top());
        scored.pop();
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const ScoredItem &a, const ScoredItem &b) noexcept {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.start != b.start) {
                return a.start > b.start;
            }
            if (a.end != b.end) {
                return a.end > b.end;
            }
            return a.similarity > b.similarity;
        });

    QVector<Candidate> selected;
    const std::size_t minimumGap = std::max<std::size_t>(1, targetSize / 4);
    for (const ScoredItem &item : ordered) {
        const bool farEnough = std::all_of(selected.begin(), selected.end(),
            [&](const Candidate &other) {
                return std::abs(static_cast<std::ptrdiff_t>(item.start)
                        - static_cast<std::ptrdiff_t>(other.start))
                    > static_cast<std::ptrdiff_t>(minimumGap);
            });
        if (farEnough) {
            selected.append(Candidate{item.start, item.end, item.similarity, item.score, 1.0});
        }
        if (selected.size() == 12) {
            break;
        }
    }
    if (selected.isEmpty()) {
        return {};
    }
    const double ambiguity = std::max(0.0, selected[0].similarity
        - (selected.size() > 1 ? selected[1].similarity : 0.0));
    for (Candidate &candidate : selected) {
        candidate.ambiguity = ambiguity;
    }
    return selected;
}

// 复刻 core/alignment.py::_align_segment。返回每个 segment 行的候选（可为空）。
[[nodiscard]] QVector<std::optional<Candidate>> alignSegment(
    const QStringList &lines, const std::u32string &stream,
    const QVector<int> &indices, std::size_t lower, std::size_t upper)
{
    std::vector<std::u32string> targets;
    std::size_t totalLength = 0;
    for (const int index : indices) {
        targets.push_back(Normalizer::normalizeCodepoints(lines.at(index)));
        totalLength += std::max<std::size_t>(1, targets.back().size());
    }

    struct LineCandidates final {
        QVector<Candidate> candidates;
    };
    std::vector<LineCandidates> candidatesByLine;
    candidatesByLine.reserve(targets.size());
    std::size_t consumed = 0;
    const double totalLengthDouble = static_cast<double>(std::max<std::size_t>(1, totalLength));
    for (const std::u32string &target : targets) {
        const std::size_t targetSize = target.size();
        const std::size_t expected = lower + static_cast<std::size_t>(pythonRound(
            static_cast<double>((upper - lower) * consumed) / totalLengthDouble));
        const std::size_t expectedSpan = std::max<std::size_t>(20, static_cast<std::size_t>(pythonRound(
            static_cast<double>((upper - lower) * std::max<std::size_t>(1, targetSize))
            / totalLengthDouble)));
        const std::size_t radius = std::max<std::size_t>(100, expectedSpan * 3);
        const std::size_t searchLeft = std::max(lower, expected > radius ? expected - radius : 0);
        const std::size_t searchRight = std::min(
            upper, expected + radius + std::max<std::size_t>(1, targetSize));
        LineCandidates line;
        line.candidates = findCandidates(
            target, stream, searchLeft, searchRight, expected, upper - lower);
        candidatesByLine.push_back(std::move(line));
        consumed += std::max<std::size_t>(1, targetSize);
    }

    struct BeamState final {
        double score = 0.0;
        std::ptrdiff_t lastEnd = 0;
        QVector<std::optional<Candidate>> choices;
    };
    std::vector<BeamState> beam;
    beam.push_back(BeamState{0.0, static_cast<std::ptrdiff_t>(lower) - 1, {}});
    for (const LineCandidates &line : candidatesByLine) {
        std::vector<BeamState> expanded;
        expanded.reserve(beam.size() * (1 + line.candidates.size()));
        for (const BeamState &state : beam) {
            BeamState skipped = state;
            skipped.score -= 0.38;
            skipped.choices.append(std::nullopt);
            expanded.push_back(std::move(skipped));
            for (const Candidate &candidate : line.candidates) {
                if (static_cast<std::ptrdiff_t>(candidate.start) > state.lastEnd) {
                    BeamState chosen = state;
                    chosen.score += candidate.score;
                    chosen.lastEnd = static_cast<std::ptrdiff_t>(candidate.end);
                    chosen.choices.append(candidate);
                    expanded.push_back(std::move(chosen));
                }
            }
        }
        std::stable_sort(expanded.begin(), expanded.end(),
            [](const BeamState &a, const BeamState &b) noexcept { return a.score > b.score; });
        if (expanded.size() > 80) {
            expanded.resize(80);
        }
        beam = std::move(expanded);
    }
    auto best = beam.begin();
    for (auto it = beam.begin() + 1; it != beam.end(); ++it) {
        if (it->score > best->score) {
            best = it;
        }
    }
    return best->choices;
}

// 复刻 core/alignment.py::_candidate_words。
[[nodiscard]] QString candidateWords(const QVector<TranscriptWord> &words, qint64 startId, qint64 endId)
{
    QString out;
    for (const TranscriptWord &word : words) {
        if (startId <= word.id && word.id <= endId) {
            out += word.text;
        }
    }
    return out;
}

// 复刻 core/alignment.py::_apply_candidate。
void applyCandidate(Subtitle &subtitle, const std::optional<Candidate> &candidate,
    const QVector<CharToken> &timeline, const QVector<TranscriptWord> &words)
{
    if (!candidate) {
        return;
    }
    const CharToken &first = timeline.at(static_cast<qsizetype>(candidate->start));
    const CharToken &last = timeline.at(static_cast<qsizetype>(candidate->end));
    subtitle.start = MediaTime::fromMilliseconds(first.startMs);
    subtitle.end = MediaTime::fromMilliseconds(last.endMs);
    subtitle.startWordId = first.wordId;
    subtitle.endWordId = last.wordId;
    subtitle.candidateText = candidateWords(words, first.wordId, last.wordId);
    subtitle.ambiguity = candidate->ambiguity;
    const qint64 targetLength = static_cast<qint64>(
        Normalizer::normalizeCodepoints(subtitle.text).size());
    const qint64 candidateLength = static_cast<qint64>(candidate->end - candidate->start + 1);
    const qint64 minimum = std::min(targetLength, candidateLength);
    const qint64 maximum = std::max<qint64>(1, std::max(targetLength, candidateLength));
    subtitle.confidence = AlignmentEngine::calculateConfidence(
        candidate->similarity, candidate->ambiguity,
        static_cast<double>(minimum) / static_cast<double>(maximum));
    if (candidate->ambiguity < 0.04) {
        subtitle.confidence = std::min(subtitle.confidence, 0.69);
    }
    subtitle.status = subtitle.confidence >= 0.70
        ? QStringLiteral("MATCHED")
        : QStringLiteral("LOW_CONFIDENCE");
}

// 复刻 core/alignment.py::_apply_anchor。
void applyAnchor(Subtitle &subtitle, std::ptrdiff_t start, std::ptrdiff_t end,
    double confidence, const QVector<CharToken> &timeline, const QVector<TranscriptWord> &words)
{
    const Candidate candidate{static_cast<std::size_t>(start), static_cast<std::size_t>(end),
        confidence, confidence, 1.0};
    applyCandidate(subtitle, candidate, timeline, words);
    subtitle.confidence = std::max(0.98, confidence);
    subtitle.status = QStringLiteral("HIGH_CONFIDENCE_ANCHOR");
}

// 复刻 core/alignment.py::resolve_timing。
void resolveTiming(QVector<Subtitle> &subtitles)
{
    qint64 cursor = 0;
    for (Subtitle &subtitle : subtitles) {
        if (subtitle.end <= subtitle.start) {
            continue;
        }
        if (subtitle.start.milliseconds() < cursor) {
            subtitle.start = MediaTime::fromMilliseconds(cursor);
        }
        if (subtitle.end.milliseconds() - subtitle.start.milliseconds() < 250) {
            subtitle.start = MediaTime{};
            subtitle.end = MediaTime{};
            subtitle.status = QStringLiteral("SKIPPED_NO_AUDIO");
            subtitle.skipReason = QStringLiteral(u"有效语音区间不足 250ms");
            continue;
        }
        cursor = subtitle.end.milliseconds();
    }
}

} // namespace

QString AlignmentEngine::normalizeText(QStringView text)
{
    return Normalizer::normalizeText(text);
}

double AlignmentEngine::fuzzRatio(QStringView left, QStringView right)
{
    return FuzzRatio::ratio(left, right);
}

double AlignmentEngine::calculateConfidence(
    double similarity, double ambiguity, double lengthRatio) noexcept
{
    const double value = similarity * 0.86 + std::min(1.0, lengthRatio) * 0.09
        + std::min(0.05, ambiguity);
    return std::max(0.0, std::min(1.0, value));
}

bool AlignmentEngine::needsAiReview(double confidence, double ambiguity) noexcept
{
    return confidence < 0.75 || ambiguity < 0.04;
}

AlignmentResult AlignmentEngine::alignSubtitleLines(
    const QStringList &lines, const QVector<TranscriptWord> &words)
{
    AlignmentResult result;
    result.subtitles.reserve(lines.size());
    for (const QString &line : lines) {
        Subtitle subtitle;
        subtitle.text = line;
        result.subtitles.append(std::move(subtitle));
    }

    CharTimeline timeline = buildCharTimeline(words);
    if (timeline.tokens.isEmpty()) {
        return result;
    }
    const QVector<Anchor> anchors = AnchorFinder::findAnchors(lines, timeline.stream);

    // 复刻 boundaries = [Anchor(-1,-1,-1,1.0), *anchors, Anchor(len, len, len, 1.0)]
    struct Boundary final {
        int lineIndex = -1;
        std::ptrdiff_t start = 0;
        std::ptrdiff_t end = 0;
        double confidence = 1.0;
    };
    QVector<Boundary> boundaries;
    boundaries.append(Boundary{-1, -1, -1, 1.0});
    for (const Anchor &anchor : anchors) {
        boundaries.append(Boundary{anchor.lineIndex,
            static_cast<std::ptrdiff_t>(anchor.start),
            static_cast<std::ptrdiff_t>(anchor.end), anchor.confidence});
    }
    boundaries.append(Boundary{static_cast<int>(lines.size()),
        static_cast<std::ptrdiff_t>(timeline.stream.size()),
        static_cast<std::ptrdiff_t>(timeline.stream.size()), 1.0});

    const qsizetype lineCount = lines.size();
    for (qsizetype index = 0; index + 1 < boundaries.size(); ++index) {
        const Boundary &left = boundaries[index];
        const Boundary &right = boundaries[index + 1];
        QVector<int> segmentIndices;
        for (int lineIndex = left.lineIndex + 1; lineIndex < right.lineIndex; ++lineIndex) {
            segmentIndices.append(lineIndex);
        }
        if (!segmentIndices.isEmpty()) {
            const QVector<std::optional<Candidate>> choices = alignSegment(
                lines, timeline.stream, segmentIndices,
                static_cast<std::size_t>(left.end + 1),
                static_cast<std::size_t>(right.start));
            for (qsizetype k = 0; k < segmentIndices.size(); ++k) {
                applyCandidate(result.subtitles[segmentIndices[k]], choices[k], timeline.tokens, words);
            }
        }
        if (right.lineIndex < lineCount) {
            applyAnchor(result.subtitles[right.lineIndex], right.start, right.end,
                right.confidence, timeline.tokens, words);
        }
    }
    resolveTiming(result.subtitles);
    return result;
}

void AlignmentEngine::finalizeAudioFirst(QVector<Subtitle> &subtitles)
{
    for (Subtitle &subtitle : subtitles) {
        if (subtitle.status == QStringLiteral("SKIPPED_NO_AUDIO")) {
            continue;
        }
        if (subtitle.end <= subtitle.start) {
            subtitle.start = MediaTime{};
            subtitle.end = MediaTime{};
            subtitle.status = QStringLiteral("SKIPPED_NO_AUDIO");
            if (subtitle.skipReason.isEmpty()) {
                subtitle.skipReason = QStringLiteral(u"音频中未找到对应片段");
            }
        } else if (subtitle.confidence < 0.70 && subtitle.startWordId >= 0
                   && subtitle.endWordId >= subtitle.startWordId) {
            // 真实词区间保留为待确认，低分本身不等于缺少音频。
            subtitle.status = QStringLiteral("LOW_CONFIDENCE");
        } else if (subtitle.confidence < 0.70) {
            subtitle.start = MediaTime{};
            subtitle.end = MediaTime{};
            subtitle.status = QStringLiteral("SKIPPED_NO_AUDIO");
            if (subtitle.skipReason.isEmpty()) {
                subtitle.skipReason = QStringLiteral(u"没有达到可靠音频匹配阈值");
            }
        }
    }
    resolveTiming(subtitles);
}

} // namespace subcue
