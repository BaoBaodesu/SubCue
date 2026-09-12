#include "subtitle/subtitle_command_manager.h"

#include <QtGui/QUndoCommand>

#include <functional>
#include <utility>

namespace subcue {
namespace {

class SubtitleCommand final : public QUndoCommand {
public:
    SubtitleCommand(QString text, std::function<void()> redoAction,
                    std::function<void()> undoAction)
        : QUndoCommand(std::move(text)), redoAction_(std::move(redoAction)),
          undoAction_(std::move(undoAction))
    {
    }

    void redo() override { redoAction_(); }
    void undo() override { undoAction_(); }

private:
    std::function<void()> redoAction_;
    std::function<void()> undoAction_;
};

bool validRange(MediaTime start, MediaTime end)
{
    return start.microseconds() >= 0 && end > start;
}

} // namespace

SubtitleCommandManager::SubtitleCommandManager(SubtitleDocument *document, QObject *parent)
    : QObject(parent), document_(document), stack_(this)
{
    Q_ASSERT(document_);
    connect(document_, &SubtitleDocument::aboutToReset, this, &SubtitleCommandManager::clear);
    connect(&stack_, &QUndoStack::canUndoChanged, this, &SubtitleCommandManager::canUndoChanged);
    connect(&stack_, &QUndoStack::canRedoChanged, this, &SubtitleCommandManager::canRedoChanged);
}

bool SubtitleCommandManager::canUndo() const noexcept { return stack_.canUndo(); }
bool SubtitleCommandManager::canRedo() const noexcept { return stack_.canRedo(); }
QUndoStack *SubtitleCommandManager::stack() noexcept { return &stack_; }

bool SubtitleCommandManager::create(int index, Subtitle subtitle)
{
    if (!subtitle.isValid() || document_->indexOf(subtitle.id) >= 0) return false;
    index = qBound(0, index, document_->count());
    const QString id = subtitle.id;
    stack_.push(new SubtitleCommand(
        tr("创建字幕"),
        [document = document_, index, subtitle] { document->insertSubtitle(index, subtitle); },
        [document = document_, id] { document->takeSubtitle(id); }));
    return true;
}

bool SubtitleCommandManager::remove(const QString &id)
{
    int index = -1;
    const auto subtitle = document_->subtitle(id);
    index = document_->indexOf(id);
    if (!subtitle || index < 0) return false;
    stack_.push(new SubtitleCommand(
        tr("删除字幕"),
        [document = document_, id] { document->takeSubtitle(id); },
        [document = document_, index, subtitle = *subtitle] {
            document->insertSubtitle(index, subtitle);
        }));
    return true;
}

bool SubtitleCommandManager::replace(const QString &id, Subtitle updated,
                                     const QString &commandText)
{
    const auto original = document_->subtitle(id);
    if (!original || updated.id != id || !updated.isValid()) return false;
    stack_.push(new SubtitleCommand(
        commandText,
        [document = document_, id, updated] { document->replaceSubtitle(id, updated); },
        [document = document_, id, original = *original] {
            document->replaceSubtitle(id, original);
        }));
    return true;
}

bool SubtitleCommandManager::move(const QString &id, MediaTime start, MediaTime end)
{
    const auto current = document_->subtitle(id);
    if (!current || !validRange(start, end)) return false;
    Subtitle updated = *current;
    updated.start = start;
    updated.end = end;
    return replace(id, std::move(updated), tr("移动字幕"));
}

bool SubtitleCommandManager::trim(const QString &id, MediaTime start, MediaTime end)
{
    const auto current = document_->subtitle(id);
    if (!current || !validRange(start, end)) return false;
    Subtitle updated = *current;
    updated.start = start;
    updated.end = end;
    return replace(id, std::move(updated), tr("修剪字幕"));
}

bool SubtitleCommandManager::setTiming(const QString &id, MediaTime start, MediaTime end)
{
    const auto current = document_->subtitle(id);
    if (!current || !validRange(start, end)) return false;
    if (current->start == start && current->end == end
        && current->source == QStringLiteral("manual")
        && current->status == QStringLiteral("MANUAL")) {
        return true;
    }
    Subtitle updated = *current;
    updated.start = start;
    updated.end = end;
    updated.source = QStringLiteral("manual");
    updated.status = QStringLiteral("MANUAL");
    updated.metadata.insert(QStringLiteral("manualConfirmed"), true);
    return replace(id, std::move(updated), tr("调整字幕时间"));
}

bool SubtitleCommandManager::split(const QString &id, MediaTime splitTime,
                                   QString firstText, QString secondText)
{
    const auto original = document_->subtitle(id);
    const int originalIndex = document_->indexOf(id);
    if (!original || originalIndex < 0 || splitTime <= original->start || splitTime >= original->end) {
        return false;
    }
    Subtitle first = *original;
    first.end = splitTime;
    first.text = std::move(firstText);
    Subtitle second = *original;
    second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    second.start = splitTime;
    second.text = std::move(secondText);
    const QString secondId = second.id;
    stack_.push(new SubtitleCommand(
        tr("分割字幕"),
        [document = document_, id, originalIndex, first, second] {
            document->replaceSubtitle(id, first);
            document->insertSubtitle(originalIndex + 1, second);
        },
        [document = document_, id, secondId, original = *original] {
            document->takeSubtitle(secondId);
            document->replaceSubtitle(id, original);
        }));
    return true;
}

bool SubtitleCommandManager::merge(const QString &firstId, const QString &secondId, QString text)
{
    if (firstId == secondId) return false;
    const auto firstOriginal = document_->subtitle(firstId);
    const auto secondOriginal = document_->subtitle(secondId);
    const int secondIndex = document_->indexOf(secondId);
    if (!firstOriginal || !secondOriginal || secondIndex < 0) return false;
    Subtitle merged = *firstOriginal;
    merged.start = qMin(firstOriginal->start, secondOriginal->start);
    merged.end = qMax(firstOriginal->end, secondOriginal->end);
    merged.text = std::move(text);
    stack_.push(new SubtitleCommand(
        tr("合并字幕"),
        [document = document_, firstId, secondId, merged] {
            document->takeSubtitle(secondId);
            document->replaceSubtitle(firstId, merged);
        },
        [document = document_, firstId, secondIndex, first = *firstOriginal,
         second = *secondOriginal] {
            document->replaceSubtitle(firstId, first);
            document->insertSubtitle(secondIndex, second);
        }));
    return true;
}

bool SubtitleCommandManager::editText(const QString &id, QString text)
{
    const auto current = document_->subtitle(id);
    if (!current) return false;
    Subtitle updated = *current;
    updated.text = std::move(text);
    if (updated.isTimed()) {
        updated.status = QStringLiteral("MANUAL");
        updated.metadata.insert(QStringLiteral("manualConfirmed"), true);
    }
    return replace(id, std::move(updated), tr("修改字幕文本"));
}

void SubtitleCommandManager::undo() { stack_.undo(); }
void SubtitleCommandManager::redo() { stack_.redo(); }
void SubtitleCommandManager::clear() { stack_.clear(); }

} // namespace subcue
