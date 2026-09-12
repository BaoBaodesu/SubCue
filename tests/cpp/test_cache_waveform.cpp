#include "cache/cache_key.h"
#include "cache/lru_cache.h"
#include "cache/preview_cache.h"
#include "media/media_probe.h"
#include "waveform/waveform_generator.h"
#include "waveform/waveform_pyramid.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtTest/QTest>

#include <atomic>
#include <memory>
#include <variant>

extern "C" {
#include <libavutil/error.h>
}

using namespace subcue;

class CacheWaveformTests final : public QObject {
    Q_OBJECT

private slots:
    void cacheKeyIncludesCanonicalPathSizeMtimeStreamAndChunkHashes();
    void cacheKeyChangesWhenContentOrStreamChanges();
    void lruEvictsLeastRecentlyUsedAcrossKinds();
    void pyramidLevelsAreSixteenTimesFour();
    void pyramidPeaksMatchSourceMinMax();
    void pyramidSelectsCoarserLevelForWideViewport();
    void generatorBuildsMonoPyramidFromAudio();
    void generatorCancelDoesNotProducePyramid();
    void thumbnailLazyLoadOnlyRequestedTimes();
    void thumbnailCacheHitReusesImage();
    void previewCacheLatestWinsRejectsStaleGeneration();
    void thumbnailFailsForAudioOnly();

private:
    [[nodiscard]] QString mediaPath(const QString &name) const;
    [[nodiscard]] CacheKey manualKey(const QString &path, const QString &qualifier) const;
};

QString CacheWaveformTests::mediaPath(const QString &name) const
{
    return QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(name);
}

CacheKey CacheWaveformTests::manualKey(const QString &path, const QString &qualifier) const
{
    CacheKey key;
    key.canonicalPath = path;
    key.size = 1;
    key.mtimeMs = 1;
    key.streamIndex = 0;
    key.headHash = QByteArray(32, 'h');
    key.tailHash = QByteArray(32, 't');
    key.qualifier = qualifier;
    return key;
}

