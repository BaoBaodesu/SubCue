#include "timeline_scene_item.h"

#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QFontMetricsF>
#include <QtGui/QStyleHints>
#include <queue>
#include <QtGui/QGuiApplication>
#include <QtGui/QHoverEvent>
#include <QtGui/QMatrix4x4>
#include <QtGui/QMouseEvent>
#include <QtGui/QTextLine>
#include <QtGui/QTextOption>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGFlatColorMaterial>
#include <QtQuick/QSGGeometry>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>
#include <QtQuick/QSGSimpleRectNode>
#include <QtQuick/QSGTextNode>

#include <algorithm>
#include <cmath>

namespace subcue {
namespace {

class TimelineRenderNode final : public QSGNode {
public:
    QVector<TimelineCueVisual> cues;
    QFont font;
    qreal devicePixelRatio = 0;
    bool hasPlayhead = false;
    QSGSimpleRectNode *playheadLine = nullptr;
    QSGSimpleRectNode *playheadHead = nullptr;
    QSGNode *selection = nullptr;
    QHash<QString, QSGSimpleRectNode *> bodies;
    QHash<QString, QPair<QSGSimpleRectNode *, QSGSimpleRectNode *>> handles;
    TimelineSceneLayout layout;
};

QSGSimpleRectNode *makeRect(const QRectF &rect, const QColor &color)
{
    return new QSGSimpleRectNode(rect, color);
}

QSGGeometryNode *makeLines(const QVector<QPointF> &segments, const QColor &color)
{
    auto *node = new QSGGeometryNode;
    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), segments.size());
    geometry->setDrawingMode(QSGGeometry::DrawLines);
    geometry->setLineWidth(1);
    QSGGeometry::Point2D *vertices = geometry->vertexDataAsPoint2D();
    for (int index = 0; index < segments.size(); ++index) {
        vertices[index].set(static_cast<float>(segments.at(index).x()),
                            static_cast<float>(segments.at(index).y()));
    }
    auto *material = new QSGFlatColorMaterial;
    material->setColor(color);
    node->setGeometry(geometry);
    node->setMaterial(material);
    node->setFlags(QSGNode::OwnsGeometry | QSGNode::OwnsMaterial);
    return node;
}

QRectF toRect(const TimelineSceneRect &rect)
{
    return QRectF(rect.x, rect.y, rect.width, rect.height);
}

void clearChildren(QSGNode *node)
{
    if (node == nullptr) {
        return;
    }
    while (QSGNode *child = node->firstChild()) {
        node->removeChildNode(child);
        delete child;
    }
}

} // namespace

TimelineSceneItem::TimelineSceneItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setClip(true);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win) {
        if (win) connect(win, &QWindow::activeChanged, this, [this, win] { if (!win->isActive()) cancelCueDrag(); });
    });
    viewport_.setDuration(MediaTime::fromMilliseconds(TimelineViewport::kEmptyTimelineMs));
    dragScrollTimer_.setInterval(16);
    connect(&dragScrollTimer_, &QTimer::timeout, this, [this] {
        if (!draggingCue_) { dragScrollTimer_.stop(); return; }
        const double delta = lastDragX_ < 24 ? -8.0 : lastDragX_ > width() - 24 ? 8.0 : 0.0;
        if (delta == 0.0) return;
        viewport_.scrollBy(delta, width());
        emit viewChanged();
        emit cueDragUpdated(lastDragX_ - dragStartX_ + viewport_.scrollOffset() - dragScrollStart_);
        refresh();
    });
}

void TimelineSceneItem::setEditable(bool value)
{
    if (editable_ == value) return;
    if (!value) cancelCueDrag();
    editable_ = value;
    emit editableChanged();
    refreshAppearance();
}

double TimelineSceneItem::subtitleViewportHeight() const
{
    return metrics().subtitleTrackHeight;
}

void TimelineSceneItem::setSubtitleScrollOffset(double value)
{
    value = std::clamp(value, 0.0, std::max(0.0, subtitleContentHeight() - subtitleViewportHeight()));
    if (qFuzzyCompare(value + 1.0, subtitleScrollOffset_ + 1.0)) return;
    subtitleScrollOffset_ = value;
    emit viewChanged();
    refresh();
}

QVariantMap TimelineSceneItem::hoveredCue() const
{
    const int row = cueRows_.value(hoveredCueId_, -1);
    if (row >= 0) {
        const Subtitle &cue = subtitles_.at(row);
        return {{QStringLiteral("text"), cue.text}, {QStringLiteral("startUs"), cue.start.microseconds()},
            {QStringLiteral("endUs"), cue.end.microseconds()},
            {QStringLiteral("review"), cue.status == QStringLiteral("REVIEW") || cue.status == QStringLiteral("LOW_CONFIDENCE")
                || cue.metadata.value(QStringLiteral("review")).toBool()},
            {QStringLiteral("overlap"), overlappingIds_.contains(cue.id)}};
    }
    return {};
}

