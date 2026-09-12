#include "alignment/normalizer.h"

#include "alignment/unicode_filter_data.h"

#include <QtCore/QChar>
#include <QtCore/QString>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace subcue {
namespace {

using alignment::detail::CodepointRange;

template <std::size_t Size>
[[nodiscard]] bool inRanges(char32_t codepoint, const CodepointRange (&ranges)[Size]) noexcept
{
    const auto *found = std::lower_bound(std::begin(ranges), std::end(ranges), codepoint,
        [](const CodepointRange &range, char32_t value) noexcept { return range.last < value; });
    return found != std::end(ranges) && found->first <= codepoint;
}

} // namespace

std::u32string Normalizer::toCodepoints(QStringView text)
{
    std::u32string out;
    out.reserve(static_cast<std::size_t>(text.size()));
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar current = text.at(index);
        if (current.isHighSurrogate() && index + 1 < text.size()
            && text.at(index + 1).isLowSurrogate()) {
            out.push_back(QChar::surrogateToUcs4(current, text.at(index + 1)));
            ++index;
        } else {
            out.push_back(static_cast<char32_t>(current.unicode()));
        }
    }
    return out;
}

std::u32string Normalizer::normalizeCodepoints(QStringView text)
{
    const QString nfkc = text.toString().normalized(QString::NormalizationForm_KC);
    const std::u32string source = toCodepoints(nfkc);

    // CPython str.lower() 语义：simple case mapping + İ / final sigma 特例。
    std::u32string lowered;
    lowered.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char32_t codepoint = source[index];
        if (codepoint == 0x0130) {
            // LATIN CAPITAL LETTER I WITH DOT ABOVE → i + U+0307
            lowered.push_back(U'i');
            lowered.push_back(0x0307);
            continue;
        }
        if (codepoint == 0x03A3) {
            // CPython Final_Sigma：前后跳过 Case_Ignorable，再看是否存在 Cased 字母。
            bool hasCasedAfter = false;
            for (std::size_t look = index + 1; look < source.size(); ++look) {
                if (inRanges(source[look], alignment::detail::kCasedRanges)) {
                    hasCasedAfter = true;
                    break;
                }
                if (!inRanges(source[look], alignment::detail::kCaseIgnorableRanges)) {
                    break;
                }
            }
            bool hasCasedBefore = false;
            for (std::size_t look = index; look > 0;) {
                --look;
                if (inRanges(source[look], alignment::detail::kCasedRanges)) {
                    hasCasedBefore = true;
                    break;
                }
                if (!inRanges(source[look], alignment::detail::kCaseIgnorableRanges)) {
                    break;
                }
            }
            lowered.push_back((!hasCasedAfter && hasCasedBefore) ? 0x03C2 : 0x03C3);
            continue;
        }
        lowered.push_back(QChar::toLower(codepoint));
    }

    std::u32string out;
    out.reserve(lowered.size());
    for (const char32_t codepoint : lowered) {
        if (!inRanges(codepoint, alignment::detail::kNormalizeDropRanges)) {
            out.push_back(codepoint);
        }
    }
    return out;
}

QString Normalizer::normalizeText(QStringView text)
{
    const std::u32string codepoints = normalizeCodepoints(text);
    return QString::fromUcs4(codepoints.data(), static_cast<qsizetype>(codepoints.size()));
}

} // namespace subcue
