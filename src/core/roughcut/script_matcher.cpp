#include "roughcut/script_matcher.h"

#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"

#include <QtCore/QHash>

#include <algorithm>
#include <atomic>

namespace subcue {
namespace {

double editSimilarity(const std::u32string &left, const std::u32string &right,
    const std::atomic<bool> *cancel)
{
    if (cancel && cancel->load()) return 0.0;
    if (left.empty() || right.empty()) return left == right ? 100.0 : 0.0;
    QVector<int> previous(static_cast<qsizetype>(right.size()) + 1);
    QVector<int> current(static_cast<qsizetype>(right.size()) + 1);
    for (int index = 0; index <= static_cast<int>(right.size()); ++index) previous[index] = index;
    for (int row = 1; row <= static_cast<int>(left.size()); ++row) {
        if (cancel && (row & 63) == 0 && cancel->load()) return 0.0;
        current[0] = row;
        for (int column = 1; column <= static_cast<int>(right.size()); ++column) {
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
                previous[column - 1] + (left.at(row - 1) == right.at(column - 1) ? 0 : 1)});
        }
        previous.swap(current);
    }
    return 100.0 * (1.0 - previous.constLast() / double(std::max(left.size(), right.size())));
}

struct ScriptIndex final {
    std::u32string text;
    QVector<int> starts;
    QVector<int> ends;

    int lineAt(int token) const
    {
        for (int index = 0; index < ends.size(); ++index) {
            if (token < ends.at(index)) return index;
        }
        return ends.isEmpty() ? -1 : ends.size() - 1;
    }
};

ScriptIndex indexScript(const ScriptDocument &script)
{
    ScriptIndex index;
    for (const ScriptLine &line : script.lines) {
        index.starts.append(static_cast<int>(index.text.size()));
        index.text += Normalizer::normalizeCodepoints(line.text);
        index.ends.append(static_cast<int>(index.text.size()));
    }
    return index;
}

struct Candidate final {
    ScriptMatch match;
    double score = 0.0;
};

QVector<Candidate> candidates(const ScriptIndex &script, const QString &text, int recordingIndex,
    const std::atomic<bool> *cancel)
{
    QVector<Candidate> result;
    if (cancel && cancel->load()) return result;
    const std::u32string spoken = Normalizer::normalizeCodepoints(text);
    if (spoken.empty() || script.text.empty()) return result;
    QVector<int> starts;
    starts.reserve(script.starts.size() + 64);
    for (int position : script.starts) {
        if (position < static_cast<int>(script.text.size())) starts.append(position);
    }
    for (int position = 0; position < static_cast<int>(script.text.size()); ++position) {
        if (script.text.at(position) == spoken.front()
            && (spoken.size() < 2 || position + 1 >= static_cast<int>(script.text.size())
                || script.text.at(position + 1) == spoken.at(1)))
            starts.append(position);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    for (int start : starts) {
        QVector<int> ends;
        const int length = static_cast<int>(spoken.size());
        for (int delta : {0, -2, 2, -length / 4, length / 4}) {
            ends.append(std::clamp(start + length + delta, start + 1,
                static_cast<int>(script.text.size())));
        }
        const int firstLine = script.lineAt(start);
        for (int line = firstLine; line < std::min(firstLine + 4, static_cast<int>(script.ends.size())); ++line) {
            if (script.ends.at(line) > start) ends.append(script.ends.at(line));
        }
        std::sort(ends.begin(), ends.end());
        ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
        for (int end : ends) {
            const std::u32string target = script.text.substr(start, end - start);
            const double fuzz = FuzzRatio::ratio(spoken, target);
            if (fuzz < 45.0) continue;
            const double edit = editSimilarity(spoken, target, cancel);
            if (cancel && cancel->load()) return result;
            const int lastLine = script.lineAt(end - 1);
            const int lineLength = std::max(1, script.ends.at(lastLine) - script.starts.at(firstLine));
            const double coverage = std::min(100.0, 100.0 * (end - start) / lineLength);
            const double combined = fuzz * 0.55 + edit * 0.45;
            if (combined < 52.0) continue;
            result.append({{recordingIndex, firstLine, ScriptMatchStatus::Modified,
                combined, edit, coverage, lastLine, start, end}, combined});
        }
    }
    std::sort(result.begin(), result.end(), [](const Candidate &left, const Candidate &right) {
        return left.score > right.score;
    });
    if (result.size() > 32) result.resize(32);
    return result;
}

struct Node final {
    ScriptMatch match;
    int parent = -1;
};

struct Beam final {
    int cursor = 0;
    double score = 0.0;
    int node = -1;
};

} // namespace

