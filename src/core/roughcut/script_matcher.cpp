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
        double bestScore = 0.0;
        const int upper = std::min(cursor + 4, static_cast<int>(script.lines.size()));
        for (int line = cursor; line < upper; ++line) {
            if (script.lines.at(line).text.trimmed().isEmpty()) continue;
            const double score = similarity(recording.at(recordingIndex).text, script.lines.at(line).text);
            if (score > bestScore) {
                bestScore = score;
                bestLine = line;
            }
        }

        int retakeLine = -1;
        double retakeScore = 0.0;
        for (int line = 0; line < cursor; ++line) {
            if (matchedRecording.at(line) < 0) continue;
            const double score = similarity(recording.at(recordingIndex).text, script.lines.at(line).text);
            if (score > retakeScore) {
                retakeScore = score;
                retakeLine = line;
            }
        }
        if (retakeScore >= 75.0 && retakeScore > bestScore) {
            matches.append({recordingIndex, retakeLine, ScriptMatchStatus::Retake, retakeScore});
            continue;
        }
        if (bestLine < 0 || bestScore < 60.0) {
            matches.append({recordingIndex, -1, ScriptMatchStatus::Added, bestScore});
            continue;
        }
        while (cursor < bestLine) {
            if (!script.lines.at(cursor).text.trimmed().isEmpty()) {
                matches.append({-1, cursor, ScriptMatchStatus::Skipped, 0.0});
            }
            ++cursor;
        }
        matches.append({recordingIndex, bestLine,
            bestScore >= 90.0 ? ScriptMatchStatus::Match : ScriptMatchStatus::Modified,
            bestScore});
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
