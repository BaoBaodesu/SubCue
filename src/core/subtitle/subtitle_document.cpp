#include "subtitle/subtitle_document.h"

#include <utility>

namespace subcue {

SubtitleDocument::SubtitleDocument(QObject *parent) : QObject(parent) {}

int SubtitleDocument::count() const noexcept
{
    return static_cast<int>(subtitles_.size());
}

const QList<Subtitle> &SubtitleDocument::subtitles() const noexcept
{
    return subtitles_;
}

std::optional<Subtitle> SubtitleDocument::subtitle(const QString &id) const
{
    const int index = indexOf(id);
    if (index < 0) return std::nullopt;
    return subtitles_.at(index);
}

int SubtitleDocument::indexOf(const QString &id) const noexcept
{
    for (qsizetype index = 0; index < subtitles_.size(); ++index) {
        if (subtitles_.at(index).id == id) return static_cast<int>(index);
    }
    return -1;
}

void SubtitleDocument::setSubtitles(QList<Subtitle> subtitles)
{
    emit aboutToReset();
    subtitles_ = std::move(subtitles);
    emit reset();
}

bool SubtitleDocument::insertSubtitle(int index, Subtitle subtitle)
{
    if (subtitle.id.isEmpty() || indexOf(subtitle.id) >= 0) return false;
    index = qBound(0, index, count());
    emit aboutToInsert(index);
    subtitles_.insert(index, std::move(subtitle));
    emit inserted();
    return true;
}

std::optional<Subtitle> SubtitleDocument::takeSubtitle(const QString &id, int *index)
{
    const int found = indexOf(id);
    if (found < 0) return std::nullopt;
    emit aboutToRemove(found);
    Subtitle result = subtitles_.takeAt(found);
    emit removed();
    if (index) *index = found;
    return result;
}

bool SubtitleDocument::replaceSubtitle(const QString &id, Subtitle subtitle)
{
    const int index = indexOf(id);
    if (index < 0 || subtitle.id != id) return false;
    subtitles_[index] = std::move(subtitle);
    emit changed(index);
    return true;
}

} // namespace subcue
