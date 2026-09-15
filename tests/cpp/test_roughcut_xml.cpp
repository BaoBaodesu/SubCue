#include "roughcut/xmeml_exporter.h"

#include <QtCore/QFileInfo>
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
    void roundsSourceRangesOutward();
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
    QCOMPARE(fileReferences, 5);
    QCOMPARE(roughItems, 4);
    QCOMPARE(referenceItems, 2);
    QCOMPARE(data.count(QUrl::fromLocalFile(QFileInfo(media.fileName()).absoluteFilePath()).toEncoded()), 1);
    QCOMPARE(data.count("<enabled>FALSE</enabled>"), 4);
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

QTEST_APPLESS_MAIN(RoughCutXmlTests)

#include "test_roughcut_xml.moc"
