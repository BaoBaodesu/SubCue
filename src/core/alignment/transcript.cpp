#include "alignment/transcript.h"

#include <algorithm>

namespace subcue {

void Transcript::sortAndReindex()
{
    std::stable_sort(words.begin(), words.end(),
        [](const TranscriptWord &left, const TranscriptWord &right) noexcept {
            if (left.startMs != right.startMs) {
                return left.startMs < right.startMs;
            }
            return left.endMs < right.endMs;
        });
    qint64 index = 1;
    for (TranscriptWord &word : words) {
        word.id = index++;
    }
}

} // namespace subcue
