#pragma once

#include "common/media_time.h"
#include "subtitle/subtitle.h"
#include "subtitle/subtitle_command_manager.h"
#include "subtitle/subtitle_document.h"
#include "timeline/snap_engine.h"
#include "timeline/timeline_viewport.h"

#include <QtCore/QList>
#include <QtCore/QString>

#include <optional>

namespace subcue {

enum class CueDragMode {
    Move,
    TrimStart,
    TrimEnd
};

struct NeighborBounds final {
    MediaTime previousEnd;
    MediaTime nextStart;
};

class TimelineEditor final {
public:
    static constexpr qint64 kMinCueUs = 250'000;

    TimelineEditor(
        SubtitleDocument *document,
        SubtitleCommandManager *commands,
        TimelineViewport *viewport,
        SnapEngine *snap);

    void setInPoint(std::optional<MediaTime> inPoint);
    void setOutPoint(std::optional<MediaTime> outPoint);
    [[nodiscard]] std::optional<MediaTime> inPoint() const noexcept { return inPoint_; }
    [[nodiscard]] std::optional<MediaTime> outPoint() const noexcept { return outPoint_; }

    [[nodiscard]] NeighborBounds neighborBounds(const QString &id) const;
    [[nodiscard]] QList<Subtitle> visibleCues(double viewportWidth) const;
    [[nodiscard]] std::optional<Subtitle> previewCue() const;
    [[nodiscard]] bool isDragging() const noexcept { return drag_.has_value(); }

    [[nodiscard]] bool beginDrag(const QString &id, CueDragMode mode);
    [[nodiscard]] bool updateDrag(MediaTime delta);
    [[nodiscard]] bool updateDragByPixels(double deltaX);
    [[nodiscard]] bool endDrag();
    void cancelDrag();

    [[nodiscard]] bool setTiming(const QString &id, MediaTime start, MediaTime end);
    [[nodiscard]] bool splitAtPlayhead(const QString &id);
    [[nodiscard]] bool joinAroundPlayhead();

private:
    struct DragState final {
        QString id;
        CueDragMode mode = CueDragMode::Move;
        MediaTime originalStart;
        MediaTime originalEnd;
        MediaTime previewStart;
        MediaTime previewEnd;
    };

    [[nodiscard]] MediaTime snapTime(MediaTime value, const QString &excludeId) const;

    SubtitleDocument *document_;
    SubtitleCommandManager *commands_;
    TimelineViewport *viewport_;
    SnapEngine *snap_;
    std::optional<MediaTime> inPoint_;
    std::optional<MediaTime> outPoint_;
    std::optional<DragState> drag_;
};

} // namespace subcue
