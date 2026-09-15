#include "media/media_probe.h"
#include "roughcut/xmeml_exporter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTextStream>

#include <cmath>
#include <variant>

using namespace subcue;

namespace {

constexpr int kFixtureSampleRate = 48'000;
constexpr int kFixtureSectionSeconds = 3;

void printUsage()
{
    QTextStream(stderr) << "用法：\n"
        << "  SubCueRoughCutXml <片段清单.json> <输出.xml>\n"
        << "  SubCueRoughCutXml --create-acceptance-fixture <输出目录>\n";
}

bool writeFixtureWav(const QString &path, QString *errorMessage)
{
    constexpr int sectionCount = 6;
    constexpr int sampleCount = kFixtureSampleRate * kFixtureSectionSeconds * sectionCount;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *errorMessage = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + sampleCount * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(kFixtureSampleRate)
           << quint32(kFixtureSampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(sampleCount * 2);
    const double frequencies[sectionCount] = {440.0, 523.25, 659.25, 783.99, 587.33, 880.0};
    for (int index = 0; index < sampleCount; ++index) {
        const int withinSection = index % (kFixtureSampleRate * kFixtureSectionSeconds);
        const int section = index / (kFixtureSampleRate * kFixtureSectionSeconds);
        const bool audible = withinSection < kFixtureSampleRate * 2;
        const double envelope = audible && withinSection < kFixtureSampleRate / 50
            ? static_cast<double>(withinSection) / (kFixtureSampleRate / 50) : 1.0;
        const qint16 sample = audible ? static_cast<qint16>(
            std::sin(2.0 * 3.14159265358979323846 * frequencies[section]
                     * withinSection / kFixtureSampleRate) * 9000.0 * envelope) : 0;
        stream << sample;
    }
    if (stream.status() != QDataStream::Ok || !file.commit()) {
        *errorMessage = file.errorString();
        return false;
    }
    return true;
}

QJsonObject fixtureManifest(const QString &wavPath)
{
    const auto sample = [](int second) { return static_cast<qint64>(second) * kFixtureSampleRate; };
    return {{QStringLiteral("sequenceName"), QStringLiteral("SubCue Phase 1 Acceptance")},
            {QStringLiteral("mediaPath"), QFileInfo(wavPath).fileName()},
            {QStringLiteral("frameRate"), 60},
            {QStringLiteral("clips"), QJsonArray{
                QJsonObject{{QStringLiteral("name"), QStringLiteral("A")},
                            {QStringLiteral("sourceStartSample"), sample(0)},
                            {QStringLiteral("sourceEndSample"), sample(3)}},
                QJsonObject{{QStringLiteral("name"), QStringLiteral("B2")},
                            {QStringLiteral("sourceStartSample"), sample(6)},
                            {QStringLiteral("sourceEndSample"), sample(9)}},
                QJsonObject{{QStringLiteral("name"), QStringLiteral("C")},
                            {QStringLiteral("sourceStartSample"), sample(9)},
                            {QStringLiteral("sourceEndSample"), sample(12)}},
                QJsonObject{{QStringLiteral("name"), QStringLiteral("D2")},
                            {QStringLiteral("sourceStartSample"), sample(15)},
                            {QStringLiteral("sourceEndSample"), sample(18)}}}}};
}

bool parseManifest(const QString &path, RoughCutExportRequest *request, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *errorMessage = QStringLiteral("片段清单不是有效 JSON：%1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    request->sequenceName = root.value(QStringLiteral("sequenceName"))
        .toString(QStringLiteral("SubCue Rough Cut"));
    QString mediaPath = root.value(QStringLiteral("mediaPath")).toString();
    if (QDir::isRelativePath(mediaPath)) mediaPath = QFileInfo(path).absoluteDir().filePath(mediaPath);
    request->mediaPath = QFileInfo(mediaPath).absoluteFilePath();
    request->frameRateNumerator = root.value(QStringLiteral("frameRate")).toInt(60);
    request->frameRateDenominator = 1;
    for (const QJsonValue &value : root.value(QStringLiteral("clips")).toArray()) {
        const QJsonObject clip = value.toObject();
        request->clips.append({clip.value(QStringLiteral("name")).toString(),
            clip.value(QStringLiteral("sourceStartSample")).toInteger(-1),
            clip.value(QStringLiteral("sourceEndSample")).toInteger(-1)});
    }
    const ProbeResult probed = MediaProbe::probe(request->mediaPath);
    if (std::holds_alternative<AppError>(probed)) {
        *errorMessage = std::get<AppError>(probed).userMessage();
        return false;
    }
    const MediaInfo &media = std::get<MediaInfo>(probed);
    for (const MediaStreamInfo &stream : media.streams) {
        if (stream.index != media.audioStreamIndex) continue;
        request->sampleRate = stream.sampleRate;
        request->channels = stream.channels;
        const MediaTime duration = stream.duration.isValidRange() ? stream.duration : media.duration;
        request->sourceSampleCount = qRound64(duration.seconds() * stream.sampleRate);
        break;
    }
    return true;
}

bool exportManifest(const QString &manifestPath, const QString &xmlPath, QString *errorMessage)
{
    RoughCutExportRequest request;
    return parseManifest(manifestPath, &request, errorMessage)
        && XmemlExporter::save(xmlPath, request, errorMessage);
}

bool createFixture(const QString &directoryPath, QString *errorMessage)
{
    QDir directory(directoryPath);
    if (!directory.mkpath(QStringLiteral("."))) {
        *errorMessage = QStringLiteral("无法创建验收目录：%1").arg(directoryPath);
        return false;
    }
    const QString wavPath = directory.filePath(QStringLiteral("phase1_source.wav"));
    const QString manifestPath = directory.filePath(QStringLiteral("phase1_clips.json"));
    const QString xmlPath = directory.filePath(QStringLiteral("roughcut.xml"));
    if (!writeFixtureWav(wavPath, errorMessage)) return false;
    QSaveFile manifest(manifestPath);
    if (!manifest.open(QIODevice::WriteOnly)
        || manifest.write(QJsonDocument(fixtureManifest(wavPath)).toJson(QJsonDocument::Indented)) < 0
        || !manifest.commit()) {
        *errorMessage = manifest.errorString();
        return false;
    }
    return exportManifest(manifestPath, xmlPath, errorMessage);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    QString errorMessage;
    bool ok = false;
    if (arguments.size() == 3 && arguments.at(1) == QLatin1String("--create-acceptance-fixture")) {
        ok = createFixture(arguments.at(2), &errorMessage);
    } else if (arguments.size() == 3) {
        ok = exportManifest(arguments.at(1), arguments.at(2), &errorMessage);
    } else {
        printUsage();
        return 2;
    }
    if (!ok) {
        QTextStream(stderr) << errorMessage << '\n';
        return 1;
    }
    QTextStream(stdout) << "完成。\n";
    return 0;
}