QVariantMap TimelineSceneItem::dragPreview() const
{
    if (!previewCue_) return {};
    const Subtitle &cue = *previewCue_;
    const int row = cueRows_.value(cue.id, -1);
    QString target;
    bool overlap = false;
    if (cue.start == viewport_.playhead() || cue.end == viewport_.playhead()) target = QStringLiteral("播放头");
    if (inPoint_ && (cue.start == *inPoint_ || cue.end == *inPoint_)) target = QStringLiteral("入点");
    if (outPoint_ && (cue.start == *outPoint_ || cue.end == *outPoint_)) target = QStringLiteral("出点");
    for (const Subtitle &other : subtitles_) {
        if (other.id == cue.id || !other.isTimed()) continue;
        if (other.start < cue.end && cue.start < other.end) overlap = true;
        if (cue.start == other.start || cue.start == other.end || cue.end == other.start || cue.end == other.end)
            target = QStringLiteral("字幕边界");
    }
    return {{QStringLiteral("startUs"), cue.start.microseconds()}, {QStringLiteral("endUs"), cue.end.microseconds()},
        {QStringLiteral("deltaUs"), row >= 0 ? cue.start.microseconds() - subtitles_.at(row).start.microseconds() : qint64{0}},
        {QStringLiteral("overlap"), overlap}, {QStringLiteral("target"), target}};
}

QVariantMap TimelineSceneItem::renderStats() const
{
    return {{QStringLiteral("geometry"), geometryBuildCount_.load()},
        {QStringLiteral("text"), textBuildCount_.load()}, {QStringLiteral("waveform"), waveformBuildCount_.load()}};
}

void TimelineSceneItem::refreshAppearance()
{
    dirty_ |= PaintAppearance | PaintPlayhead;
    update();
}

qint64 TimelineSceneItem::durationUs() const noexcept
{
    return viewport_.duration().microseconds();
}

void TimelineSceneItem::setDurationUs(qint64 value)
{
    const MediaTime duration = MediaTime::fromMicroseconds(std::max<qint64>(0, value));
    if (viewport_.duration() == duration) {
        return;
    }
    viewport_.setDuration(duration);
    viewport_.setHasMedia(duration.microseconds() > 0);
    emit durationUsChanged();
    refresh();
}

qint64 TimelineSceneItem::playheadUs() const noexcept
{
    return viewport_.playhead().microseconds();
}

void TimelineSceneItem::setPlayheadUs(qint64 value)
{
    const MediaTime playhead = MediaTime::fromMicroseconds(std::max<qint64>(0, value));
    if (viewport_.playhead() == playhead) {
        return;
    }
    viewport_.setPlayhead(playhead);
    if (!viewportInteracting_ && !draggingCue_
        && (!wheelInteraction_.isValid() || wheelInteraction_.elapsed() >= 180)
        && viewport_.followPlayback(followDirection_, width())) {
        emit viewChanged();
        refresh();
    } else {
        refreshPlayhead();
    }
    emit playheadUsChanged();
}

qint64 TimelineSceneItem::inPointUs() const noexcept
{
    return inPoint_ ? inPoint_->microseconds() : -1;
}

void TimelineSceneItem::setInPointUs(qint64 value)
{
    std::optional<MediaTime> next;
    if (value >= 0) {
        next = MediaTime::fromMicroseconds(value);
    }
    if (inPoint_ == next) {
        return;
    }
    inPoint_ = next;
    emit rangeChanged();
    refresh();
}

qint64 TimelineSceneItem::outPointUs() const noexcept
{
    return outPoint_ ? outPoint_->microseconds() : -1;
}

void TimelineSceneItem::setOutPointUs(qint64 value)
{
    std::optional<MediaTime> next;
    if (value >= 0) {
        next = MediaTime::fromMicroseconds(value);
    }
    if (outPoint_ == next) {
        return;
    }
    outPoint_ = next;
    emit rangeChanged();
    refresh();
}

double TimelineSceneItem::pixelsPerMs() const noexcept
{
    return viewport_.pixelsPerMs();
}

void TimelineSceneItem::setPixelsPerMs(double value)
{
    if (qFuzzyCompare(viewport_.pixelsPerMs(), value)) {
        return;
    }
    viewport_.setPixelsPerMs(value);
    emit viewChanged();
    refresh();
}

int TimelineSceneItem::zoomPercent() const noexcept
{
    return viewport_.zoomPercent();
}

double TimelineSceneItem::scrollOffset() const noexcept
{
    return viewport_.scrollOffset();
}

void TimelineSceneItem::setScrollOffset(double value)
{
    const double previous = viewport_.scrollOffset();
    viewport_.setScrollOffset(value, width());
    if (qFuzzyCompare(previous, viewport_.scrollOffset())) {
        return;
    }
    emit viewChanged();
    refresh();
}

void TimelineSceneItem::setFollowDirection(int value)
{
    value = std::clamp(value, -1, 1);
    if (followDirection_ == value) {
        return;
    }
    followDirection_ = value;
    emit followDirectionChanged();
    if (!viewportInteracting_ && !draggingCue_ && followDirection_ != 0
        && (!wheelInteraction_.isValid() || wheelInteraction_.elapsed() >= 180)
        && viewport_.followPlayback(followDirection_, width())) {
        emit viewChanged();
        refresh();
    }
}

void TimelineSceneItem::setView(double pixelsPerMs, double scrollOffset)
{
    if (qFuzzyCompare(viewport_.pixelsPerMs(), pixelsPerMs)
        && qFuzzyCompare(viewport_.scrollOffset() + 1.0, scrollOffset + 1.0)
        && viewport_.contentWidth() >= width()) {
        return;
    }
    viewport_.setPixelsPerMs(pixelsPerMs);
    if (width() > 0.0 && viewport_.contentWidth() < width()) {
        viewport_.fit(width());
    }
    viewport_.setScrollOffset(scrollOffset, width());
    emit viewChanged();
    refresh();
}

void TimelineSceneItem::adjustZoomPercent(int delta)
{
    viewport_.adjustZoomPercent(delta, width());
    emit viewChanged();
    refresh();
}

