#include "roughcut/script_matcher.h"

#include "alignment/fuzz_ratio.h"
#include "alignment/normalizer.h"

#include <algorithm>

namespace subcue {
namespace {

double similarity(const QString &left, const QString &right)
{
    return FuzzRatio::ratio(
        Normalizer::normalizeCodepoints(left), Normalizer::normalizeCodepoints(right));
}

double editSimilarity(const QString &left, const QString &right)
{
    const std::u32string a = Normalizer::normalizeCodepoints(left);
    const std::u32string b = Normalizer::normalizeCodepoints(right);
    if (a.empty() || b.empty()) return a == b ? 100.0 : 0.0;
    QVector<int> previous(static_cast<qsizetype>(b.size()) + 1);
    QVector<int> current(static_cast<qsizetype>(b.size()) + 1);
    for (int index = 0; index <= static_cast<int>(b.size()); ++index) previous[index] = index;
    for (int row = 1; row <= static_cast<int>(a.size()); ++row) {
        current[0] = row;
        for (int column = 1; column <= static_cast<int>(b.size()); ++column) {
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
                previous[column - 1] + (a.at(row - 1) == b.at(column - 1) ? 0 : 1)});
        }
        previous.swap(current);
    }
    return 100.0 * (1.0 - previous.constLast() / double(std::max(a.size(), b.size())));
}

double continuousCoverage(const QString &recognized, const QString &script)
{
    const std::u32string a = Normalizer::normalizeCodepoints(recognized);
    const std::u32string b = Normalizer::normalizeCodepoints(script);
    if (a.empty() || b.empty()) return 0.0;
    QVector<int> previous(static_cast<qsizetype>(b.size()) + 1);
    QVector<int> current(static_cast<qsizetype>(b.size()) + 1);
    int longest = 0;
    for (int row = 1; row <= static_cast<int>(a.size()); ++row) {
        for (int column = 1; column <= static_cast<int>(b.size()); ++column) {
            current[column] = a.at(row - 1) == b.at(column - 1)
                ? previous.at(column - 1) + 1 : 0;
            longest = std::max(longest, current.at(column));
        }
        previous.swap(current);
        current.fill(0);
    }
    return 100.0 * longest / b.size();
}

struct MatchScore final {
    double combined = 0.0;
    double edit = 0.0;
    double coverage = 0.0;
};

MatchScore score(const QString &recognized, const QString &script)
{
    MatchScore result;
    const double fuzz = similarity(recognized, script);
    result.edit = editSimilarity(recognized, script);
    result.coverage = continuousCoverage(recognized, script);
    result.combined = fuzz * 0.45 + result.edit * 0.35 + result.coverage * 0.20;
    return result;
}

} // namespace

QVector<ScriptMatch> ScriptMatcher::match(
    const ScriptDocument &script,
    const QVector<RecognizedPassage> &recording)
{
    QVector<ScriptMatch> matches;
    QVector<int> matchedRecording(script.lines.size(), -1);
    int cursor = 0;
    for (int recordingIndex = 0; recordingIndex < recording.size(); ++recordingIndex) {
        int bestLine = -1;
        MatchScore bestScore;
        const int upper = std::min(cursor + 4, static_cast<int>(script.lines.size()));
        for (int line = cursor; line < upper; ++line) {
            if (script.lines.at(line).text.trimmed().isEmpty()) continue;
            const MatchScore candidate = score(recording.at(recordingIndex).text, script.lines.at(line).text);
            if (candidate.combined > bestScore.combined) {
                bestScore = candidate;
                bestLine = line;
            }
        }

        int retakeLine = -1;
        MatchScore retakeScore;
        for (int line = 0; line < cursor; ++line) {
            if (matchedRecording.at(line) < 0) continue;
            const MatchScore candidate = score(recording.at(recordingIndex).text, script.lines.at(line).text);
            if (candidate.combined > retakeScore.combined) {
                retakeScore = candidate;
                retakeLine = line;
            }
        }
        if (retakeScore.combined >= 65.0 && retakeScore.combined > bestScore.combined) {
            matches.append({recordingIndex, retakeLine, ScriptMatchStatus::Retake,
                retakeScore.combined, retakeScore.edit, retakeScore.coverage});
            continue;
        }
        if (bestLine < 0 || bestScore.combined < 55.0) {
            matches.append({recordingIndex, -1, ScriptMatchStatus::Added,
                bestScore.combined, bestScore.edit, bestScore.coverage});
            continue;
        }
        while (cursor < bestLine) {
            if (!script.lines.at(cursor).text.trimmed().isEmpty()) {
                matches.append({-1, cursor, ScriptMatchStatus::Skipped, 0.0});
            }
            ++cursor;
        }
        matches.append({recordingIndex, bestLine,
            bestScore.combined >= 88.0 ? ScriptMatchStatus::Match : ScriptMatchStatus::Modified,
            bestScore.combined, bestScore.edit, bestScore.coverage});
        matchedRecording[bestLine] = recordingIndex;
        cursor = bestLine + 1;
    }
    while (cursor < script.lines.size()) {
        if (!script.lines.at(cursor).text.trimmed().isEmpty()) {
            matches.append({-1, cursor, ScriptMatchStatus::Skipped, 0.0});
        }
        ++cursor;
    }
    return matches;
}

} // namespace subcue
