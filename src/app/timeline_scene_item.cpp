#include "timeline_scene_item.h"

#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QFontMetricsF>
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
    viewport_.setDuration(MediaTime::fromMilliseconds(TimelineViewport::kEmptyTimelineMs));
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
    }
    emit playheadUsChanged();
    refresh();
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

void TimelineSceneItem::setVisibleRange(double startRatio, double endRatio)
{
    viewport_.setVisibleRange(startRatio, endRatio, width());
    emit viewChanged();
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
    selectedCueId_ = id;
    emit selectedCueIdChanged();
    refresh();
}

void TimelineSceneItem::addCue(const QString &id, qint64 startUs, qint64 endUs, const QString &text)
{
    Subtitle cue;
    cue.id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    cue.start = MediaTime::fromMicroseconds(startUs);
    cue.end = MediaTime::fromMicroseconds(endUs);
    cue.text = text;
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
    return currentLayout().cues.size();
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
    for (int index = 0; index < subtitles_.size(); ++index) {
        if (subtitles_.at(index).isTimed()) cueOrder_.append(index);
    }
    std::stable_sort(cueOrder_.begin(), cueOrder_.end(), [this](int left, int right) {
        return subtitles_.at(left).start < subtitles_.at(right).start;
    });
    for (int index : cueOrder_) {
        cueEndIndex_.append(std::max(cueEndIndex_.isEmpty() ? qint64(0) : cueEndIndex_.last(),
            subtitles_.at(index).end.microseconds()));
    }
}

void TimelineSceneItem::setPreviewCue(std::optional<Subtitle> cue)
{
    previewCue_ = std::move(cue);
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

    clearChildren(geometry);

    const TimelineSceneLayout layout = currentLayout();
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
        geometry->appendChildNode(makeRect(toRect(cue.rect), fill));
        if (cue.pending) {
            geometry->appendChildNode(makeRect(QRectF(cue.rect.x, cue.rect.y, cue.rect.width, 2), UiTheme::kWarning));
        }
        if (cue.selected || hovered) {
            const double handleWidth = 3.0;
            geometry->appendChildNode(
                makeRect(QRectF(cue.rect.x, cue.rect.y, handleWidth, cue.rect.height), trimHandleColor_));
            geometry->appendChildNode(makeRect(
                QRectF(cue.rect.x + cue.rect.width - handleWidth, cue.rect.y, handleWidth, cue.rect.height),
                trimHandleColor_));
        }
    }

    if (layout.playheadVisible) {
        geometry->appendChildNode(makeRect(QRectF(layout.playheadX, 0.0, 2.0, viewHeight), playheadColor_));
        geometry->appendChildNode(makeRect(QRectF(layout.playheadX - 4.0, 0.0, 10.0, 7.0), playheadColor_));
    }

    auto *cueLabels = dynamic_cast<QSGTextNode *>(geometry->nextSibling());
    if (cueLabels && cueLabels != labels && (root->cues != layout.cues || root->font != rulerFont())) {
        cueLabels->clear();
        cueLabels->setColor(UiTheme::kPrimaryText);
        cueLabels->setRenderType(QSGTextNode::QtRendering);
        for (const TimelineCueVisual &cue : layout.cues) {
            const double left = std::max(0.0, cue.rect.x) + 5.0;
            const double available = std::min(width(), cue.rect.x + cue.rect.width) - left - 5.0;
            if (available < 12.0) continue;
            QTextLayout text(QFontMetricsF(rulerFont()).elidedText(cue.text, Qt::ElideRight, available), rulerFont());
            text.beginLayout();
            QTextLine line = text.createLine();
            if (line.isValid()) line.setLineWidth(available);
            text.endLayout();
            cueLabels->addTextLayout(QPointF(left, cue.rect.y + 6), &text);
        }
        root->cues = layout.cues;
        root->font = rulerFont();
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

void TimelineSceneItem::mousePressEvent(QMouseEvent *event)
{
    const TimelineSceneLayout layout = currentLayout();
    const TimelineHit hit = TimelineSceneBuilder::hitTest(layout, event->position().x(), event->position().y(), metrics());
    if (!hit.cueId.isEmpty()) {
        setSelectedCueId(hit.cueId);
        draggingCue_ = true;
        dragStartX_ = event->position().x();
        emit cueDragStarted(hit.cueId, hit.kind == TimelineHitKind::CueTrimStart ? 1
            : hit.kind == TimelineHitKind::CueTrimEnd ? 2 : 0);
        event->accept();
        return;
    }
    viewport_.setPlayhead(viewport_.timeAtX(event->position().x()));
    emit playheadUsChanged();
    emit userSeeked(viewport_.playhead().microseconds());
    event->accept();
    refresh();
}

void TimelineSceneItem::mouseMoveEvent(QMouseEvent *event)
{
    if (draggingCue_) {
        emit cueDragUpdated(event->position().x() - dragStartX_);
        event->accept();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
        return;
    }
    viewport_.setPlayhead(viewport_.timeAtX(event->position().x()));
    emit playheadUsChanged();
    emit userSeeked(viewport_.playhead().microseconds());
    event->accept();
    refresh();
}

void TimelineSceneItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (draggingCue_) {
        emit cueDragUpdated(event->position().x() - dragStartX_);
        draggingCue_ = false;
        emit cueDragFinished(false);
    }
    event->accept();
}

void TimelineSceneItem::mouseUngrabEvent()
{
    if (draggingCue_) {
        draggingCue_ = false;
        emit cueDragFinished(true);
    }
}

void TimelineSceneItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    mouseUngrabEvent();
    const TimelineHit hit = TimelineSceneBuilder::hitTest(currentLayout(),
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
        refresh();
    }
    QQuickItem::hoverLeaveEvent(event);
}

void TimelineSceneItem::wheelEvent(QWheelEvent *event)
{
    // 普通鼠标滚轮没有结束事件，短暂空闲后恢复跟随，避免连续滚动被播放时钟抢回。
    wheelInteraction_.start();
    const double viewWidth = std::max(1.0, width());
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) {
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
    return result;
}

TimelineSceneLayout TimelineSceneItem::currentLayout() const
{
    QList<Subtitle> visible;
    const auto first = std::lower_bound(cueEndIndex_.cbegin(), cueEndIndex_.cend(),
        viewport_.visibleStart().microseconds());
    const auto last = std::upper_bound(cueOrder_.cbegin(), cueOrder_.cend(),
        viewport_.visibleEnd(width()), [this](MediaTime time, int index) {
            return time < subtitles_.at(index).start;
        });
    for (qsizetype index = first - cueEndIndex_.cbegin(); index < last - cueOrder_.cbegin(); ++index) {
        if (previewCue_ && subtitles_.at(cueOrder_.at(index)).id == previewCue_->id) continue;
        visible.append(subtitles_.at(cueOrder_.at(index)));
    }
    if (previewCue_) visible.append(*previewCue_);
    return TimelineSceneBuilder::build(
        viewport_, visible, waveform_.get(), metrics(), selectedCueId_, inPoint_, outPoint_);
}

void TimelineSceneItem::refresh()
{
    update();
}

void TimelineSceneItem::updateHoveredCue(double x, double y)
{
    const TimelineHit hit = TimelineSceneBuilder::hitTest(currentLayout(), x, y, metrics());
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
    refresh();
}

QFont TimelineSceneItem::rulerFont() const
{
    QFont font = QGuiApplication::font();
    font.setPixelSize(11);
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
