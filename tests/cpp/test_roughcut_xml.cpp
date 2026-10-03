#include "roughcut/xmeml_exporter.h"
#include "media/media_probe.h"

#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamReader>
#include <QtTest/QTest>

using namespace subcue;

class RoughCutXmlTests final : public QObject {
    Q_OBJECT

private slots:
    void rejectsInvalidSourceRanges();
    void exportsNonDestructiveTimeline();
    void exportsMonoWithoutStereoMarkers();
    void roundsSourceRangesOutward();
    void videoUsesSourceRateAndLinkedSequenceBounds();
    void rejectsUnverifiedVideoAndCutBoundary();
    void refusesToOverwriteSource();
    void ntcsManyClipsDoNotAccumulateSampleRounding();
    void writesPremiereAcceptanceFiles();
    void conformedSourceRangesDoNotExpandAgain();
};

RoughCutExportRequest requestFor(const QString &mediaPath)
{
    RoughCutExportRequest request;
    request.mediaPath = mediaPath;
    request.sampleRate = 48'000;
    request.channels = 1;
    request.sourceSampleCount = 480'000;
    request.frameRateNumerator = 60;
    request.clips = {{QStringLiteral("A"), 48'000, 96'000},
                     {QStringLiteral("B2"), 192'000, 288'000}};
    return request;
}

void RoughCutXmlTests::rejectsInvalidSourceRanges()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    RoughCutExportRequest request = requestFor(media.fileName());
    request.clips[0].sourceEndSample = request.sourceSampleCount + 1;
    QString error;
    QVERIFY(!XmemlExporter::validate(request, &error));
    QVERIFY(error.contains(QStringLiteral("A")));
}

void RoughCutXmlTests::exportsNonDestructiveTimeline()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    RoughCutExportRequest request = requestFor(media.fileName());
    request.channels = 2;
    const QByteArray data = XmemlExporter::build(request);
    QXmlStreamReader xml(data);
    int fileDefinitions = 0;
    int fileReferences = 0;
    int roughItems = 0;
    int referenceItems = 0;
    QString currentClipId;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QLatin1String("clipitem")) {
            currentClipId = xml.attributes().value(QStringLiteral("id")).toString();
            if (currentClipId.startsWith(QStringLiteral("rough-"))) ++roughItems;
            if (currentClipId.startsWith(QStringLiteral("reference-"))) ++referenceItems;
        } else if (xml.name() == QLatin1String("file")) {
            xml.readNextStartElement();
            if (xml.name() == QLatin1String("name")) ++fileDefinitions;
            else ++fileReferences;
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(fileDefinitions, 1);
    QCOMPARE(fileReferences, 3);
    QCOMPARE(roughItems, 4);
    QCOMPARE(referenceItems, 0);
    QCOMPARE(data.count("</track>"), 2);
    QCOMPARE(data.count("premiereTrackType=\"Stereo\""), 2);
    QCOMPARE(data.count("premiereChannelType=\"stereo\""), 4);
    QCOMPARE(data.count("<layout>stereo</layout>"), 0);
    QCOMPARE(data.count("<sourcechannel>"), 0);
    QCOMPARE(data.count("totalExplodedTrackCount=\"2\""), 2);
    QCOMPARE(data.count("outputchannelindex"), 0);
    QCOMPARE(data.count("<channelcount>2</channelcount>"), 1);
    QCOMPARE(data.count(QUrl::fromLocalFile(QFileInfo(media.fileName()).absoluteFilePath()).toEncoded()), 1);
    QCOMPARE(data.count("<enabled>FALSE</enabled>"), 0);
}

void RoughCutXmlTests::exportsMonoWithoutStereoMarkers()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    const QByteArray data = XmemlExporter::build(requestFor(media.fileName()));
    QCOMPARE(data.count("</track>"), 1);
    QVERIFY(data.contains("<track>"));
    QCOMPARE(data.count("premiereTrackType=\"Stereo\""), 0);
    QCOMPARE(data.count("premiereChannelType=\"stereo\""), 0);
    QCOMPARE(data.count("<layout>stereo</layout>"), 0);
    QCOMPARE(data.count("<sourcechannel>"), 0);
    QCOMPARE(data.count("<channelcount>1</channelcount>"), 1);
}