void CacheWaveformTests::cacheKeyIncludesCanonicalPathSizeMtimeStreamAndChunkHashes()
{
    const QString path = mediaPath(QStringLiteral("audio.wav"));
    AppError error(ErrorDomain::Media, 0, QString());
    const CacheKey key = makeCacheKey(path, 0, waveformQualifier(8'000), &error);
    QVERIFY(key.isValid());
    QVERIFY(QFileInfo(key.canonicalPath).isAbsolute());
    QCOMPARE(key.canonicalPath, QDir::fromNativeSeparators(QFileInfo(path).canonicalFilePath()));
    QCOMPARE(key.size, QFileInfo(path).size());
    QVERIFY(key.mtimeMs > 0);
    QCOMPARE(key.streamIndex, 0);
    QCOMPARE(key.headHash.size(), 32);
    QCOMPARE(key.tailHash.size(), 32);
    QCOMPARE(key.qualifier, QStringLiteral("waveform:8000"));
    QVERIFY(key.id().contains(key.canonicalPath));
    QVERIFY(key.id().contains(QString::fromLatin1(key.headHash.toHex())));
    QVERIFY(key.id().contains(QString::fromLatin1(key.tailHash.toHex())));
}

void CacheWaveformTests::cacheKeyChangesWhenContentOrStreamChanges()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firstPath = dir.filePath(QStringLiteral("first.bin"));
    const QString secondPath = dir.filePath(QStringLiteral("second.bin"));
    QByteArray payload(kCacheKeyChunkBytes * 2, 'A');
    QFile first(firstPath);
    QVERIFY(first.open(QIODevice::WriteOnly));
    QCOMPARE(first.write(payload), payload.size());
    first.close();

    payload[payload.size() - 1] = 'B';
    QFile second(secondPath);
    QVERIFY(second.open(QIODevice::WriteOnly));
    QCOMPARE(second.write(payload), payload.size());
    second.close();

    AppError error(ErrorDomain::Media, 0, QString());
    const CacheKey firstKey = makeCacheKey(firstPath, 0, QStringLiteral("waveform:8000"), &error);
    const CacheKey secondKey = makeCacheKey(secondPath, 0, QStringLiteral("waveform:8000"), &error);
    const CacheKey otherStream = makeCacheKey(firstPath, 1, QStringLiteral("waveform:8000"), &error);
    QVERIFY(firstKey.isValid());
    QVERIFY(secondKey.isValid());
    QCOMPARE(firstKey.headHash, secondKey.headHash);
    QVERIFY(firstKey.tailHash != secondKey.tailHash);
    QVERIFY(firstKey.id() != secondKey.id());
    QVERIFY(firstKey.id() != otherStream.id());
    QCOMPARE(otherStream.streamIndex, 1);
}

void CacheWaveformTests::lruEvictsLeastRecentlyUsedAcrossKinds()
{
    QImage red(8, 8, QImage::Format_RGBA8888);
    QImage green(8, 8, QImage::Format_RGBA8888);
    QImage blue(8, 8, QImage::Format_RGBA8888);
    red.fill(QColor(255, 0, 0));
    green.fill(QColor(0, 255, 0));
    blue.fill(QColor(0, 0, 255));
    QCOMPARE(red.sizeInBytes(), 256);

    LruCache cache(600);
    const CacheKey keyA = manualKey(QStringLiteral("a"), QStringLiteral("thumb:a"));
    const CacheKey keyB = manualKey(QStringLiteral("b"), QStringLiteral("thumb:b"));
    const CacheKey keyC = manualKey(QStringLiteral("c"), QStringLiteral("thumb:c"));
    QVERIFY(cache.insertThumbnail(keyA, red));
    QVERIFY(cache.insertThumbnail(keyB, green));
    QCOMPARE(cache.size(), 2);

    QCOMPARE(cache.thumbnail(keyA).pixelColor(0, 0), QColor(255, 0, 0));
    QVERIFY(cache.insertThumbnail(keyC, blue));
    QCOMPARE(cache.size(), 2);
    QVERIFY(cache.contains(keyA));
    QVERIFY(!cache.contains(keyB));
    QVERIFY(cache.contains(keyC));
    QCOMPARE(cache.stats().evictions, 1);

    QVector<float> samples(16, 0.5f);
    const auto pyramid = std::make_shared<WaveformPyramid>(
        WaveformPyramid::fromMonoFloat(samples.constData(), samples.size(), 8'000));
    const CacheKey waveKey = manualKey(QStringLiteral("wave"), QStringLiteral("waveform:8000"));
    QVERIFY(cache.insertWaveform(waveKey, pyramid));
    QVERIFY(cache.contains(waveKey));
    QVERIFY(cache.waveform(waveKey));
}

void CacheWaveformTests::pyramidLevelsAreSixteenTimesFour()
{
    QVector<float> samples(256, 0.0f);
    const WaveformPyramid pyramid = WaveformPyramid::fromMonoFloat(samples.constData(), samples.size(), 8'000);
    QCOMPARE(pyramid.sampleRate(), 8'000);
    QCOMPARE(pyramid.sampleCount(), 256);
    QCOMPARE(pyramid.levels().size(), 3);
    QCOMPARE(pyramid.levels()[0].samplesPerPeak, 16);
    QCOMPARE(pyramid.levels()[0].peaks.size(), 16);
    QCOMPARE(pyramid.levels()[1].samplesPerPeak, 64);
    QCOMPARE(pyramid.levels()[1].peaks.size(), 4);
    QCOMPARE(pyramid.levels()[2].samplesPerPeak, 256);
    QCOMPARE(pyramid.levels()[2].peaks.size(), 1);
}

void CacheWaveformTests::pyramidPeaksMatchSourceMinMax()
{
    QVector<float> samples(64, 0.0f);
    for (int index = 0; index < 16; ++index) {
        samples[index] = 0.25f;
    }
    samples[3] = -0.5f;
    samples[8] = 0.75f;
    for (int index = 16; index < 32; ++index) {
        samples[index] = 0.1f;
    }
    for (int index = 32; index < 48; ++index) {
        samples[index] = -1.0f;
    }
    for (int index = 48; index < 64; ++index) {
        samples[index] = 1.0f;
    }

    const WaveformPyramid pyramid = WaveformPyramid::fromMonoFloat(samples.constData(), samples.size(), 8'000);
    QCOMPARE(pyramid.levels().size(), 2);
    QCOMPARE(pyramid.levels()[0].peaks.size(), 4);
    QCOMPARE(pyramid.levels()[0].peaks[0].min, -0.5f);
    QCOMPARE(pyramid.levels()[0].peaks[0].max, 0.75f);
    QCOMPARE(pyramid.levels()[0].peaks[1].min, 0.1f);
    QCOMPARE(pyramid.levels()[0].peaks[1].max, 0.1f);
    QCOMPARE(pyramid.levels()[0].peaks[2].min, -1.0f);
    QCOMPARE(pyramid.levels()[0].peaks[2].max, -1.0f);
    QCOMPARE(pyramid.levels()[0].peaks[3].min, 1.0f);
    QCOMPARE(pyramid.levels()[0].peaks[3].max, 1.0f);
    QCOMPARE(pyramid.levels()[1].samplesPerPeak, 64);
    QCOMPARE(pyramid.levels()[1].peaks[0].min, -1.0f);
    QCOMPARE(pyramid.levels()[1].peaks[0].max, 1.0f);
}

void CacheWaveformTests::pyramidSelectsCoarserLevelForWideViewport()
{
    QVector<float> samples(256, 0.0f);
    samples[0] = -0.5f;
    samples[255] = 0.75f;
    const WaveformPyramid pyramid = WaveformPyramid::fromMonoFloat(samples.constData(), samples.size(), 8'000);
    QCOMPARE(pyramid.levelForSamplesPerPeak(20)->samplesPerPeak, 16);
    QCOMPARE(pyramid.levelForSamplesPerPeak(64)->samplesPerPeak, 64);
    QCOMPARE(pyramid.levelForSamplesPerPeak(100)->samplesPerPeak, 64);
    QCOMPARE(pyramid.levelForSamplesPerPeak(256)->samplesPerPeak, 256);

    const QVector<WaveformPeak> columns = pyramid.peaksForRange(
        MediaTime::fromMicroseconds(0),
        pyramid.duration(),
        2);
    QCOMPARE(columns.size(), 2);
    QCOMPARE(columns[0].min, -0.5f);
    QCOMPARE(columns[1].max, 0.75f);
}

void CacheWaveformTests::generatorBuildsMonoPyramidFromAudio()
{
    const WaveformGenerator::Result result = WaveformGenerator().generate(
        mediaPath(QStringLiteral("audio.wav")));
    QVERIFY(std::holds_alternative<std::shared_ptr<const WaveformPyramid>>(result));
    const auto pyramid = std::get<std::shared_ptr<const WaveformPyramid>>(result);
    QVERIFY(pyramid);
    QVERIFY(pyramid->sampleCount() > 0);
    QCOMPARE(pyramid->sampleRate(), 8'000);
    QVERIFY(!pyramid->levels().isEmpty());
    QCOMPARE(pyramid->levels().first().samplesPerPeak, 16);
    for (int index = 1; index < pyramid->levels().size(); ++index) {
        QCOMPARE(
            pyramid->levels()[index].samplesPerPeak,
            pyramid->levels()[index - 1].samplesPerPeak * WaveformPyramid::kLevelFactor);
        QVERIFY(pyramid->levels()[index].peaks.size() <= pyramid->levels()[index - 1].peaks.size());
    }
}

void CacheWaveformTests::generatorCancelDoesNotProducePyramid()
{
    std::atomic<bool> cancel{true};
    const WaveformGenerator::Result result = WaveformGenerator().generate(
        mediaPath(QStringLiteral("audio.wav")),
        -1,
        WaveformGenerator::kDefaultSampleRate,
        &cancel);
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).code(), AVERROR(EINTR));
}

void CacheWaveformTests::thumbnailLazyLoadOnlyRequestedTimes()
{
    PreviewCache cache(16 * 1024 * 1024);
    const QString path = mediaPath(QStringLiteral("cfr_av.mp4"));
    const QSize size(32, 24);
    const ProbeResult probe = MediaProbe::probe(path);
    QVERIFY(std::holds_alternative<MediaInfo>(probe));
    const int videoStream = std::get<MediaInfo>(probe).videoStreamIndex;
    QVERIFY(videoStream >= 0);
    AppError error(ErrorDomain::Media, 0, QString());
    const CacheKey firstKey = makeCacheKey(
        path,
        videoStream,
        thumbnailQualifier(MediaTime::fromMicroseconds(0), size),
        &error);
    QVERIFY(firstKey.isValid());
    QVERIFY(cache.peekThumbnail(firstKey).isNull());
    QCOMPARE(cache.store().size(), 0);

    const ThumbnailLoader::Result first = cache.thumbnail(path, MediaTime::fromMicroseconds(0), size);
    QVERIFY(std::holds_alternative<QImage>(first));
    QCOMPARE(std::get<QImage>(first).size(), size);
    QCOMPARE(cache.store().size(), 1);
    QVERIFY(!cache.peekThumbnail(firstKey).isNull());

    const QVector<QImage> range = cache.thumbnailsInRange(
        path,
        MediaTime::fromMicroseconds(0),
        MediaTime::fromMilliseconds(600),
        MediaTime::fromMilliseconds(300),
        size);
    QCOMPARE(range.size(), 2);
    QVERIFY(!range[0].isNull());
    QVERIFY(!range[1].isNull());
    QCOMPARE(cache.store().size(), 2);
}

void CacheWaveformTests::thumbnailCacheHitReusesImage()
{
    PreviewCache cache(16 * 1024 * 1024);
    const QString path = mediaPath(QStringLiteral("cfr_av.mp4"));
    const QSize size(32, 24);
    const ThumbnailLoader::Result first = cache.thumbnail(path, MediaTime::fromMilliseconds(200), size);
    QVERIFY(std::holds_alternative<QImage>(first));
    QCOMPARE(cache.store().size(), 1);
    QCOMPARE(cache.store().stats().misses, 1);
    QCOMPARE(cache.store().stats().hits, 0);

    const ThumbnailLoader::Result second = cache.thumbnail(path, MediaTime::fromMilliseconds(200), size);
    QVERIFY(std::holds_alternative<QImage>(second));
    QCOMPARE(cache.store().size(), 1);
    QCOMPARE(cache.store().stats().hits, 1);
    QCOMPARE(std::get<QImage>(first).sizeInBytes(), std::get<QImage>(second).sizeInBytes());
}

void CacheWaveformTests::previewCacheLatestWinsRejectsStaleGeneration()
{
    PreviewCache cache(16 * 1024 * 1024);
    const quint64 stale = cache.generation();
    const quint64 latest = cache.bumpGeneration();
    QVERIFY(latest != stale);

    const WaveformGenerator::Result staleWave = cache.waveform(
        mediaPath(QStringLiteral("audio.wav")),
        -1,
        WaveformGenerator::kDefaultSampleRate,
        stale);
    QVERIFY(std::holds_alternative<AppError>(staleWave));
    QCOMPARE(std::get<AppError>(staleWave).code(), AVERROR(ECANCELED));
    QCOMPARE(cache.store().size(), 0);

    const WaveformGenerator::Result currentWave = cache.waveform(
        mediaPath(QStringLiteral("audio.wav")),
        -1,
        WaveformGenerator::kDefaultSampleRate,
        latest);
    QVERIFY(std::holds_alternative<std::shared_ptr<const WaveformPyramid>>(currentWave));
    QCOMPARE(cache.store().size(), 1);
}

void CacheWaveformTests::thumbnailFailsForAudioOnly()
{
    const ThumbnailLoader::Result result = ThumbnailLoader().decode(
        mediaPath(QStringLiteral("audio.wav")),
        MediaTime::fromMicroseconds(0),
        QSize(32, 24));
    QVERIFY(std::holds_alternative<AppError>(result));
    QVERIFY(std::get<AppError>(result).code() < 0);
}

QTEST_APPLESS_MAIN(CacheWaveformTests)

#include "test_cache_waveform.moc"
