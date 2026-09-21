#include "roughcut/xmeml_exporter.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamWriter>

#include <algorithm>

namespace subcue {
namespace {

qint64 floorFrame(qint64 sample, const RoughCutExportRequest &request)
{
    return sample * request.frameRateNumerator
        / (static_cast<qint64>(request.sampleRate) * request.frameRateDenominator);
}

qint64 ceilFrame(qint64 sample, const RoughCutExportRequest &request)
{
    const qint64 denominator = static_cast<qint64>(request.sampleRate)
        * request.frameRateDenominator;
    return (sample * request.frameRateNumerator + denominator - 1) / denominator;
}

void writeTextElement(QXmlStreamWriter &xml, const QString &name, const QString &value)
{
    xml.writeTextElement(name, value);
}

void writeRate(QXmlStreamWriter &xml, const RoughCutExportRequest &request)
{
    xml.writeStartElement(QStringLiteral("rate"));
    writeTextElement(xml, QStringLiteral("timebase"),
                     QString::number(request.frameRateNumerator / request.frameRateDenominator));
    writeTextElement(xml, QStringLiteral("ntsc"), QStringLiteral("FALSE"));
    xml.writeEndElement();
}

void writeFile(QXmlStreamWriter &xml, const RoughCutExportRequest &request,
               qint64 sourceDurationFrames, bool includeDetails)
{
    xml.writeStartElement(QStringLiteral("file"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("file-1"));
    if (includeDetails) {
        const QFileInfo media(request.mediaPath);
        writeTextElement(xml, QStringLiteral("name"), media.fileName());
        writeTextElement(xml, QStringLiteral("pathurl"),
                         QString::fromUtf8(QUrl::fromLocalFile(media.absoluteFilePath()).toEncoded()));
        writeRate(xml, request);
        writeTextElement(xml, QStringLiteral("duration"), QString::number(sourceDurationFrames));
        xml.writeStartElement(QStringLiteral("media"));
        xml.writeStartElement(QStringLiteral("audio"));
        xml.writeStartElement(QStringLiteral("samplecharacteristics"));
        writeTextElement(xml, QStringLiteral("samplerate"), QString::number(request.sampleRate));
        xml.writeEndElement();
        writeTextElement(xml, QStringLiteral("channelcount"), QString::number(request.channels));
        xml.writeEndElement();
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

void writeClipItem(QXmlStreamWriter &xml, const RoughCutExportRequest &request,
                   const RoughCutSourceClip &clip, const QString &id, int channel,
                   qint64 timelineStart, qint64 sourceDurationFrames, bool enabled,
                   bool includeFileDetails, int clipIndex, bool reference)
{
    const qint64 sourceIn = floorFrame(clip.sourceStartSample, request);
    const qint64 sourceOut = ceilFrame(clip.sourceEndSample, request);
    const qint64 duration = sourceOut - sourceIn;
    xml.writeStartElement(QStringLiteral("clipitem"));
    xml.writeAttribute(QStringLiteral("id"), id);
    if (request.channels == 2)
        xml.writeAttribute(QStringLiteral("premiereChannelType"), QStringLiteral("stereo"));
    writeTextElement(xml, QStringLiteral("name"), clip.name);
    writeTextElement(xml, QStringLiteral("duration"), QString::number(sourceDurationFrames));
    writeRate(xml, request);
    writeTextElement(xml, QStringLiteral("enabled"), enabled ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
    writeTextElement(xml, QStringLiteral("in"), QString::number(sourceIn));
    writeTextElement(xml, QStringLiteral("out"), QString::number(sourceOut));
    writeTextElement(xml, QStringLiteral("start"), QString::number(timelineStart));
    writeTextElement(xml, QStringLiteral("end"), QString::number(timelineStart + duration));
    writeFile(xml, request, sourceDurationFrames, includeFileDetails);
    xml.writeStartElement(QStringLiteral("sourcetrack"));
    writeTextElement(xml, QStringLiteral("mediatype"), QStringLiteral("audio"));
    writeTextElement(xml, QStringLiteral("trackindex"), QString::number(channel + 1));
    xml.writeEndElement();
    for (int linkedChannel = 0; linkedChannel < request.channels; ++linkedChannel) {
        xml.writeStartElement(QStringLiteral("link"));
        writeTextElement(xml, QStringLiteral("linkclipref"), reference
            ? QStringLiteral("reference-%1").arg(linkedChannel + 1)
            : QStringLiteral("rough-%1-%2").arg(linkedChannel + 1).arg(clipIndex + 1));
        writeTextElement(xml, QStringLiteral("mediatype"), QStringLiteral("audio"));
        writeTextElement(xml, QStringLiteral("trackindex"),
                         QString::number((reference ? request.channels : 0) + linkedChannel + 1));
        writeTextElement(xml, QStringLiteral("clipindex"), QString::number(clipIndex + 1));
        writeTextElement(xml, QStringLiteral("groupindex"), QStringLiteral("1"));
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

} // namespace

bool XmemlExporter::validate(const RoughCutExportRequest &request, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) *errorMessage = message;
        return false;
    };
    if (!QFileInfo::exists(request.mediaPath)) return fail(QStringLiteral("源音频不存在：%1").arg(request.mediaPath));
    if (request.sampleRate <= 0 || request.channels <= 0 || request.sourceSampleCount <= 0)
        return fail(QStringLiteral("源音频参数无效。"));
    if (request.frameRateNumerator <= 0 || request.frameRateDenominator <= 0
        || request.frameRateNumerator % request.frameRateDenominator != 0)
        return fail(QStringLiteral("Phase 1 仅支持整数序列帧率。"));
    if (request.clips.isEmpty()) return fail(QStringLiteral("粗剪片段列表为空。"));
    for (const RoughCutSourceClip &clip : request.clips) {
        if (clip.sourceStartSample < 0 || clip.sourceEndSample <= clip.sourceStartSample
            || clip.sourceEndSample > request.sourceSampleCount)
            return fail(QStringLiteral("片段 %1 的源采样区间无效。").arg(clip.name));
        if (ceilFrame(clip.sourceEndSample, request) <= floorFrame(clip.sourceStartSample, request))
            return fail(QStringLiteral("片段 %1 在当前帧率下长度为零。").arg(clip.name));
    }
    return true;
}

QByteArray XmemlExporter::build(const RoughCutExportRequest &request)
{
    QByteArray output;
    QXmlStreamWriter xml(&output);
    xml.setAutoFormatting(true);
    xml.writeStartDocument(QStringLiteral("1.0"));
    xml.writeStartElement(QStringLiteral("xmeml"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("5"));
    xml.writeStartElement(QStringLiteral("sequence"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("sequence-1"));
    writeTextElement(xml, QStringLiteral("name"), request.sequenceName);
    qint64 timelineDuration = 0;
    qint64 sequentialStart = 0;
    for (const RoughCutSourceClip &clip : request.clips) {
        const qint64 start = clip.timelineStartSample >= 0
            ? floorFrame(clip.timelineStartSample, request) : sequentialStart;
        const qint64 duration = ceilFrame(clip.sourceEndSample, request)
            - floorFrame(clip.sourceStartSample, request);
        timelineDuration = std::max(timelineDuration, start + duration);
        sequentialStart = start + duration;
    }
    writeTextElement(xml, QStringLiteral("duration"), QString::number(timelineDuration));
    writeRate(xml, request);
    xml.writeStartElement(QStringLiteral("media"));
    xml.writeStartElement(QStringLiteral("audio"));
    xml.writeStartElement(QStringLiteral("format"));
    xml.writeStartElement(QStringLiteral("samplecharacteristics"));
    writeTextElement(xml, QStringLiteral("samplerate"), QString::number(request.sampleRate));
    xml.writeEndElement();
    xml.writeEndElement();
    writeTextElement(xml, QStringLiteral("numOutputChannels"), QString::number(request.channels));

    const qint64 sourceDurationFrames = ceilFrame(request.sourceSampleCount, request);
    bool includeFileDetails = true;
    for (int channel = 0; channel < request.channels; ++channel) {
        xml.writeStartElement(QStringLiteral("track"));
        if (request.channels == 2) {
            xml.writeAttribute(QStringLiteral("currentExplodedTrackIndex"), QString::number(channel));
            xml.writeAttribute(QStringLiteral("totalExplodedTrackCount"), QStringLiteral("2"));
            xml.writeAttribute(QStringLiteral("premiereTrackType"), QStringLiteral("Stereo"));
        }
        qint64 timelineStart = 0;
        for (qsizetype index = 0; index < request.clips.size(); ++index) {
            const RoughCutSourceClip &clip = request.clips.at(index);
            const qint64 clipTimelineStart = clip.timelineStartSample >= 0
                ? floorFrame(clip.timelineStartSample, request) : timelineStart;
            writeClipItem(xml, request, clip,
                QStringLiteral("rough-%1-%2").arg(channel + 1).arg(index + 1), channel,
                clipTimelineStart, sourceDurationFrames, true, includeFileDetails,
                static_cast<int>(index), false);
            includeFileDetails = false;
            timelineStart = clipTimelineStart + ceilFrame(clip.sourceEndSample, request)
                - floorFrame(clip.sourceStartSample, request);
        }
        writeTextElement(xml, QStringLiteral("enabled"), QStringLiteral("TRUE"));
        writeTextElement(xml, QStringLiteral("locked"), QStringLiteral("FALSE"));
        if (request.channels == 2)
            writeTextElement(xml, QStringLiteral("outputchannelindex"), QString::number(channel + 1));
        xml.writeEndElement();
    }
    for (int channel = 0; channel < request.channels; ++channel) {
        xml.writeStartElement(QStringLiteral("track"));
        if (request.channels == 2) {
            xml.writeAttribute(QStringLiteral("currentExplodedTrackIndex"), QString::number(channel));
            xml.writeAttribute(QStringLiteral("totalExplodedTrackCount"), QStringLiteral("2"));
            xml.writeAttribute(QStringLiteral("premiereTrackType"), QStringLiteral("Stereo"));
        }
        const RoughCutSourceClip reference{QStringLiteral("ORIGINAL REFERENCE"), 0, request.sourceSampleCount};
        writeClipItem(xml, request, reference, QStringLiteral("reference-%1").arg(channel + 1), channel,
                      0, sourceDurationFrames, false, false, 0, true);
        writeTextElement(xml, QStringLiteral("enabled"), QStringLiteral("FALSE"));
        writeTextElement(xml, QStringLiteral("locked"), QStringLiteral("FALSE"));
        if (request.channels == 2)
            writeTextElement(xml, QStringLiteral("outputchannelindex"), QString::number(channel + 1));
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return output;
}

bool XmemlExporter::save(const QString &path, const RoughCutExportRequest &request,
                         QString *errorMessage)
{
    if (!validate(request, errorMessage)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(build(request)) < 0 || !file.commit()) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    return true;
}

} // namespace subcue