void RoughCutXmlTests::roundsSourceRangesOutward()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    RoughCutExportRequest request = requestFor(media.fileName());
    request.clips = {{QStringLiteral("fractional"), 1, 801}};
    const QByteArray data = XmemlExporter::build(request);
    QVERIFY(data.contains("<in>0</in>"));
    QVERIFY(data.contains("<out>2</out>"));
    QVERIFY(data.contains("<start>0</start>"));
    QVERIFY(data.contains("<end>2</end>"));
}

void RoughCutXmlTests::videoUsesSourceRateAndLinkedSequenceBounds()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    auto request = requestFor(media.fileName());
    request.frameRateNumerator = 30;
    MediaStreamInfo video;
    video.type = MediaStreamType::Video;
    video.width = 1920; video.height = 1080;
    video.frameRateNumerator = 25; video.frameRateDenominator = 1;
    video.duration = MediaTime::fromMilliseconds(10'000);
    request.mediaInfo.streams.append(video);
    request.mediaInfo.videoStreamIndex = 0;
    request.mediaInfo.cfrVerified = true;
    const QByteArray data = XmemlExporter::build(request);
    QVERIFY(!data.isEmpty());
    QVERIFY(data.contains("<timebase>25</timebase>"));
    QVERIFY(data.contains("<timebase>30</timebase>"));
    QCOMPARE(data.count("<in>30</in>"), 2);
    QCOMPARE(data.count("<out>60</out>"), 2);
    QCOMPARE(data.count("<start>0</start>"), 2);
    QCOMPARE(data.count("<end>30</end>"), 2);
    QCOMPARE(data.count("<pproTicksIn>254016000000</pproTicksIn>"), 2);
    QCOMPARE(data.count("<pproTicksOut>508032000000</pproTicksOut>"), 2);
    QCOMPARE(data.count("<linkclipref>video-1</linkclipref>"), 2);
    QCOMPARE(data.count("<linkclipref>rough-1</linkclipref>"), 2);
    // groupindex 只用于音频链接，视频链接不能包含该字段。
    QCOMPARE(data.count("<groupindex>1</groupindex>"), 4);
    request.frameRateNumerator = 30000;
    request.frameRateDenominator = 1001;
    QVERIFY(XmemlExporter::build(request).contains("<ntsc>TRUE</ntsc>"));
}

void RoughCutXmlTests::rejectsUnverifiedVideoAndCutBoundary()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    auto request = requestFor(media.fileName());
    request.clips = {{QStringLiteral("protected"), 1, 801, 0, 1, 801, 1, 801}};
    QString error;
    QVERIFY(!XmemlExporter::validate(request, &error));
    QVERIFY(!error.isEmpty());
    request = requestFor(media.fileName());
    MediaStreamInfo video;
    video.type = MediaStreamType::Video;
    request.mediaInfo.streams.append(video);
    request.mediaInfo.videoStreamIndex = 0;
    QVERIFY(!XmemlExporter::validate(request, &error));
    QVERIFY(error.contains(QStringLiteral("固定帧率")));
}

void RoughCutXmlTests::refusesToOverwriteSource()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    QCOMPARE(media.write("original"), 8);
    media.flush();
    QString error;
    QVERIFY(!XmemlExporter::save(media.fileName(), requestFor(media.fileName()), &error));
    QVERIFY(media.seek(0));
    QCOMPARE(media.readAll(), QByteArray("original"));
}

void RoughCutXmlTests::ntcsManyClipsDoNotAccumulateSampleRounding()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    auto request = requestFor(media.fileName());
    MediaStreamInfo video;
    video.type = MediaStreamType::Video;
    video.width = 1920; video.height = 1080;
    video.frameRateNumerator = 30000; video.frameRateDenominator = 1001;
    video.duration = MediaTime::fromMilliseconds(10000);
    request.mediaInfo.streams.append(video);
    request.mediaInfo.videoStreamIndex = 0;
    request.mediaInfo.cfrVerified = true;
    request.clips.clear();
    for (int i = 0; i < 1000; ++i) request.clips.append({QString::number(i), 0, 1601});
    const auto xml = XmemlExporter::build(request);
    QVERIFY(!xml.isEmpty());
    // 1000 个源帧恰好为 2002 个 60 fps 序列帧，不能逐片段先舍入到音频采样。
    QVERIFY(xml.contains("<duration>2002</duration>"));
    QVERIFY(xml.contains("<end>2002</end>"));
    QVERIFY(!xml.contains("<end>2003</end>"));
}