void TimelineSceneItem::setVisibleRange(double startRatio, double endRatio)
{
    viewport_.setVisibleRange(startRatio, endRatio, width());
    emit viewChanged();
    refresh();
}

void TimelineSceneItem::ensureTimeVisible(qint64 timeUs)
{
    const double previous = viewport_.scrollOffset();
    viewport_.ensureTimeVisible(MediaTime::fromMicroseconds(std::max<qint64>(0, timeUs)), width());
    if (!qFuzzyCompare(previous + 1.0, viewport_.scrollOffset() + 1.0)) emit viewChanged();
    refresh();
}

QString TimelineSceneItem::selectedCueId() const
{
    return selectedCueId_;
}

void TimelineSceneItem::setSelectedCueId(const QString &id)
{
    if (selectedCueId_ == id) {
        return;
    }
    if (cueLanes_.contains(id)) {
        const double top = cueLanes_.value(id) * 54.0;
        if (top < subtitleScrollOffset_) setSubtitleScrollOffset(top);
        else if (top + 54.0 > subtitleScrollOffset_ + subtitleViewportHeight())
            setSubtitleScrollOffset(top + 54.0 - subtitleViewportHeight());
    }
    selectedCueId_ = id;
    emit selectedCueIdChanged();
    refreshAppearance();
}

void TimelineSceneItem::addCue(const QString &id, qint64 startUs, qint64 endUs, const QString &text,
    qint64 sourceStartUs, qint64 sourceEndUs, bool review)
{
    Subtitle cue;
    cue.id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    cue.start = MediaTime::fromMicroseconds(startUs);
    cue.end = MediaTime::fromMicroseconds(endUs);
    cue.text = text;
    if (sourceEndUs > sourceStartUs && sourceStartUs >= 0) {
        cue.metadata.insert(QStringLiteral("sourceStartUs"), sourceStartUs);
        cue.metadata.insert(QStringLiteral("sourceEndUs"), sourceEndUs);
    }
    if (review) cue.metadata.insert(QStringLiteral("review"), true);
    cue.source = QStringLiteral("manual");
    cue.status = QStringLiteral("MANUAL");
    for (Subtitle &existing : subtitles_) {
        if (existing.id == cue.id) {
            existing = cue;
            rebuildCueIndex();
            refresh();
            return;
        }
    }
    subtitles_.push_back(std::move(cue));
    rebuildCueIndex();
    refresh();
}

void TimelineSceneItem::clearCues()
{
    if (subtitles_.isEmpty()) {
        return;
    }
    subtitles_.clear();
    previewCue_.reset();
    cueOrder_.clear();
    cueEndIndex_.clear();
    cueRows_.clear();
    cueLanes_.clear();
    laneCueOrder_.clear();
    overlappingIds_.clear();
    laneCount_ = 1;
    subtitleScrollOffset_ = 0;
    emit viewChanged();
    refresh();
}

void TimelineSceneItem::fit()
{
    viewport_.fit(width());
    emit viewChanged();
    refresh();
}

int TimelineSceneItem::visibleCueCount() const
{
    return currentLayout(false).cues.size();
}

void TimelineSceneItem::setWaveform(std::shared_ptr<const WaveformPyramid> waveform)
{
    waveform_ = std::move(waveform);
    refresh();
}

void TimelineSceneItem::setSubtitles(QList<Subtitle> subtitles)
{
    // 文档未变化时 QList 共享同一快照，播放/Seek 不必重建索引。
    if (subtitles_.constData() == subtitles.constData() && subtitles_.size() == subtitles.size()) return;
    subtitles_ = std::move(subtitles);
    previewCue_.reset();
    rebuildCueIndex();
    refresh();
}

void TimelineSceneItem::rebuildCueIndex()
{
    cueOrder_.clear();
    cueEndIndex_.clear();
    cueRows_.clear();
    for (int index = 0; index < subtitles_.size(); ++index) {
        cueRows_.insert(subtitles_.at(index).id, index);
        if (subtitles_.at(index).isTimed()) cueOrder_.append(index);
    }
    std::stable_sort(cueOrder_.begin(), cueOrder_.end(), [this](int left, int right) {
        return subtitles_.at(left).start < subtitles_.at(right).start;
    });
    for (int index : cueOrder_) {
        cueEndIndex_.append(std::max(cueEndIndex_.isEmpty() ? qint64(0) : cueEndIndex_.last(),
            subtitles_.at(index).end.microseconds()));
    }
    cueLanes_.clear();
    laneCueOrder_.clear();
    overlappingIds_.clear();
    laneCount_ = 1;
    using EndLane = std::pair<qint64, int>;
    std::priority_queue<EndLane, std::vector<EndLane>, std::greater<EndLane>> active;
    std::priority_queue<int, std::vector<int>, std::greater<int>> free;
    qint64 maximumEnd = -1;
    QString maximumId;
    int nextLane = 0;
    for (int index : cueOrder_) {
        const Subtitle &cue = subtitles_.at(index);
        while (!active.empty() && active.top().first <= cue.start.microseconds()) {
            free.push(active.top().second); active.pop();
        }
        const int lane = free.empty() ? nextLane++ : free.top();
        if (!free.empty()) free.pop();
        cueLanes_.insert(cue.id, lane);
        if (laneCueOrder_.size() <= lane) laneCueOrder_.resize(lane + 1);
        laneCueOrder_[lane].append(index);
        active.push({cue.end.microseconds(), lane});
        if (cue.start.microseconds() < maximumEnd) {
            overlappingIds_.insert(cue.id); overlappingIds_.insert(maximumId);
        }
        if (cue.end.microseconds() > maximumEnd) { maximumEnd = cue.end.microseconds(); maximumId = cue.id; }
    }
    laneCount_ = std::max(1, nextLane);
    emit hoveredCueChanged();
    subtitleScrollOffset_ = std::clamp(subtitleScrollOffset_, 0.0,
        std::max(0.0, subtitleContentHeight() - subtitleViewportHeight()));
    emit viewChanged();

}