QVector<ScriptMatch> ScriptMatcher::match(
    const ScriptDocument &script, const QVector<RecognizedPassage> &recording,
    const std::atomic<bool> *cancel, bool *cancelled)
{
    if (cancelled) *cancelled = false;
    auto markCancelled = [cancelled] {
        if (cancelled) *cancelled = true;
        return QVector<ScriptMatch>{};
    };
    const ScriptIndex indexed = indexScript(script);
    QVector<Node> nodes;
    if (recording.size() > 0) nodes.reserve(recording.size() * 8 + 32);
    QVector<Beam> beams{{}};
    for (int recordingIndex = 0; recordingIndex < recording.size(); ++recordingIndex) {
        if (cancel && cancel->load()) return markCancelled();
        const QVector<Candidate> options = candidates(indexed,
            recording.at(recordingIndex).text, recordingIndex, cancel);
        if (cancel && cancel->load()) return markCancelled();
        QVector<Beam> expanded;
        expanded.reserve(beams.size() * (options.size() + 1));
        for (const Beam &beam : beams) {
            Beam added = beam;
            added.score -= 5.0;
            added.node = nodes.size();
            nodes.append({{recordingIndex, -1, ScriptMatchStatus::Added}, beam.node});
            expanded.append(added);
            for (const Candidate &option : options) {
                const int start = option.match.scriptTokenStart;
                if (start + 240 < beam.cursor) continue;
                Beam next = beam;
                next.score += (option.score - 58.0) / 2.0;
                if (start < beam.cursor) next.score -= 3.0 + (beam.cursor - start) * 0.04;
                else next.score -= std::min(24.0, (start - beam.cursor) * 0.06);
                next.cursor = std::max(beam.cursor, option.match.scriptTokenEnd);
                ScriptMatch match = option.match;
                match.status = start < beam.cursor ? ScriptMatchStatus::Retake
                    : option.score >= 86.0 ? ScriptMatchStatus::Match
                    : ScriptMatchStatus::Modified;
                next.node = nodes.size();
                nodes.append({match, beam.node});
                expanded.append(next);
            }
        }
        std::sort(expanded.begin(), expanded.end(), [](const Beam &left, const Beam &right) {
            return left.score > right.score;
        });
        QHash<int, bool> seen;
        beams.clear();
        for (Beam &beam : expanded) {
            if (seen.contains(beam.cursor)) continue;
            seen.insert(beam.cursor, true);
            beams.append(beam);
            if (beams.size() >= 24) break;
        }
    }
    QVector<ScriptMatch> result;
    if (beams.isEmpty()) return result;
    QVector<ScriptMatch> path;
    for (int node = beams.constFirst().node; node >= 0; node = nodes.at(node).parent)
        path.append(nodes.at(node).match);
    std::reverse(path.begin(), path.end());
    for (int index = 0; index + 1 < path.size(); ++index) {
        ScriptMatch &failed = path[index];
        ScriptMatch &next = path[index + 1];
        if (failed.scriptTokenStart >= 0 && next.scriptTokenStart == failed.scriptTokenStart
            && next.scriptTokenEnd >= failed.scriptTokenEnd
            && next.similarity >= 85.0 && next.similarity > failed.similarity + 5.0) {
            failed.status = ScriptMatchStatus::Retake;
            next.status = ScriptMatchStatus::Match;
        }
        if (failed.status != ScriptMatchStatus::Added || next.scriptTokenStart < 0
            || next.similarity < 75.0) continue;
        const std::u32string spoken = Normalizer::normalizeCodepoints(recording.at(index).text);
        const std::u32string replacement = Normalizer::normalizeCodepoints(recording.at(index + 1).text);
        int prefix = 0;
        while (prefix < static_cast<int>(std::min(spoken.size(), replacement.size()))
            && spoken.at(prefix) == replacement.at(prefix)) ++prefix;
        if (prefix < 3 || next.scriptTokenStart + prefix > static_cast<int>(indexed.text.size())
            || indexed.text.compare(next.scriptTokenStart, prefix, spoken, 0, prefix) != 0) continue;
        failed.scriptLineIndex = next.scriptLineIndex;
        failed.scriptLineEndIndex = next.scriptLineIndex;
        failed.scriptTokenStart = next.scriptTokenStart;
        failed.scriptTokenEnd = next.scriptTokenStart + prefix;
        failed.status = ScriptMatchStatus::Retake;
        failed.similarity = 55.0;
        failed.editSimilarity = 55.0;
        failed.continuousCoverage = std::min(100.0, 100.0 * prefix
            / std::max(1, indexed.ends.at(next.scriptLineIndex)
                - indexed.starts.at(next.scriptLineIndex)));
    }
    int cursor = 0;
    for (const ScriptMatch &match : path) {
        if (match.scriptLineIndex >= 0 && match.status != ScriptMatchStatus::Retake) {
            while (cursor < match.scriptLineIndex) {
                if (indexed.ends.at(cursor) > indexed.starts.at(cursor))
                    result.append({-1, cursor, ScriptMatchStatus::Skipped});
                ++cursor;
            }
            cursor = std::max(cursor, match.scriptLineEndIndex + 1);
        }
        result.append(match);
    }
    while (cursor < indexed.starts.size()) {
        if (indexed.ends.at(cursor) > indexed.starts.at(cursor))
            result.append({-1, cursor, ScriptMatchStatus::Skipped});
        ++cursor;
    }
    return result;
}

} // namespace subcue