void RoughCutXmlTests::conformedSourceRangesDoNotExpandAgain()
{
    QTemporaryFile media;
    QVERIFY(media.open());
    auto request = requestFor(media.fileName());
    MediaStreamInfo video;
    video.type = MediaStreamType::Video;
    video.width = 640; video.height = 360;
    video.frameRateNumerator = 30000; video.frameRateDenominator = 1001;
    video.duration = MediaTime::fromMilliseconds(10'000);
    request.mediaInfo.streams.append(video);
    request.mediaInfo.videoStreamIndex = 0;
    request.mediaInfo.cfrVerified = true;
    request.frameRateNumerator = 30000;
    request.frameRateDenominator = 1001;
    request.clips = {{QStringLiteral("frame"), 1, 1601}};
    const auto original = XmemlExporter::build(request);
    QString error;
    request.clips = XmemlExporter::conformSourceRanges(request, &error);
    QCOMPARE(request.clips.size(), 1);
    QCOMPARE(request.clips.first().sourceStartSample, qint64(0));
    QCOMPARE(request.clips.first().sourceEndSample, qint64(1602));
    request.sourceRangesAreFrameAligned = true;
    QCOMPARE(XmemlExporter::build(request), original);
}

void RoughCutXmlTests::writesPremiereAcceptanceFiles()
{
    const QString directory = qEnvironmentVariable("SUBCUE_PREMIERE_ACCEPTANCE_DIR");
    if (directory.isEmpty()) QSKIP("仅在提供真实 Premiere 验收素材目录时生成文件。");
    const QStringList rates{QStringLiteral("24"), QStringLiteral("25"), QStringLiteral("30"),
        QStringLiteral("50"), QStringLiteral("60"), QStringLiteral("24000-1001"),
        QStringLiteral("30000-1001"), QStringLiteral("60000-1001")};
    for (const auto &rate : rates) {
        for (const int channels : {1, 2}) {
            const QString stem = QStringLiteral("source-%1-%2").arg(rate).arg(channels);
            const QString path = QDir(directory).filePath(stem + QStringLiteral(".mp4"));
            const auto probe = MediaProbe::verifyFrameRate(path, nullptr);
            QVERIFY2(std::holds_alternative<MediaInfo>(probe), qPrintable(path));
            auto request = requestFor(path);
            request.mediaInfo = std::get<MediaInfo>(probe);
            const auto &video = request.mediaInfo.streams.at(request.mediaInfo.videoStreamIndex);
            request.channels = channels;
            request.frameRateNumerator = video.frameRateNumerator;
            request.frameRateDenominator = video.frameRateDenominator;
            // 同时覆盖首端、非整帧边界、多个片段、末端和原素材拉边。
            request.clips = {{QStringLiteral("start"), 0, 48'000},
                {QStringLiteral("middle"), 96'001, 144'001},
                {QStringLiteral("tail"), 384'000, 480'000}};
            request.sequenceName = stem;
            QString error;
            QVERIFY2(XmemlExporter::save(QDir(directory).filePath(stem + QStringLiteral(".xml")), request, &error), qPrintable(error));
            if (rate == QStringLiteral("25") || rate == QStringLiteral("30000-1001") || rate == QStringLiteral("60")) {
                request.frameRateNumerator = rate == QStringLiteral("25") ? 30 : rate == QStringLiteral("60") ? 24 : 25;
                request.frameRateDenominator = 1;
                request.sequenceName = stem + QStringLiteral("-to-%1").arg(request.frameRateNumerator);
                QVERIFY2(XmemlExporter::save(QDir(directory).filePath(request.sequenceName + QStringLiteral(".xml")), request, &error), qPrintable(error));
            }
        }
    }
}

QTEST_APPLESS_MAIN(RoughCutXmlTests)

#include "test_roughcut_xml.moc"
