#pragma once

#include "subtitle/subtitle_document.h"

#include <QtCore/QObject>
#include <QtCore/QVector>
#include <QtGui/QUndoStack>

namespace subcue {

class SubtitleCommandManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)

public:
    explicit SubtitleCommandManager(SubtitleDocument *document, QObject *parent = nullptr);

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QUndoStack *stack() noexcept;

    bool create(int index, Subtitle subtitle);
    bool remove(const QString &id);
    bool move(const QString &id, MediaTime start, MediaTime end);
    bool trim(const QString &id, MediaTime start, MediaTime end);
    bool setTiming(const QString &id, MediaTime start, MediaTime end);
    bool split(const QString &id, MediaTime splitTime, QString firstText, QString secondText);
    bool merge(const QString &firstId, const QString &secondId, QString text);
    bool editText(const QString &id, QString text);
    bool replaceMany(QVector<Subtitle> updated, const QString &commandText);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clear();

signals:
    void canUndoChanged();
    void canRedoChanged();

private:
    bool replace(const QString &id, Subtitle updated, const QString &commandText);

    SubtitleDocument *document_;
    QUndoStack stack_;
};

} // namespace subcue
