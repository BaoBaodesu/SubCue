#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_time.h"
#include "media/hw_accel.h"
#include "playback/audio_clock.h"
#include "playback/audio_output.h"
#include "playback/frame_index.h"
#include "playback/frame_stepper.h"
#include "playback/playback_engine.h"
#include "playback/seek_controller.h"
#include "playback/video_scheduler.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtTest/QTest>

using namespace subcue;

class PlaybackTests final : public QObject {
    Q_OBJECT

private slots:
    void audioClockUsesFirstPtsConsumedSamplesAndBuffer();
    void audioClockPauseResumeDoesNotJump();
    void audioOutputRejectsStaleGeneration();
    void audioOutputKeepsWholeDecodeBlockAtCapacityBoundary();
    void videoSchedulerWaitsDisplaysAndDrops();
    void seekControllerLatestWins();
    void seekDecodesFromKeyframeToTarget();
    void frameIndexCapturesPtsAndKeyframes();
    void frameStepperUsesGopAndHistoryRing();
    void hwAccelFallbackOrderAndSoftwareDecode();
    void hwRuntimeFailureReopensSoftware();
    void playbackPauseResumeStaysSynced();
    void playbackAudioOnlyAdvancesPosition();
    void playbackLatestWinsSeekDisplaysNewGeneration();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
};

QString PlaybackTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