void TimelineSceneItem::setPreviewCue(std::optional<Subtitle> cue)
{
    previewCue_ = std::move(cue);
    if (previewCue_) {
        // 只有候选范围实际占用同一行时才展开，普通移动不改变波形位置。
        int lane = 0;
        for (; lane < laneCueOrder_.size(); ++lane) {
            const auto &order = laneCueOrder_.at(lane);
            auto first = std::upper_bound(order.cbegin(), order.cend(), previewCue_->start,
                [this](MediaTime time, int index) { return time < subtitles_.at(index).end; });
            while (first != order.cend() && subtitles_.at(*first).id == previewCue_->id) ++first;
            if (first == order.cend() || subtitles_.at(*first).start >= previewCue_->end) break;
        }
        // 视口高度不足时保留原可见行与重叠标记，避免候选块被裁掉。
        if (lane * 54.0 >= subtitleScrollOffset_ + std::max(54.0, height() - metrics().rulerHeight - 90.0))
            lane = cueLanes_.value(previewCue_->id);
        previewCue_->metadata.insert(QStringLiteral("displayLane"), lane);
    }
    emit dragPreviewChanged();
    refresh();
}

QSGNode *TimelineSceneItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<TimelineRenderNode *>(oldNode);
    QSGNode *geometry = nullptr;
    QSGTextNode *labels = nullptr;
    if (root != nullptr) {
        geometry = root->firstChild();
        auto *maybeLabels = dynamic_cast<QSGTextNode *>(root->lastChild());
        if (maybeLabels != nullptr && maybeLabels != geometry) {
            labels = maybeLabels;
        } else {
            clearChildren(root);
            geometry = nullptr;
            root->hasPlayhead = false;
            root->playheadLine = nullptr;
            root->playheadHead = nullptr;
            root->selection = nullptr;
            root->bodies.clear();
            root->handles.clear();
        }
    } else {
        root = new TimelineRenderNode;
    }
    if (geometry == nullptr) {
        geometry = new QSGNode;
        root->prependChildNode(geometry);
    }
    if (labels == nullptr && window() != nullptr) {
        labels = window()->createTextNode();
        if (labels != nullptr) {
            labels->setFlag(QSGNode::OwnedByParent, true);
            root->appendChildNode(labels);
            rulerLabels_.intervalMs = -1;
        }
    }
    if (geometry->nextSibling() == labels && window()) {
        if (auto *cueLabels = window()->createTextNode()) {
            root->insertChildNodeAfter(cueLabels, geometry);
            root->cues.clear();
        }
    }

    const quint8 dirty = dirty_;
    dirty_ = PaintNone;
    const auto paintSelection = [&] {
        if (!root->selection) return;
        clearChildren(root->selection);
        for (const TimelineCueVisual &cue : root->layout.cues) {
            if (cue.id != selectedCueId_) continue;
            const double bottom = root->layout.audioTrack.y + root->layout.audioTrack.height;
            const double top = cue.rect.y;
            root->selection->appendChildNode(makeRect(QRectF(cue.rect.x, top, cue.rect.width, bottom - top), selectedCueRangeColor_));
            root->selection->appendChildNode(makeRect(QRectF(cue.rect.x, top, 1.0, bottom - top), selectedCueBoundaryColor_));
            root->selection->appendChildNode(makeRect(QRectF(cue.rect.x + cue.rect.width - 1.0, top, 1.0, bottom - top), selectedCueBoundaryColor_));
        }
    };
    if (!(dirty & PaintGeometry) && root->playheadLine && root->playheadHead) {
        if (dirty & PaintAppearance) {
            for (const TimelineCueVisual &cue : root->layout.cues) {
                const bool selected = cue.id == selectedCueId_;
                const bool hovered = cue.id == hoveredCueId_;
                if (auto *body = root->bodies.value(cue.id))
                    body->setColor(selected ? cueSelectedColor_ : hovered ? cueHoverColor_ : cueColor_);
                const auto handles = root->handles.value(cue.id);
                const bool show = editable_ && cue.rect.width >= 24.0 && (selected || hovered);
                if (handles.first) handles.first->setRect(show ? QRectF(cue.rect.x, cue.rect.y, 3.0, cue.rect.height) : QRectF());
                if (handles.second) handles.second->setRect(show ? QRectF(cue.rect.x + cue.rect.width - 3.0, cue.rect.y, 3.0, cue.rect.height) : QRectF());
            }
            paintSelection();
        }
        const double x = viewport_.xAtTime(viewport_.playhead());
        const bool visible = x >= -2.0 && x <= width() + 2.0;
        root->playheadLine->setRect(visible ? QRectF(x, 0.0, 2.0, height()) : QRectF());
        root->playheadHead->setRect(visible ? QRectF(x - 4.0, 0.0, 10.0, 7.0) : QRectF());
        return root;
    }

    clearChildren(geometry);
    root->selection = nullptr;
    root->bodies.clear();
    root->handles.clear();
    root->playheadLine = nullptr;
    root->playheadHead = nullptr;
    ++geometryBuildCount_;
    const TimelineSceneLayout layout = currentLayout();
    root->layout = layout;
    if (waveform_) ++waveformBuildCount_;
    geometry->appendChildNode(makeRect(toRect(layout.background), backgroundColor_));
    geometry->appendChildNode(makeRect(toRect(layout.ruler), rulerColor_));
    geometry->appendChildNode(makeRect(toRect(layout.subtitleTrack), subtitleTrackColor_));
    geometry->appendChildNode(makeRect(toRect(layout.audioTrack), audioTrackColor_));

    const double viewWidth = width();
    const double viewHeight = height();
    const qint64 majorStep = std::max<qint64>(1, viewport_.rulerIntervalMs());
    const qint64 minorStep = std::max<qint64>(1, majorStep / 5);
    const qint64 durationMs = viewport_.timelineDuration().milliseconds();
    const qint64 startMs = viewport_.visibleStart().milliseconds();
    qint64 firstMinor = static_cast<qint64>(std::floor(static_cast<double>(startMs) / static_cast<double>(minorStep)))
        * minorStep;
    if (firstMinor < 0) {
        firstMinor = 0;
    }
    QVector<QPointF> minorGrid;
    QVector<QPointF> majorGrid;
    for (qint64 ms = firstMinor; ms <= durationMs; ms += minorStep) {
        const double x = viewport_.xAtTime(MediaTime::fromMilliseconds(ms));
        if (x > viewWidth) {
            break;
        }
        if (x < 0.0) {
            continue;
        }
        QVector<QPointF> &target = (ms % majorStep == 0) ? majorGrid : minorGrid;
        target.push_back(QPointF(x, 0.0));
        target.push_back(QPointF(x, viewHeight));
    }
    if (!minorGrid.isEmpty()) {
        geometry->appendChildNode(makeLines(minorGrid, gridColor_));
    }
    if (!majorGrid.isEmpty()) {
        geometry->appendChildNode(makeLines(majorGrid, majorGridColor_));
    }

    if (TimelineSceneBuilder::shouldDrawAudioClip(layout)) {
        const double clipLeft = std::max(0.0, viewport_.xAtTime(MediaTime::fromMicroseconds(0)));
        const double clipRight = std::min(viewWidth, viewport_.xAtTime(viewport_.duration()));
        if (clipRight > clipLeft) {
            const double pad = 4.0;
            const QRectF clipRect(
                clipLeft,
                layout.audioTrack.y + pad,
                clipRight - clipLeft,
                std::max(1.0, layout.audioTrack.height - pad * 2.0));
            geometry->appendChildNode(makeRect(clipRect, audioClipColor_));
        }
    }

    if (layout.inOutRange.width > 0.0) {
        geometry->appendChildNode(makeRect(toRect(layout.inOutRange), rangeColor_));
    }

    root->selection = new QSGNode;
    geometry->appendChildNode(root->selection);
    paintSelection();

    if (layout.subtitleTrack.height > 0.0) {
        geometry->appendChildNode(
            makeRect(QRectF(0.0, layout.subtitleTrack.y, viewWidth, 1.0), trackDividerColor_));
    }
    if (layout.audioTrack.height > 0.0) {
        geometry->appendChildNode(
            makeRect(QRectF(0.0, layout.audioTrack.y, viewWidth, 1.0), trackDividerColor_));
    }

    if (TimelineSceneBuilder::shouldDrawWaveform(layout)) {
        QVector<QPointF> wave;
        wave.reserve(layout.waveform.size() * 2);
        const double pad = 4.0;
        const double middle = layout.audioTrack.y + layout.audioTrack.height * 0.5;
        const double amplitude = std::max(1.0, layout.audioTrack.height * 0.5 - pad) * 0.88;
        for (int column = 0; column < layout.waveform.size(); ++column) {
            const WaveformPeak &peak = layout.waveform.at(column);
            const double x = static_cast<double>(column);
            wave.push_back(QPointF(x, middle - peak.max * amplitude));
            wave.push_back(QPointF(x, middle - peak.min * amplitude));
        }
        geometry->appendChildNode(makeLines(wave, waveformColor_));
    }

    for (const TimelineCueVisual &cue : layout.cues) {
        const bool hovered = !cue.selected && cue.id == hoveredCueId_;
        const QColor fill = cue.selected ? cueSelectedColor_ : (hovered ? cueHoverColor_ : cueColor_);
        auto *body = makeRect(toRect(cue.rect), fill);
        geometry->appendChildNode(body);
        root->bodies.insert(cue.id, body);
        if (cue.pending) geometry->appendChildNode(makeRect(QRectF(cue.rect.x, cue.rect.y, cue.rect.width, 2), UiTheme::kWarning));
        if (cue.overlapping) geometry->appendChildNode(makeRect(QRectF(cue.rect.x, cue.rect.y + 3, 3, cue.rect.height - 3), UiTheme::kWarning));
        const bool handlesVisible = editable_ && cue.rect.width >= 24.0 && (cue.selected || hovered);
        auto *left = makeRect(handlesVisible ? QRectF(cue.rect.x, cue.rect.y, 3, cue.rect.height) : QRectF(), trimHandleColor_);
        auto *right = makeRect(handlesVisible ? QRectF(cue.rect.x + cue.rect.width - 3, cue.rect.y, 3, cue.rect.height) : QRectF(), trimHandleColor_);
        geometry->appendChildNode(left); geometry->appendChildNode(right);
        root->handles.insert(cue.id, {left, right});
    }

    for (const TimelineCueVisual &cue : layout.cues) {
        if (!cue.audioBlock || layout.audioTrack.height <= 4.0) continue;
        const double left = std::max(0.0, cue.rect.x);
        const double right = std::min(viewWidth, cue.rect.x + cue.rect.width);
        if (right - left < 1.0) continue;
        const QRectF block(left, layout.audioTrack.y + 3.0, right - left,
            std::max(1.0, layout.audioTrack.height - 6.0));
        geometry->appendChildNode(makeRect(block, cue.selected ? cueSelectedColor_ : audioClipColor_));
        if (cue.pending) {
            geometry->appendChildNode(makeRect(QRectF(block.x(), block.y(), block.width(), 3.0), UiTheme::kWarning));
        }
        if (cue.waveform.isEmpty()) continue;
        QVector<QPointF> wave;
        wave.reserve(cue.waveform.size() * 2);
        const double middle = block.center().y();
        const double amplitude = std::max(1.0, block.height() * 0.5 - 2.0) * 0.9;
        const double span = std::max(1.0, cue.waveformWidth);
        for (int column = 0; column < cue.waveform.size(); ++column) {
            const double x = cue.waveformX + (column + 0.5) * span / cue.waveform.size();
            if (x < left || x > right) continue;
            const WaveformPeak &peak = cue.waveform.at(column);
            wave.push_back(QPointF(x, middle - peak.max * amplitude));
            wave.push_back(QPointF(x, middle - peak.min * amplitude));
        }
        if (!wave.isEmpty()) geometry->appendChildNode(makeLines(wave, waveformColor_));
    }

    root->playheadLine = makeRect(layout.playheadVisible ? QRectF(layout.playheadX, 0.0, 2.0, viewHeight) : QRectF(), playheadColor_);
    root->playheadHead = makeRect(layout.playheadVisible ? QRectF(layout.playheadX - 4.0, 0.0, 10.0, 7.0) : QRectF(), playheadColor_);
    geometry->appendChildNode(root->playheadLine);
    geometry->appendChildNode(root->playheadHead);
    root->hasPlayhead = true;

    QVector<TimelineCueVisual> textCues = layout.cues;
    for (auto &cue : textCues) {
        cue.waveform.clear(); cue.selected = false; cue.audioBlock = false;
        cue.waveformX = 0; cue.waveformWidth = 0;
    }
    auto *cueLabels = dynamic_cast<QSGTextNode *>(geometry->nextSibling());
    if (cueLabels && cueLabels != labels && (root->cues != textCues || root->font != rulerFont() || (window() && root->devicePixelRatio != window()->devicePixelRatio()))) {
        ++textBuildCount_;
        cueLabels->clear();
        cueLabels->setColor(UiTheme::kPrimaryText);
        cueLabels->setRenderType(QSGTextNode::QtRendering);
        for (const TimelineCueVisual &cue : layout.cues) {
            const double left = std::max(0.0, cue.rect.x) + 5.0;
            const double available = std::min(width(), cue.rect.x + cue.rect.width) - left - 5.0;
            if (available < 12.0 || cue.rect.height < 20.0) continue;
            const QString label = (cue.overlapping ? QStringLiteral("⚠ ") : cue.pending ? QStringLiteral("? ") : QString()) + cue.text;
            QTextLayout text(available >= 160 ? label : QFontMetricsF(rulerFont()).elidedText(label, Qt::ElideRight, available), rulerFont());
            QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); text.setTextOption(option);
            text.beginLayout();
            for (int row = 0; row < (available >= 160 ? 2 : 1); ++row) {
                QTextLine line = text.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(available); line.setPosition(QPointF(0, row * 15.0));
            }
            text.endLayout();
            cueLabels->addTextLayout(QPointF(left, cue.rect.y + 6), &text);
        }
        root->cues = std::move(textCues);
        root->font = rulerFont();
        root->devicePixelRatio = window() ? window()->devicePixelRatio() : 1;
    }
    if (labels != nullptr) {
        if (rulerLabelsNeedRebuild()) {
            rebuildRulerLabelLayouts();
            syncRulerLabelNode(labels);
        }
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(-viewport_.scrollOffset()), 0.0f);
        labels->setMatrix(matrix);
    }
    return root;
}

