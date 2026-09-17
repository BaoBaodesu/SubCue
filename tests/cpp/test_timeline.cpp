#include "subtitle/subtitle_command_manager.h"
#include "subtitle/subtitle_document.h"
#include "timeline/snap_engine.h"
#include "timeline/timeline_editor.h"
#include "timeline/timeline_scene.h"
#include "timeline/timeline_viewport.h"
#include "waveform/waveform_pyramid.h"

#include <QtTest/QTest>

#include <cmath>
#include <optional>
#include <vector>

using namespace subcue;

namespace {

Subtitle makeCue(const QString &id, qint64 startMs, qint64 endMs, const QString &text = QStringLiteral("cue"))
{
    Subtitle subtitle;
    subtitle.id = id;
    subtitle.text = text;
    subtitle.start = MediaTime::fromMilliseconds(startMs);
    subtitle.end = MediaTime::fromMilliseconds(endMs);
    subtitle.source = QStringLiteral("asr");
    subtitle.status = QStringLiteral("MATCHED");
    return subtitle;
}

} // namespace

class TimelineTests final : public QObject {
    Q_OBJECT

private slots:
    void playbackPageScroll();
    void viewportConvertsTimeAndPixel();
    void viewportZoomAnchorsPlayhead();
    void viewportZoomPercentIsClamped();
    void navigatorRangeKeepsPlayheadAndBounds();
    void viewportFitUsesMediaDuration();
    void viewportScrollIsClamped();
    void viewportVisibleRangeFollowsScroll();
    void snapDisabledReturnsClampedValue();
    void snapToFramePlayheadRangeAndCueEdges();
    void snapIgnoresExcludedCue();
    void snapThresholdScalesWithZoom();
    void visibleCuesCullOutsideViewport();
    void dragPreviewDoesNotMutateDocument();
    void dragMoveSnapsAndCommitsViaCommand();
    void dragMoveCanCrossAnotherCue();
    void dragTrimRespectsMinDurationAndNeighbors();
    void dragUndoRestoresOriginalTiming();
    void splitAtPlayheadAndUndo();
    void joinAroundPlayheadUsesSingleUndo();
    void sceneLayoutContainsOnlyVisibleCues();
    void sceneLayoutProjectsSelectedCueToAudioTrack();
    void sceneLayoutWaveformMatchesViewportWidth();
    void shortWaveformDoesNotStretchPastMedia();
    void sceneLayoutOmitsWaveformWithoutData();
    void rulerLabelFormatsMajorTicks();
    void sceneHitTestPrefersTrimHandles();
};

