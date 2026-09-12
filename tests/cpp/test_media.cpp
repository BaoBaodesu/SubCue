#include "media/bounded_queue.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"
#include "media/media_engine.h"

#include <QtCore/QDir>
#include <QtTest/QTest>

#include <variant>

using namespace subcue;

class MediaTests final : public QObject {
    Q_OBJECT

private slots:
    void ffmpegErrorIncludesText();
    void timeBaseConversionIsIntegerBased();
    void boundedQueueHonorsCapacityAndGeneration();
    void probesAudioVideoAndVfr();
    void rejectsCorruptMedia();
    void decodesAndConvertsVideo();
    void decodesAndResamplesAudio();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
};

QString MediaTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

void MediaTests::ffmpegErrorIncludesText()
{
    const QString text = ffmpegErrorString(AVERROR(EINVAL));
    QVERIFY(!text.isEmpty());
    QVERIFY(!text.contains(QStringLiteral("Unknown")));
}

void MediaTests::timeBaseConversionIsIntegerBased()
{
    const MediaTime time = mediaTimeFromTimestamp(90'000, AVRational{1, 90'000});
    QCOMPARE(time.microseconds(), 1'000'000);
    QCOMPARE(timestampFromMediaTime(time, AVRational{1, 90'000}), 90'000);
    QCOMPARE(mediaTimeFromTimestamp(AV_NOPTS_VALUE, AVRational{1, 1}).microseconds(), -1);
}

void MediaTests::boundedQueueHonorsCapacityAndGeneration()
{
    BoundedQueue<int> queue(2, 8);
    QVERIFY(queue.push(1, 4, 0));
    QVERIFY(queue.push(2, 4, 0));
    QCOMPARE(queue.size(), 2);
    QCOMPARE(queue.totalBytes(), 8);

    BoundedQueue<int>::Item item;
    QVERIFY(queue.tryPop(item));
    QCOMPARE(item.value, 1);
    QCOMPARE(item.generation, 0ULL);
    queue.setGeneration(3);
    QCOMPARE(queue.size(), 0);
    QVERIFY(!queue.push(4, 4, 2));
    QVERIFY(queue.push(5, 4, 3));
    queue.abort();
    QVERIFY(!queue.tryPop(item));
    queue.resetAbort();
    queue.clear();
}

void MediaTests::probesAudioVideoAndVfr()
{
    MediaEngine engine;
    ProbeResult result = engine.probe(mediaPath(QStringLiteral("cfr_av.mp4")));
    QVERIFY(std::holds_alternative<MediaInfo>(result));
    const MediaInfo &video = std::get<MediaInfo>(result);
    QVERIFY(video.videoStreamIndex >= 0);
    QVERIFY(video.audioStreamIndex >= 0);
    QCOMPARE(video.variableFrameRate, false);
    QVERIFY(video.duration.microseconds() >= 900'000);

    result = engine.probe(mediaPath(QStringLiteral("audio.wav")));
    QVERIFY(std::holds_alternative<MediaInfo>(result));
    const MediaInfo &audio = std::get<MediaInfo>(result);
    QCOMPARE(audio.videoStreamIndex, AVERROR_STREAM_NOT_FOUND);
    QVERIFY(audio.audioStreamIndex >= 0);

    result = engine.probe(mediaPath(QStringLiteral("vfr.mp4")));
    QVERIFY(std::holds_alternative<MediaInfo>(result));
    QCOMPARE(std::get<MediaInfo>(result).variableFrameRate, true);
}

void MediaTests::rejectsCorruptMedia()
{
    const ProbeResult result = MediaEngine().probe(mediaPath(QStringLiteral("corrupt.bin")));
    QVERIFY(std::holds_alternative<AppError>(result));
    QVERIFY(std::get<AppError>(result).code() < 0);
    QVERIFY(!std::get<AppError>(result).technicalDetails().isEmpty());
}

void MediaTests::decodesAndConvertsVideo()
{
    const VideoFrameResult result = MediaEngine().decodeFirstVideoFrame(
        mediaPath(QStringLiteral("cfr_av.mp4")), QSize(32, 24));
    QVERIFY(std::holds_alternative<QImage>(result));
    const QImage &image = std::get<QImage>(result);
    QCOMPARE(image.size(), QSize(32, 24));
    QCOMPARE(image.format(), QImage::Format_RGBA8888);
}

void MediaTests::decodesAndResamplesAudio()
{
    const AudioBufferResult result = MediaEngine().decodeAudio(
        mediaPath(QStringLiteral("audio.wav")), MediaTime::fromMilliseconds(200), 48'000, 2);
    QVERIFY(std::holds_alternative<AudioBuffer>(result));
    const AudioBuffer &audio = std::get<AudioBuffer>(result);
    QCOMPARE(audio.sampleRate, 48'000);
    QCOMPARE(audio.channels, 2);
    QCOMPARE(audio.samples.size(), 19'200);
    QVERIFY(audio.startTime.microseconds() >= 0);
}

QTEST_APPLESS_MAIN(MediaTests)

#include "test_media.moc"