void TimelineSceneItem::cancelCueDrag()
{
    pressedCueId_.clear();
    dragScrollTimer_.stop();
    if (draggingCue_) {
        draggingCue_ = false;
        emit cueDragFinished(true);
    }
}

void TimelineSceneItem::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && (!pressedCueId_.isEmpty() || draggingCue_)) {
        cancelCueDrag(); event->accept(); return;
    }
    QQuickItem::keyPressEvent(event);
}

void TimelineSceneItem::focusOutEvent(QFocusEvent *event)
{
    cancelCueDrag();
    QQuickItem::focusOutEvent(event);
}

void TimelineSceneItem::mousePressEvent(QMouseEvent *event)
{
    if (interactionGuard_.isCallable() && !interactionGuard_.call().toBool()) {
        event->accept(); return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    const TimelineHit hit = TimelineSceneBuilder::hitTest(currentLayout(false), event->position().x(), event->position().y(), metrics());
    if (!hit.cueId.isEmpty()) {
        setSelectedCueId(hit.cueId);
        if (editable_) {
            pressedCueId_ = hit.cueId;
            pressedMode_ = hit.kind == TimelineHitKind::CueTrimStart ? 1 : hit.kind == TimelineHitKind::CueTrimEnd ? 2 : 0;
            dragStartX_ = event->position().x();
            dragScrollStart_ = viewport_.scrollOffset();
        } else {
            viewport_.setPlayhead(viewport_.timeAtX(event->position().x()));
            emit playheadUsChanged();
            emit userSeeked(viewport_.playhead().microseconds());
            refreshPlayhead();
        }
        event->accept(); return;
    }
    viewport_.setPlayhead(viewport_.timeAtX(event->position().x()));
    emit playheadUsChanged();
    emit userSeeked(viewport_.playhead().microseconds());
    event->accept(); refreshPlayhead();
}

void TimelineSceneItem::mouseMoveEvent(QMouseEvent *event)
{
    lastDragX_ = event->position().x();
    if (!pressedCueId_.isEmpty() && !draggingCue_ &&
        std::abs(lastDragX_ - dragStartX_) >= QGuiApplication::styleHints()->startDragDistance()) {
        draggingCue_ = true;
        emit cueDragStarted(pressedCueId_, pressedMode_);
        dragScrollTimer_.start();
    }
    if (draggingCue_) {
        emit cueDragUpdated(lastDragX_ - dragStartX_ + viewport_.scrollOffset() - dragScrollStart_);
        event->accept(); return;
    }
    if (!pressedCueId_.isEmpty() || !(event->buttons() & Qt::LeftButton)) return;
    viewport_.setPlayhead(viewport_.timeAtX(event->position().x()));
    emit playheadUsChanged();
    emit userSeeked(viewport_.playhead().microseconds());
    event->accept(); refreshPlayhead();
}

void TimelineSceneItem::mouseReleaseEvent(QMouseEvent *event)
{
    dragScrollTimer_.stop();
    if (draggingCue_) {
        emit cueDragUpdated(event->position().x() - dragStartX_ + viewport_.scrollOffset() - dragScrollStart_);
        draggingCue_ = false;
        emit cueDragFinished(false);
    }
    pressedCueId_.clear(); event->accept();
}

void TimelineSceneItem::mouseUngrabEvent()
{
    cancelCueDrag();
}

void TimelineSceneItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    mouseUngrabEvent();
    const TimelineHit hit = TimelineSceneBuilder::hitTest(currentLayout(false),
        event->position().x(), event->position().y(), metrics());
    if (!hit.cueId.isEmpty()) emit cueEditRequested(hit.cueId);
    event->accept();
}