void PlaybackTests::audioClockUsesFirstPtsConsumedSamplesAndBuffer()
{
    AudioClock clock;
    clock.start(MediaTime::fromMilliseconds(250), 48'000);
    clock.setWrittenSamples(4'800);
    clock.setBufferedSamples(2'400);
    QCOMPARE(clock.consumedSamples(), 2'400);
    QCOMPARE(clock.now().microseconds(), 250'000 + 50'000);
}

void PlaybackTests::audioClockPauseResumeDoesNotJump()
{
    AudioOutput output;
    QVERIFY(output.configure(48'000, 2));
    QVector<float> samples(48'000, 0.0f);
    QVERIFY(output.write(samples, MediaTime::fromMilliseconds(0), 0));
    QCOMPARE(output.consumeDuration(MediaTime::fromMilliseconds(200)), 9'600);

    AudioClock clock;
    clock.syncFrom(output);
    const MediaTime pausedAt = clock.now();
    QCOMPARE(pausedAt.milliseconds(), 200);

    clock.pause();
    output.pause();
    QCOMPARE(output.consumeDuration(MediaTime::fromMilliseconds(400)), 0);
    clock.syncFrom(output);
    QCOMPARE(clock.now().microseconds(), pausedAt.microseconds());

    output.resume();
    clock.resume();
    QCOMPARE(output.consumeDuration(MediaTime::fromMilliseconds(50)), 2'400);
    clock.syncFrom(output);
    QCOMPARE(clock.now().milliseconds(), pausedAt.milliseconds() + 50);
}

void PlaybackTests::audioOutputRejectsStaleGeneration()
{
    AudioOutput output;
    QVERIFY(output.configure(48'000, 1));
    output.setGeneration(4);
    QVector<float> stale(48, 0.1f);
    QVERIFY(!output.write(stale, MediaTime::fromMilliseconds(0), 3));
    QVERIFY(output.write(stale, MediaTime::fromMilliseconds(10), 4));
    QCOMPARE(output.writtenSamples(), 48);
    QCOMPARE(output.bufferedSamples(), 48);
}

void PlaybackTests::audioOutputKeepsWholeDecodeBlockAtCapacityBoundary()
{
    AudioOutput output;
    QVERIFY(output.configure(48'000, 2));
    QVector<float> almostFull(47'040, 0.1f);
    QVector<float> decodeBlock(1'920, 0.2f);
    QVERIFY(output.write(almostFull, MediaTime::fromMilliseconds(0), 0));
    QVERIFY(output.write(decodeBlock, MediaTime::fromMilliseconds(490), 0));
    QCOMPARE(output.bufferedFrames(), 24'480);
    QVERIFY(!output.write(decodeBlock, MediaTime::fromMilliseconds(510), 0));
}

void PlaybackTests::videoSchedulerWaitsDisplaysAndDrops()
{
    VideoScheduler scheduler;
    const MediaTime clock = MediaTime::fromMilliseconds(200);

    const ScheduleDecision wait = scheduler.evaluate(MediaTime::fromMilliseconds(280), clock);
    QCOMPARE(wait.action, FrameAction::Wait);
    QCOMPARE(wait.wakeDelay.milliseconds(), 80);

    const ScheduleDecision display = scheduler.evaluate(MediaTime::fromMilliseconds(190), clock);
    QCOMPARE(display.action, FrameAction::Display);
    QCOMPARE(display.wakeDelay.microseconds(), 0);

    const ScheduleDecision drop = scheduler.evaluate(MediaTime::fromMilliseconds(150), clock);
    QCOMPARE(drop.action, FrameAction::Drop);
}

void PlaybackTests::seekControllerLatestWins()
{
    SeekController controller;
    const quint64 first = controller.request(MediaTime::fromMilliseconds(100));
    const quint64 second = controller.request(MediaTime::fromMilliseconds(800));
    const quint64 latest = controller.request(MediaTime::fromMilliseconds(400));
    QVERIFY(!controller.canCommit(first));
    QVERIFY(!controller.canCommit(second));
    QVERIFY(controller.canCommit(latest));
    QCOMPARE(controller.target().milliseconds(), 400);
    QCOMPARE(controller.generation(), latest);
}

void PlaybackTests::seekDecodesFromKeyframeToTarget()
{
    Demuxer demuxer;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(demuxer.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_VIDEO);
    const AVStream *stream = demuxer.stream(streamIndex);
    QVERIFY(stream);

    Decoder decoder;
    QVERIFY(decoder.open(*stream, &error));

    SeekController controller;
    const quint64 generation = controller.request(MediaTime::fromMilliseconds(200));
    QVERIFY(controller.executeKeyframeSeek(demuxer, decoder, streamIndex, stream->time_base, generation, &error));
    const FramePtr frame = controller.decodeToTarget(
        demuxer, decoder, streamIndex, stream->time_base, generation, &error);
    QVERIFY(frame);
    const MediaTime pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, stream->time_base);
    QVERIFY(pts.milliseconds() >= 200);
    QVERIFY(pts.milliseconds() < 400);
}

void PlaybackTests::frameIndexCapturesPtsAndKeyframes()
{
    FrameIndex index;
    AppError error(ErrorDomain::Decoder, 0, QString());
    QVERIFY(index.build(mediaPath(QStringLiteral("vfr.mp4")), &error));
    QVERIFY(index.size() >= 2);
    QVERIFY(index.at(0).keyframe);
    QVERIFY(index.at(0).pts.microseconds() >= 0);
    for (int i = 1; i < index.size(); ++i) {
        QVERIFY(index.at(i).pts.microseconds() >= index.at(i - 1).pts.microseconds());
    }
    const int keyframe = index.keyframeIndexAtOrBefore(index.size() - 1);
    QVERIFY(keyframe >= 0);
    QVERIFY(index.at(keyframe).keyframe);
}

void PlaybackTests::frameStepperUsesGopAndHistoryRing()
{
    FrameStepper stepper;
    AppError error(ErrorDomain::Decoder, 0, QString());
    QVERIFY(stepper.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    QVERIFY(stepper.index().size() >= 3);
    QCOMPARE(stepper.currentIndex(), 0);
    QVERIFY(stepper.gopSize() >= 1);
    QVERIFY(stepper.cachedBytes() <= FrameStepper::kMaximumCacheBytes);

    const MediaTime first = stepper.currentPts();
    QVERIFY(stepper.stepForward(&error));
    QVERIFY(stepper.historySize() >= 1);
    QVERIFY(stepper.currentPts() > first);
    QVERIFY(stepper.gopSize() >= 1);
    QVERIFY(stepper.cachedBytes() <= FrameStepper::kMaximumCacheBytes);

    const MediaTime second = stepper.currentPts();
    QVERIFY(stepper.stepForward(&error));
    QVERIFY(stepper.stepBackward(&error));
    QCOMPARE(stepper.currentPts().microseconds(), second.microseconds());
    QVERIFY(stepper.stepBackward(&error));
    QCOMPARE(stepper.currentPts().microseconds(), first.microseconds());
    QVERIFY(stepper.cachedBytes() <= FrameStepper::kMaximumCacheBytes);
}

void PlaybackTests::hwAccelFallbackOrderAndSoftwareDecode()
{
    const QVector<HwAccelType> order = hwAccelFallbackOrder();
    QCOMPARE(order.size(), 5);
    QCOMPARE(order.at(0), HwAccelType::D3D11VA);
    QCOMPARE(order.at(1), HwAccelType::D3D12VA);
    QCOMPARE(order.at(2), HwAccelType::QSV);
    QCOMPARE(order.at(3), HwAccelType::CUDA);
    QCOMPARE(order.at(4), HwAccelType::Software);

    Demuxer demuxer;
    AppError error(ErrorDomain::Decoder, 0, QString());
    QVERIFY(demuxer.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    const AVStream *stream = demuxer.stream(demuxer.bestStream(AVMEDIA_TYPE_VIDEO));
    QVERIFY(stream);

    Decoder decoder;
    QVERIFY(decoder.openWithFallback(*stream, &error));
    QVERIFY(!decoder.attempts().isEmpty());
    QCOMPARE(decoder.attempts().constFirst().type, HwAccelType::D3D11VA);
    QCOMPARE(decoder.attempts().constLast().type, decoder.activeType());
    QVERIFY(decoder.attempts().constLast().succeeded);

    FramePtr frame = makeFrame();
    QVERIFY(frame);
    QVERIFY(decoder.pullFrame(demuxer, stream->index, frame.get(), &error));
    QVERIFY(frame->width > 0);
    QVERIFY(!frame->hw_frames_ctx);
}

void PlaybackTests::hwRuntimeFailureReopensSoftware()
{
    Demuxer demuxer;
    AppError error(ErrorDomain::Decoder, 0, QString());
    QVERIFY(demuxer.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    const AVStream *stream = demuxer.stream(demuxer.bestStream(AVMEDIA_TYPE_VIDEO));
    QVERIFY(stream);

    Decoder decoder;
    QVERIFY(decoder.openWithFallback(*stream, &error));
    QVERIFY(decoder.handleRuntimeFailure(&error));
    QCOMPARE(decoder.activeType(), HwAccelType::Software);
    QVERIFY(decoder.attempts().constLast().succeeded);
    QCOMPARE(decoder.attempts().constLast().details, QStringLiteral("runtime-fallback"));

    QVERIFY(demuxer.seek(stream->index, 0, &error));
    decoder.flush();
    FramePtr frame = makeFrame();
    QVERIFY(decoder.pullFrame(demuxer, stream->index, frame.get(), &error));
    QVERIFY(frame->width > 0);

    PlaybackEngine engine;
    QVERIFY(engine.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    engine.requestHwRuntimeFailure();
    (void)engine.seek(MediaTime::fromMilliseconds(40));
    engine.play();
    QVERIFY(engine.waitForDisplayedFrame(3'000));
    QVERIFY(engine.hwRuntimeFallbackOccurred());
    QCOMPARE(engine.hwAccel(), HwAccelType::Software);
}

void PlaybackTests::playbackPauseResumeStaysSynced()
{
    PlaybackEngine engine;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(engine.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    engine.play();

    QElapsedTimer timer;
    timer.start();
    qint64 consumed = 0;
    while (timer.elapsed() < 3'000 && consumed < 4'800) {
        consumed += engine.consumeAudio(MediaTime::fromMilliseconds(20));
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(consumed > 0);
    engine.consumeAudio(MediaTime::fromMilliseconds(80));
    const MediaTime playing = engine.position();
    QVERIFY(playing.milliseconds() >= 100);

    engine.pause();
    const MediaTime pausedAt = engine.position();
    engine.consumeAudio(MediaTime::fromMilliseconds(250));
    QCOMPARE(engine.position().microseconds(), pausedAt.microseconds());

    engine.play();
    engine.consumeAudio(MediaTime::fromMilliseconds(40));
    QCOMPARE(engine.position().milliseconds(), pausedAt.milliseconds() + 40);
}

void PlaybackTests::playbackAudioOnlyAdvancesPosition()
{
    PlaybackEngine engine;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(engine.open(mediaPath(QStringLiteral("audio.wav")), &error));
    QVERIFY(engine.hasAudio());
    QVERIFY(!engine.hasVideo());
    engine.play();

    QElapsedTimer timer;
    timer.start();
    qint64 consumed = 0;
    while (timer.elapsed() < 3'000 && consumed < 2'400) {
        consumed += engine.consumeAudio(MediaTime::fromMilliseconds(20));
        QCoreApplication::processEvents();
        QTest::qWait(10);
    }
    QVERIFY(consumed > 0);
    QVERIFY(engine.position().milliseconds() > 0);
}

void PlaybackTests::playbackLatestWinsSeekDisplaysNewGeneration()
{
    PlaybackEngine engine;
    AppError error(ErrorDomain::Media, 0, QString());
    QVERIFY(engine.open(mediaPath(QStringLiteral("cfr_av.mp4")), &error));
    const quint64 stale = engine.seek(MediaTime::fromMilliseconds(80));
    const quint64 ignored = engine.seek(MediaTime::fromMilliseconds(700));
    const quint64 latest = engine.seek(MediaTime::fromMilliseconds(300));
    QVERIFY(!engine.seekController().canCommit(stale));
    QVERIFY(!engine.seekController().canCommit(ignored));
    QVERIFY(engine.seekController().canCommit(latest));

    engine.play();
    QVERIFY(engine.waitForDisplayedFrame(4'000));
    const DisplayedVideoFrame displayed = engine.displayedFrame();
    QCOMPARE(displayed.generation, latest);
    QVERIFY(displayed.pts.milliseconds() >= 250);
    QVERIFY(displayed.pts.milliseconds() < 500);
}

QTEST_MAIN(PlaybackTests)

#include "test_playback.moc"
