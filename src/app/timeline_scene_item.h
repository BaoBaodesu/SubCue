#pragma once

#include "subtitle/subtitle.h"
#include "timeline/timeline_scene.h"
#include "timeline/timeline_viewport.h"
#include "ui_theme.h"
#include "waveform/waveform_pyramid.h"

#include <QtGui/QColor>
#include <QtCore/QElapsedTimer>
#include <QtGui/QFont>
#include <QtGui/QHoverEvent>
#include <QtGui/QTextLayout>
#include <QtQml/qqmlregistration.h>
#include <QtQuick/QQuickItem>

#include <memory>
#include <optional>
#include <vector>

class QSGTextNode;

namespace subcue {

class TimelineSceneItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    QML_NAMED_ELEMENT(TimelineSceneItem)
    Q_PROPERTY(qint64 durationUs READ durationUs WRITE setDurationUs NOTIFY durationUsChanged)
    Q_PROPERTY(qint64 playheadUs READ playheadUs WRITE setPlayheadUs NOTIFY playheadUsChanged)
    Q_PROPERTY(qint64 inPointUs READ inPointUs WRITE setInPointUs NOTIFY rangeChanged)
    Q_PROPERTY(qint64 outPointUs READ outPointUs WRITE setOutPointUs NOTIFY rangeChanged)
    Q_PROPERTY(double pixelsPerMs READ pixelsPerMs WRITE setPixelsPerMs NOTIFY viewChanged)
    Q_PROPERTY(int zoomPercent READ zoomPercent NOTIFY viewChanged)
    Q_PROPERTY(double scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY viewChanged)
    Q_PROPERTY(QString selectedCueId READ selectedCueId WRITE setSelectedCueId NOTIFY selectedCueIdChanged)
    Q_PROPERTY(int followDirection MEMBER followDirection_)
    Q_PROPERTY(bool viewportInteracting MEMBER viewportInteracting_)

public:
    explicit TimelineSceneItem(QQuickItem *parent = nullptr);

    [[nodiscard]] qint64 durationUs() const noexcept;
    void setDurationUs(qint64 value);

    [[nodiscard]] qint64 playheadUs() const noexcept;
    void setPlayheadUs(qint64 value);

    [[nodiscard]] qint64 inPointUs() const noexcept;
    void setInPointUs(qint64 value);

    [[nodiscard]] qint64 outPointUs() const noexcept;
    void setOutPointUs(qint64 value);

    [[nodiscard]] double pixelsPerMs() const noexcept;
    void setPixelsPerMs(double value);

    [[nodiscard]] int zoomPercent() const noexcept;
    [[nodiscard]] double scrollOffset() const noexcept;
    void setScrollOffset(double value);
    void setView(double pixelsPerMs, double scrollOffset);
    Q_INVOKABLE void setVisibleRange(double startRatio, double endRatio);

    [[nodiscard]] QString selectedCueId() const;
    void setSelectedCueId(const QString &id);

    Q_INVOKABLE void addCue(const QString &id, qint64 startUs, qint64 endUs, const QString &text);
    Q_INVOKABLE void clearCues();
    Q_INVOKABLE void fit();
    Q_INVOKABLE int visibleCueCount() const;

    void setWaveform(std::shared_ptr<const WaveformPyramid> waveform);
    void setSubtitles(QList<Subtitle> subtitles);
    void setPreviewCue(std::optional<Subtitle> cue);

signals:
    void durationUsChanged();
    void playheadUsChanged();
    void rangeChanged();
    void viewChanged();
    void selectedCueIdChanged();
    void userSeeked(qint64 playheadUs);
    void cueDragStarted(const QString &id, int mode);
    void cueDragUpdated(double deltaX);
    void cueDragFinished(bool canceled);
    void cueEditRequested(const QString &id);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct RulerLabel {
        qint64 ms = 0;
        std::unique_ptr<QTextLayout> layout;
    };

    struct RulerLabelCache {
        qint64 intervalMs = -1;
        double pixelsPerMs = 0.0;
        double viewWidth = -1.0;
        double viewHeight = -1.0;
        qreal dpr = 0.0;
        QString fontKey;
        QVector<qint64> tickMs;
        std::vector<RulerLabel> labels;
    };

    [[nodiscard]] TimelineSceneMetrics metrics() const;
    [[nodiscard]] TimelineSceneLayout currentLayout() const;
    void refresh();
    void rebuildCueIndex();
    void updateHoveredCue(double x, double y);
    [[nodiscard]] QFont rulerFont() const;
    [[nodiscard]] bool rulerLabelsNeedRebuild() const;
    void rebuildRulerLabelLayouts();
    void syncRulerLabelNode(QSGTextNode *node);

    TimelineViewport viewport_;
    bool draggingCue_ = false;
    int followDirection_ = 0;
    bool viewportInteracting_ = false;
    QElapsedTimer wheelInteraction_;
    double dragStartX_ = 0.0;
    QList<Subtitle> subtitles_;
    QVector<int> cueOrder_;
    QVector<qint64> cueEndIndex_;
    std::optional<Subtitle> previewCue_;
    std::shared_ptr<const WaveformPyramid> waveform_;
    QString selectedCueId_;
    QString hoveredCueId_;
    std::optional<MediaTime> inPoint_;
    std::optional<MediaTime> outPoint_;
    RulerLabelCache rulerLabels_;
    QColor backgroundColor_{UiTheme::kTimelineBackground};
    QColor rulerColor_{UiTheme::kRuler};
    QColor subtitleTrackColor_{UiTheme::kSubtitleTrack};
    QColor audioTrackColor_{UiTheme::kAudioTrack};
    QColor audioClipColor_{UiTheme::kAudioClip};
    QColor gridColor_{UiTheme::kTimelineGrid};
    QColor majorGridColor_{UiTheme::kTimelineMajorGrid};
    QColor trackDividerColor_{UiTheme::kTrackDivider};
    QColor cueColor_{UiTheme::kCue};
    QColor cueHoverColor_{UiTheme::kCueHover};
    QColor cueSelectedColor_{UiTheme::kCueSelected};
    QColor trimHandleColor_{UiTheme::kTrimHandle};
    QColor waveformColor_{UiTheme::kWaveform};
    QColor playheadColor_{UiTheme::kPlayhead};
    QColor rangeColor_{UiTheme::kRange};
    QColor timeTextColor_{UiTheme::kTimeText};
};

} // namespace subcue