void TimelineSceneItem::hoverMoveEvent(QHoverEvent *event)
{
    updateHoveredCue(event->position().x(), event->position().y());
    QQuickItem::hoverMoveEvent(event);
}

void TimelineSceneItem::hoverLeaveEvent(QHoverEvent *event)
{
    if (!hoveredCueId_.isEmpty()) {
        hoveredCueId_.clear();
        emit hoveredCueChanged();
        refreshAppearance();
    }
    QQuickItem::hoverLeaveEvent(event);
}

void TimelineSceneItem::wheelEvent(QWheelEvent *event)
{
    // 普通鼠标滚轮没有结束事件，短暂空闲后恢复跟随，避免连续滚动被播放时钟抢回。
    wheelInteraction_.start();
    const double viewWidth = std::max(1.0, width());
    if ((event->modifiers() & Qt::ShiftModifier) && subtitleContentHeight() > subtitleViewportHeight()) {
        setSubtitleScrollOffset(subtitleScrollOffset_ - event->angleDelta().y() * 0.5);
    } else if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
        viewport_.adjustZoomPercent(event->angleDelta().y() > 0 ? 10 : -10, viewWidth);
    } else {
        viewport_.scrollBy(-event->angleDelta().y() * 0.75, viewWidth);
    }
    emit viewChanged();
    event->accept();
    refresh();
}

void TimelineSceneItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        if (viewport_.contentWidth() < newGeometry.width()) {
            viewport_.fit(newGeometry.width());
        }
        viewport_.setScrollOffset(viewport_.scrollOffset(), newGeometry.width());
        setSubtitleScrollOffset(subtitleScrollOffset_);
        emit viewChanged();
        refresh();
    }
}

void TimelineSceneItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
        rulerLabels_.intervalMs = -1;
        refresh();
    }
}

TimelineSceneMetrics TimelineSceneItem::metrics() const
{
    TimelineSceneMetrics result;
    result.viewportWidth = width();
    result.viewportHeight = height();
    result.editable = editable_;
    result.subtitleTrackHeight = std::min(std::max(subtitleContentHeight(), previewCue_ ? (previewCue_->metadata.value(QStringLiteral("displayLane")).toInt() + 1) * 54.0 : 0.0),
        std::max(54.0, height() - result.rulerHeight - 90.0));
    result.subtitleScrollOffset = subtitleScrollOffset_;
    return result;
}

TimelineSceneLayout TimelineSceneItem::currentLayout(bool sampleWaveform) const
{
    QList<Subtitle> visible;
    // 每个派生行内字幕互不重叠，起止均有序；仅查询当前可见行和时间范围。
    const int firstLane = std::max(0, static_cast<int>(subtitleScrollOffset_ / 54.0));
    const int lastLane = std::min(static_cast<int>(laneCueOrder_.size()), static_cast<int>(std::ceil((subtitleScrollOffset_ + subtitleViewportHeight()) / 54.0)));
    for (int lane = firstLane; lane < lastLane; ++lane) {
        const auto &order = laneCueOrder_.at(lane);
        const auto first = std::upper_bound(order.cbegin(), order.cend(), viewport_.visibleStart(),
            [this](MediaTime time, int index) { return time < subtitles_.at(index).end; });
        const auto last = std::upper_bound(first, order.cend(), viewport_.visibleEnd(width()),
            [this](MediaTime time, int index) { return time < subtitles_.at(index).start; });
        for (auto index = first; index != last; ++index) {
            if (previewCue_ && subtitles_.at(*index).id == previewCue_->id) continue;
            Subtitle cue = subtitles_.at(*index);
            cue.metadata.insert(QStringLiteral("displayLane"), lane);
            cue.metadata.insert(QStringLiteral("displayOverlap"), overlappingIds_.contains(cue.id));
            visible.append(std::move(cue));
        }
    }
    if (previewCue_) {
        Subtitle cue = *previewCue_;
        int lane = cue.metadata.value(QStringLiteral("displayLane")).toInt();
        bool overlap = false;

        for (const Subtitle &other : visible) {
            if (other.start < cue.end && other.end > cue.start) {
                overlap = true;
            }
        }
        // 按候选范围派生显示行，重叠时才分开显示。
        cue.metadata.insert(QStringLiteral("displayLane"), lane);
        cue.metadata.insert(QStringLiteral("displayOverlap"), overlap);
        visible.append(std::move(cue));
    }
    return TimelineSceneBuilder::build(
        viewport_, visible, sampleWaveform ? waveform_.get() : nullptr, metrics(), selectedCueId_, inPoint_, outPoint_);
}