void TimelineTests::playbackPageScroll()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(300'000));
    viewport.setPixelsPerMs(0.1);
    viewport.setPlayhead(MediaTime::fromMilliseconds(8'999));
    QVERIFY(!viewport.followPlayback(1, 1000));
    viewport.setPlayhead(MediaTime::fromMilliseconds(9'000));
    QVERIFY(viewport.followPlayback(1, 1000));
    QCOMPARE(viewport.visibleStart().milliseconds(), qint64(8'000));
    QCOMPARE(viewport.playhead().milliseconds(), qint64(9'000));
    QVERIFY(!viewport.followPlayback(1, 1000));
    viewport.setPlayhead(MediaTime::fromMilliseconds(8'999));
    QVERIFY(viewport.followPlayback(-1, 1000));
    QCOMPARE(viewport.scrollOffset(), 0.0);
    viewport.setPlayhead(MediaTime::fromMilliseconds(300'000));
    QVERIFY(viewport.followPlayback(1, 1000));
    QCOMPARE(viewport.visibleEnd(1000).milliseconds(), qint64(300'000));
    QVERIFY(!viewport.followPlayback(1, 1000));
}

void TimelineTests::viewportConvertsTimeAndPixel()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setPixelsPerMs(0.2);
    viewport.setScrollOffset(100.0, 400.0);
    QCOMPARE(viewport.xAtTime(MediaTime::fromMilliseconds(1'000)), 100.0);
    QCOMPARE(viewport.timeAtX(100.0).milliseconds(), 1'000);
    QCOMPARE(viewport.timeAtX(0.0).milliseconds(), 500);
}

void TimelineTests::viewportZoomAnchorsPlayhead()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setHasMedia(true);
    viewport.setPixelsPerMs(0.1);
    viewport.setPlayhead(MediaTime::fromMilliseconds(2'000));
    viewport.setScrollOffset(50.0, 400.0);
    const double playheadX = viewport.xAtTime(viewport.playhead());
    viewport.zoomBy(2.0, 400.0);
    QCOMPARE(viewport.pixelsPerMs(), 0.2);
    QCOMPARE(viewport.xAtTime(viewport.playhead()), playheadX);
}

void TimelineTests::viewportZoomPercentIsClamped()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(1'000));
    viewport.setZoomPercent(5, 200.0);
    QCOMPARE(viewport.zoomPercent(), 200);
    viewport.setZoomPercent(50'000, 200.0);
    QCOMPARE(viewport.zoomPercent(), 10'000);
    viewport.setZoomPercent(100, 200.0);
    QCOMPARE(viewport.pixelsPerMs(), 0.2);
    QCOMPARE(viewport.zoomPercent(), 200);
}

void TimelineTests::navigatorRangeKeepsPlayheadAndBounds()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(100'000));
    viewport.setHasMedia(true);
    viewport.setPlayhead(MediaTime::fromMilliseconds(45'000));
    viewport.setVisibleRange(0.2, 0.6, 800);
    QCOMPARE(viewport.visibleStart().milliseconds(), 20'000);
    QCOMPARE(viewport.visibleEnd(800).milliseconds(), 60'000);
    QVERIFY(std::abs(viewport.pixelsPerMs() - 0.02) < 1e-9);
    viewport.setVisibleRange(0.3, 0.6, 800);
    QCOMPARE(viewport.visibleEnd(800).milliseconds(), 60'000);
    viewport.setVisibleRange(0.3, 0.8, 800);
    QCOMPARE(viewport.visibleStart().milliseconds(), 30'000);
    viewport.setVisibleRange(-1, 2, 800);
    QCOMPARE(viewport.scrollOffset(), 0.0);
    QCOMPARE(viewport.contentWidth(), 800.0);
    viewport.zoomBy(0.01, 800);
    QCOMPARE(viewport.contentWidth(), 800.0);
    QCOMPARE(viewport.playhead().milliseconds(), 45'000);
    viewport.setDuration(MediaTime::fromMilliseconds(10'000'000));
    viewport.fit(800);
    QCOMPARE(viewport.visibleEnd(800).milliseconds(), 10'000'000);
}

void TimelineTests::viewportFitUsesMediaDuration()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setHasMedia(true);
    viewport.setScrollOffset(80.0, 500.0);
    viewport.fit(500.0);
    QCOMPARE(viewport.pixelsPerMs(), 0.05);
    QCOMPARE(viewport.scrollOffset(), 0.0);

    TimelineViewport empty;
    empty.fit(500.0);
    QCOMPARE(empty.pixelsPerMs(), TimelineViewport::kDefaultPixelsPerMs);
    QCOMPARE(empty.timelineDuration().milliseconds(), 60'000);
}

void TimelineTests::viewportScrollIsClamped()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(1'000));
    viewport.setHasMedia(true);
    viewport.setPixelsPerMs(0.1);
    viewport.scrollBy(-50.0, 80.0);
    QCOMPARE(viewport.scrollOffset(), 0.0);
    viewport.scrollBy(10'000.0, 80.0);
    QCOMPARE(viewport.scrollOffset(), viewport.maxScrollOffset(80.0));
    QVERIFY(viewport.maxScrollOffset(80.0) > 0.0);
}

void TimelineTests::viewportVisibleRangeFollowsScroll()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setPixelsPerMs(0.1);
    viewport.setScrollOffset(50.0, 100.0);
    QCOMPARE(viewport.visibleStart().milliseconds(), 500);
    QCOMPARE(viewport.visibleEnd(100.0).milliseconds(), 1'500);
    QVERIFY(viewport.rangeOverlapsViewport(
        MediaTime::fromMilliseconds(1'200), MediaTime::fromMilliseconds(2'000), 100.0));
    QVERIFY(!viewport.rangeOverlapsViewport(
        MediaTime::fromMilliseconds(2'000), MediaTime::fromMilliseconds(3'000), 100.0));
}

void TimelineTests::snapDisabledReturnsClampedValue()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(1'000));
    SnapEngine snap;
    snap.setEnabled(false);
    const MediaTime result = snap.snap(
        MediaTime::fromMilliseconds(1'500),
        viewport,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        {});
    QCOMPARE(result.milliseconds(), 1'000);
}

void TimelineTests::snapToFramePlayheadRangeAndCueEdges()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(2'000));
    viewport.setPixelsPerMs(0.1);
    SnapEngine snap;
    snap.setFps(25.0);
    QCOMPARE(snap.frameDuration().microseconds(), 40'000);

    const QList<Subtitle> cues{makeCue(QStringLiteral("a"), 400, 700)};
    const MediaTime toCue = snap.snap(
        MediaTime::fromMilliseconds(408),
        viewport,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        cues);
    QCOMPARE(toCue.milliseconds(), 400);

    const MediaTime toPlayhead = snap.snap(
        MediaTime::fromMilliseconds(1'010),
        viewport,
        MediaTime::fromMilliseconds(1'000),
        std::nullopt,
        std::nullopt,
        {});
    QCOMPARE(toPlayhead.milliseconds(), 1'000);

    const MediaTime toIn = snap.snap(
        MediaTime::fromMilliseconds(250),
        viewport,
        MediaTime::fromMilliseconds(0),
        MediaTime::fromMilliseconds(240),
        MediaTime::fromMilliseconds(900),
        {});
    QCOMPARE(toIn.milliseconds(), 240);

    const MediaTime toFrame = snap.snap(
        MediaTime::fromMilliseconds(21),
        viewport,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        {});
    QCOMPARE(toFrame.milliseconds(), 40);
}

void TimelineTests::snapIgnoresExcludedCue()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(2'000));
    viewport.setPixelsPerMs(0.1);
    SnapEngine snap;
    snap.setFps(24.0);
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("self"), 400, 800),
        makeCue(QStringLiteral("other"), 1'200, 1'500),
    };
    const MediaTime withSelf = snap.snap(
        MediaTime::fromMilliseconds(408),
        viewport,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        cues);
    QCOMPARE(withSelf.milliseconds(), 400);
    const MediaTime withoutSelf = snap.snap(
        MediaTime::fromMilliseconds(408),
        viewport,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        cues,
        QStringLiteral("self"));
    QVERIFY(withoutSelf.milliseconds() != 400);
}

void TimelineTests::snapThresholdScalesWithZoom()
{
    TimelineViewport coarse;
    coarse.setPixelsPerMs(0.1);
    TimelineViewport fine;
    fine.setPixelsPerMs(1.0);
    SnapEngine snap;
    QCOMPARE(snap.thresholdUs(coarse), 80'000);
    QCOMPARE(snap.thresholdUs(fine), 8'000);

    coarse.setDuration(MediaTime::fromMilliseconds(2'000));
    const MediaTime far = snap.snap(
        MediaTime::fromMilliseconds(200),
        coarse,
        MediaTime::fromMilliseconds(0),
        std::nullopt,
        std::nullopt,
        QList<Subtitle>{makeCue(QStringLiteral("a"), 400, 500)});
    QCOMPARE(far.milliseconds(), 200);
}

void TimelineTests::visibleCuesCullOutsideViewport()
{
    SubtitleDocument document;
    document.setSubtitles({
        makeCue(QStringLiteral("left"), 0, 400),
        makeCue(QStringLiteral("visible"), 600, 900),
        makeCue(QStringLiteral("right"), 2'000, 2'400),
        makeCue(QStringLiteral("untimed"), 0, 0),
    });
    Subtitle untimed = document.subtitle(QStringLiteral("untimed")).value();
    untimed.status = QStringLiteral("SKIPPED_NO_AUDIO");
    QVERIFY(document.replaceSubtitle(QStringLiteral("untimed"), untimed));

    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPixelsPerMs(0.1);
    viewport.setScrollOffset(50.0, 100.0);
    SnapEngine snap;
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    const QList<Subtitle> visible = editor.visibleCues(100.0);
    QCOMPARE(visible.size(), 1);
    QCOMPARE(visible.front().id, QStringLiteral("visible"));
}

void TimelineTests::dragPreviewDoesNotMutateDocument()
{
    SubtitleDocument document;
    document.setSubtitles({makeCue(QStringLiteral("a"), 1'000, 2'000)});
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPixelsPerMs(0.1);
    SnapEngine snap;
    snap.setEnabled(false);
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(200)));
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 1'000);
    QCOMPARE(editor.previewCue()->start.milliseconds(), 1'200);
    QVERIFY(!commands.canUndo());
}

void TimelineTests::dragMoveSnapsAndCommitsViaCommand()
{
    SubtitleDocument document;
    document.setSubtitles({
        makeCue(QStringLiteral("a"), 400, 900),
        makeCue(QStringLiteral("b"), 1'200, 1'600),
    });
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPixelsPerMs(0.1);
    SnapEngine snap;
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(8)));
    QCOMPARE(editor.previewCue()->start.milliseconds(), 400);
    QVERIFY(editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 400);

    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(300)));
    QVERIFY(editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 700);
    QCOMPARE(document.subtitle(QStringLiteral("a"))->end.milliseconds(), 1'200);
    QCOMPARE(document.subtitle(QStringLiteral("a"))->source, QStringLiteral("manual"));
    QCOMPARE(document.subtitle(QStringLiteral("a"))->status, QStringLiteral("MANUAL"));
    QVERIFY(commands.canUndo());
}

void TimelineTests::dragMoveCanCrossAnotherCue()
{
    SubtitleDocument document;
    document.setSubtitles({
        makeCue(QStringLiteral("a"), 0, 500),
        makeCue(QStringLiteral("b"), 700, 1'200),
        makeCue(QStringLiteral("c"), 2'000, 2'500),
    });
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    SnapEngine snap;
    snap.setEnabled(false);
    TimelineEditor editor(&document, &commands, &viewport, &snap);

    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(1'400)));
    QCOMPARE(editor.previewCue()->start.milliseconds(), 1'400);
    QVERIFY(editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 1'400);

    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(-500)));
    QVERIFY(!editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 1'400);
}

void TimelineTests::dragTrimRespectsMinDurationAndNeighbors()
{
    SubtitleDocument document;
    document.setSubtitles({
        makeCue(QStringLiteral("a"), 0, 800),
        makeCue(QStringLiteral("b"), 1'000, 2'000),
        makeCue(QStringLiteral("c"), 2'200, 3'000),
    });
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPixelsPerMs(1.0);
    SnapEngine snap;
    snap.setEnabled(false);
    TimelineEditor editor(&document, &commands, &viewport, &snap);

    QVERIFY(editor.beginDrag(QStringLiteral("b"), CueDragMode::TrimStart));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(-500)));
    QCOMPARE(editor.previewCue()->start.milliseconds(), 800);
    QVERIFY(editor.endDrag());

    QVERIFY(editor.beginDrag(QStringLiteral("b"), CueDragMode::TrimEnd));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(5'000)));
    QCOMPARE(editor.previewCue()->end.milliseconds(), 2'200);
    QVERIFY(editor.endDrag());

    QVERIFY(editor.beginDrag(QStringLiteral("b"), CueDragMode::TrimEnd));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(-5'000)));
    QCOMPARE(editor.previewCue()->end.milliseconds() - editor.previewCue()->start.milliseconds(), 250);
    QVERIFY(editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("b"))->end.milliseconds()
                 - document.subtitle(QStringLiteral("b"))->start.milliseconds(),
             250);
}

void TimelineTests::dragUndoRestoresOriginalTiming()
{
    SubtitleDocument document;
    const Subtitle original = makeCue(QStringLiteral("a"), 500, 1'200);
    document.setSubtitles({original});
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    SnapEngine snap;
    snap.setEnabled(false);
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    QVERIFY(editor.beginDrag(QStringLiteral("a"), CueDragMode::Move));
    QVERIFY(editor.updateDrag(MediaTime::fromMilliseconds(100)));
    QVERIFY(editor.endDrag());
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 600);
    commands.undo();
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 500);
    QCOMPARE(document.subtitle(QStringLiteral("a"))->source, QStringLiteral("asr"));
    commands.redo();
    QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 600);
    QCOMPARE(document.subtitle(QStringLiteral("a"))->source, QStringLiteral("manual"));
}

void TimelineTests::splitAtPlayheadAndUndo()
{
    SubtitleDocument document;
    document.setSubtitles({makeCue(QStringLiteral("a"), 0, 2'000, QStringLiteral("hello"))});
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPlayhead(MediaTime::fromMilliseconds(800));
    SnapEngine snap;
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    QVERIFY(editor.splitAtPlayhead(QStringLiteral("a")));
    QCOMPARE(document.count(), 2);
    QCOMPARE(document.subtitles().at(0).end.milliseconds(), 800);
    QCOMPARE(document.subtitles().at(1).start.milliseconds(), 800);
    QCOMPARE(document.subtitles().at(0).text, QStringLiteral("hello"));
    QCOMPARE(document.subtitles().at(1).text, QString());
    commands.undo();
    QCOMPARE(document.count(), 1);
    QCOMPARE(document.subtitle(QStringLiteral("a"))->end.milliseconds(), 2'000);
}

void TimelineTests::joinAroundPlayheadUsesSingleUndo()
{
    SubtitleDocument document;
    document.setSubtitles({
        makeCue(QStringLiteral("left"), 0, 800),
        makeCue(QStringLiteral("right"), 1'200, 2'000),
    });
    SubtitleCommandManager commands(&document);
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPlayhead(MediaTime::fromMilliseconds(1'000));
    SnapEngine snap;
    TimelineEditor editor(&document, &commands, &viewport, &snap);
    QVERIFY(editor.joinAroundPlayhead());
    QCOMPARE(document.subtitle(QStringLiteral("left"))->end.milliseconds(), 1'000);
    QCOMPARE(document.subtitle(QStringLiteral("right"))->start.milliseconds(), 1'000);
    QCOMPARE(document.subtitle(QStringLiteral("left"))->source, QStringLiteral("manual"));
    commands.undo();
    QCOMPARE(document.subtitle(QStringLiteral("left"))->end.milliseconds(), 800);
    QCOMPARE(document.subtitle(QStringLiteral("right"))->start.milliseconds(), 1'200);
    QVERIFY(!commands.canUndo());
}

void TimelineTests::sceneLayoutContainsOnlyVisibleCues()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setPixelsPerMs(0.1);
    viewport.setScrollOffset(100.0, 200.0);
    viewport.setPlayhead(MediaTime::fromMilliseconds(1'500));
    const QList<Subtitle> cues{
        makeCue(QStringLiteral("hidden-left"), 0, 400),
        makeCue(QStringLiteral("visible"), 1'200, 1'800),
        makeCue(QStringLiteral("hidden-right"), 5'000, 5'400),
    };
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 200.0;
    metrics.viewportHeight = 160.0;
    const TimelineSceneLayout layout = TimelineSceneBuilder::build(
        viewport, cues, nullptr, metrics, QStringLiteral("visible"));
    QCOMPARE(layout.cues.size(), 1);
    QCOMPARE(layout.cues.front().id, QStringLiteral("visible"));
    QVERIFY(layout.cues.front().selected);
    QVERIFY(layout.playheadVisible);
    QVERIFY(!layout.ticks.isEmpty());
    QCOMPARE(layout.audioTrack.y, metrics.rulerHeight + metrics.subtitleTrackHeight);
}

void TimelineTests::sceneLayoutProjectsSelectedCueToAudioTrack()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(10'000));
    viewport.setPixelsPerMs(0.1);
    const QList<Subtitle> cues{makeCue(QStringLiteral("selected"), 1'200, 1'800)};
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 300.0;
    metrics.viewportHeight = 180.0;

    const TimelineSceneLayout layout = TimelineSceneBuilder::build(
        viewport, cues, nullptr, metrics, QStringLiteral("selected"));
    QCOMPARE(layout.cues.size(), 1);
    QCOMPARE(layout.selectedCueRange.x, layout.cues.front().rect.x);
    QCOMPARE(layout.selectedCueRange.width, layout.cues.front().rect.width);
    QCOMPARE(layout.selectedCueRange.y, layout.subtitleTrack.y);
    QCOMPARE(layout.selectedCueRange.height, layout.subtitleTrack.height + layout.audioTrack.height);
}

void TimelineTests::sceneLayoutWaveformMatchesViewportWidth()
{
    std::vector<float> samples(8'000, 0.0f);
    for (int index = 0; index < static_cast<int>(samples.size()); ++index) {
        samples[static_cast<size_t>(index)] = index % 2 == 0 ? 0.8f : -0.4f;
    }
    const WaveformPyramid pyramid = WaveformPyramid::fromMonoFloat(samples.data(), static_cast<qsizetype>(samples.size()), 8'000);
    TimelineViewport viewport;
    viewport.setDuration(pyramid.duration());
    viewport.setHasMedia(true);
    viewport.setPixelsPerMs(0.2);
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 120.0;
    metrics.viewportHeight = 180.0;
    const TimelineSceneLayout layout = TimelineSceneBuilder::build(viewport, {}, &pyramid, metrics);
    QCOMPARE(layout.waveform.size(), 120);
    QVERIFY(layout.waveform.front().max > 0.0f);
    QVERIFY(layout.waveform.front().min < 0.0f);
    QVERIFY(TimelineSceneBuilder::shouldDrawWaveform(layout));
    QVERIFY(TimelineSceneBuilder::shouldDrawAudioClip(layout));
}

void TimelineTests::shortWaveformDoesNotStretchPastMedia()
{
    const std::vector<float> samples(8'000, 0.5f);
    const WaveformPyramid pyramid = WaveformPyramid::fromMonoFloat(samples.data(), 8'000, 8'000);
    TimelineViewport viewport;
    viewport.setDuration(pyramid.duration());
    viewport.setHasMedia(true);
    viewport.setPixelsPerMs(0.1);
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 1'000.0;
    metrics.viewportHeight = 180.0;
    const TimelineSceneLayout layout = TimelineSceneBuilder::build(viewport, {}, &pyramid, metrics);
    QCOMPARE(layout.waveform.size(), 100);
}

void TimelineTests::sceneLayoutOmitsWaveformWithoutData()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(1'000));
    viewport.setPixelsPerMs(0.2);
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 120.0;
    metrics.viewportHeight = 180.0;

    const TimelineSceneLayout missing = TimelineSceneBuilder::build(viewport, {}, nullptr, metrics);
    QVERIFY(missing.waveform.isEmpty());
    QVERIFY(!TimelineSceneBuilder::shouldDrawWaveform(missing));
    QVERIFY(!TimelineSceneBuilder::shouldDrawAudioClip(missing));

    const WaveformPyramid emptyPyramid;
    const TimelineSceneLayout empty = TimelineSceneBuilder::build(viewport, {}, &emptyPyramid, metrics);
    QVERIFY(empty.waveform.isEmpty());
    QVERIFY(!TimelineSceneBuilder::shouldDrawWaveform(empty));
    QVERIFY(!TimelineSceneBuilder::shouldDrawAudioClip(empty));
}

void TimelineTests::rulerLabelFormatsMajorTicks()
{
    QCOMPARE(TimelineViewport::formatRulerLabel(0, 5'000), QStringLiteral("00:00"));
    QCOMPARE(TimelineViewport::formatRulerLabel(5'000, 5'000), QStringLiteral("00:05"));
    QCOMPARE(TimelineViewport::formatRulerLabel(10'000, 5'000), QStringLiteral("00:10"));
    QCOMPARE(TimelineViewport::formatRulerLabel(15'000, 5'000), QStringLiteral("00:15"));
    QCOMPARE(TimelineViewport::formatRulerLabel(3'600'000, 60'000), QStringLiteral("1:00:00"));
    QCOMPARE(TimelineViewport::formatRulerLabel(40, 40), QStringLiteral("00:00.040"));
    QCOMPARE(TimelineViewport::formatRulerLabel(500, 100), QStringLiteral("00:00.5"));
}

void TimelineTests::sceneHitTestPrefersTrimHandles()
{
    TimelineViewport viewport;
    viewport.setDuration(MediaTime::fromMilliseconds(5'000));
    viewport.setPixelsPerMs(1.0);
    const QList<Subtitle> cues{makeCue(QStringLiteral("a"), 10, 110)};
    TimelineSceneMetrics metrics;
    metrics.viewportWidth = 200.0;
    metrics.viewportHeight = 120.0;
    const TimelineSceneLayout layout = TimelineSceneBuilder::build(viewport, cues, nullptr, metrics);
    QCOMPARE(layout.cues.size(), 1);
    const TimelineSceneRect rect = layout.cues.front().rect;
    const TimelineHit left = TimelineSceneBuilder::hitTest(layout, rect.x + 2.0, rect.y + 4.0, metrics);
    QCOMPARE(left.kind, TimelineHitKind::CueTrimStart);
    QCOMPARE(left.cueId, QStringLiteral("a"));
    const TimelineHit body = TimelineSceneBuilder::hitTest(layout, rect.x + rect.width / 2.0, rect.y + 4.0, metrics);
    QCOMPARE(body.kind, TimelineHitKind::CueBody);
    const TimelineHit right = TimelineSceneBuilder::hitTest(layout, rect.x + rect.width - 2.0, rect.y + 4.0, metrics);
    QCOMPARE(right.kind, TimelineHitKind::CueTrimEnd);
    const TimelineHit ruler = TimelineSceneBuilder::hitTest(layout, 20.0, 4.0, metrics);
    QCOMPARE(ruler.kind, TimelineHitKind::Ruler);
}

QTEST_APPLESS_MAIN(TimelineTests)

#include "test_timeline.moc"
