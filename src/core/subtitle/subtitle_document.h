#pragma once

#include "subtitle/subtitle.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <optional>

namespace subcue {

class SubtitleDocument final : public QObject {
    Q_OBJECT

public:
    explicit SubtitleDocument(QObject *parent = nullptr);

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] const QList<Subtitle> &subtitles() const noexcept;
    [[nodiscard]] std::optional<Subtitle> subtitle(const QString &id) const;
    [[nodiscard]] int indexOf(const QString &id) const noexcept;

    void setSubtitles(QList<Subtitle> subtitles);
    bool insertSubtitle(int index, Subtitle subtitle);
    std::optional<Subtitle> takeSubtitle(const QString &id, int *index = nullptr);
    bool replaceSubtitle(const QString &id, Subtitle subtitle);

signals:
    void aboutToReset();
    void reset();
    void aboutToInsert(int index);
    void inserted();
    void aboutToRemove(int index);
    void removed();
    void changed(int index);

private:
    QList<Subtitle> subtitles_;
};

} // namespace subcue