void TimelineSceneItem::refresh()
{
    dirty_ |= PaintGeometry | PaintPlayhead;
    update();
}

void TimelineSceneItem::refreshPlayhead()
{
    dirty_ |= PaintPlayhead;
    update();
}

void TimelineSceneItem::updateHoveredCue(double x, double y)
{
    const TimelineHit hit = TimelineSceneBuilder::hitTest(currentLayout(false), x, y, metrics());
    setCursor(hit.kind == TimelineHitKind::CueTrimStart || hit.kind == TimelineHitKind::CueTrimEnd
        ? Qt::SizeHorCursor : hit.kind == TimelineHitKind::CueBody ? Qt::SizeAllCursor : Qt::ArrowCursor);
    QString hovered;
    if (hit.kind == TimelineHitKind::CueBody || hit.kind == TimelineHitKind::CueTrimStart
        || hit.kind == TimelineHitKind::CueTrimEnd) {
        hovered = hit.cueId;
    }
    if (hovered == hoveredCueId_) {
        return;
    }
    hoveredCueId_ = std::move(hovered);
    emit hoveredCueChanged();
    refreshAppearance();
}

QFont TimelineSceneItem::rulerFont() const
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    return font;
}

bool TimelineSceneItem::rulerLabelsNeedRebuild() const
{
    const qint64 interval = std::max<qint64>(1, viewport_.rulerIntervalMs());
    if (interval != rulerLabels_.intervalMs) {
        return true;
    }
    if (!qFuzzyCompare(viewport_.pixelsPerMs() + 1.0, rulerLabels_.pixelsPerMs + 1.0)) {
        return true;
    }
    if (!qFuzzyCompare(width() + 1.0, rulerLabels_.viewWidth + 1.0)
        || !qFuzzyCompare(height() + 1.0, rulerLabels_.viewHeight + 1.0)) {
        return true;
    }
    const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
    if (!qFuzzyCompare(dpr + 1.0, rulerLabels_.dpr + 1.0)) {
        return true;
    }
    if (rulerFont().toString() != rulerLabels_.fontKey) {
        return true;
    }
    return viewport_.rulerTickMs(width()) != rulerLabels_.tickMs;
}

void TimelineSceneItem::rebuildRulerLabelLayouts()
{
    const QFont font = rulerFont();
    const qint64 interval = std::max<qint64>(1, viewport_.rulerIntervalMs());
    const QVector<qint64> ticks = viewport_.rulerTickMs(width());
    const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;

    rulerLabels_.intervalMs = interval;
    rulerLabels_.pixelsPerMs = viewport_.pixelsPerMs();
    rulerLabels_.viewWidth = width();
    rulerLabels_.viewHeight = height();
    rulerLabels_.dpr = dpr;
    rulerLabels_.fontKey = font.toString();
    rulerLabels_.tickMs = ticks;
    rulerLabels_.labels.clear();
    rulerLabels_.labels.reserve(static_cast<size_t>(ticks.size()));

    for (qint64 ms : ticks) {
        RulerLabel label;
        label.ms = ms;
        auto layout = std::make_unique<QTextLayout>(TimelineViewport::formatRulerLabel(ms, interval), font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout->setTextOption(option);
        layout->setCacheEnabled(true);
        layout->beginLayout();
        QTextLine line = layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(256.0);
            line.setPosition(QPointF(0.0, 0.0));
        }
        layout->endLayout();
        label.layout = std::move(layout);
        rulerLabels_.labels.push_back(std::move(label));
    }
}

void TimelineSceneItem::syncRulerLabelNode(QSGTextNode *node)
{
    if (node == nullptr) {
        return;
    }
    node->clear();
    node->setColor(timeTextColor_);
    node->setRenderType(QSGTextNode::QtRendering);
    const TimelineSceneMetrics sceneMetrics = metrics();
    const QFontMetricsF fontMetrics(rulerFont());
    const double textY = std::max(1.0, (sceneMetrics.rulerHeight - fontMetrics.height()) * 0.5);
    node->setViewport(QRectF(viewport_.scrollOffset(), 0.0, std::max(1.0, width()), sceneMetrics.rulerHeight));
    for (const RulerLabel &label : rulerLabels_.labels) {
        if (label.layout == nullptr) {
            continue;
        }
        const double contentX = viewport_.xAtTime(MediaTime::fromMilliseconds(label.ms)) + viewport_.scrollOffset();
        node->addTextLayout(QPointF(contentX + 4.0, textY), label.layout.get());
    }
}

} // namespace subcue
