#include "roughcut/xmeml_exporter.h"
#include "common/file_path_guard.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamWriter>
extern "C" {
#include <libavutil/mathematics.h>
}
#include <algorithm>

namespace subcue {
namespace {

struct ExportClip final {
    qint64 in = 0;
    qint64 out = 0;
    qint64 start = 0;
    qint64 end = 0;
};

bool supportedRate(int n, int d)
{
    return (d == 1 && (n == 24 || n == 25 || n == 30 || n == 50 || n == 60))
        || (d == 1001 && (n == 24000 || n == 30000 || n == 60000));
}

const MediaStreamInfo *sourceVideo(const RoughCutExportRequest &r)
{
    return r.mediaInfo.videoStreamIndex >= 0 && r.mediaInfo.videoStreamIndex < r.mediaInfo.streams.size()
        ? &r.mediaInfo.streams.at(r.mediaInfo.videoStreamIndex) : nullptr;
}

qint64 frameAt(qint64 sample, int sampleRate, int n, int d, AVRounding rounding)
{
    return av_rescale_rnd(sample, n, qint64(sampleRate) * d, rounding);
}

void writeRate(QXmlStreamWriter &xml, int n, int d)
{
    xml.writeStartElement(QStringLiteral("rate"));
    xml.writeTextElement(QStringLiteral("timebase"), QString::number(d == 1001 ? n / 1000 : n));
    xml.writeTextElement(QStringLiteral("ntsc"), d == 1001 ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
    xml.writeEndElement();
}

QVector<ExportClip> exportClips(const RoughCutExportRequest &r, QString *error)
{
    QVector<ExportClip> result;
    const auto *video = sourceVideo(r);
    const int n = video ? video->frameRateNumerator : r.frameRateNumerator;
    const int d = video ? video->frameRateDenominator : r.frameRateDenominator;
    const qint64 origin = video ? av_rescale(video->startTime.microseconds(), r.sampleRate, 1'000'000) : 0;
    qint64 accumulated = 0, inputEnd = 0, outputEnd = 0;
    for (const auto &clip : r.clips) {
        const auto fail = [&](const QString &reason) {
            if (error) *error = QStringLiteral("片段“%1”：%2").arg(clip.name, reason);
            return QVector<ExportClip>{};
        };
        if (clip.sourceStartSample < origin || clip.sourceEndSample <= clip.sourceStartSample
            || clip.sourceEndSample > r.sourceSampleCount)
            return fail(QStringLiteral("源范围无效或超出视频范围。"));
        const qint64 allowedEnd = clip.allowedEndSample < 0 ? r.sourceSampleCount : clip.allowedEndSample;
        qint64 in = frameAt(clip.sourceStartSample - origin, r.sampleRate, n, d,
            r.sourceRangesAreFrameAligned ? AV_ROUND_NEAR_INF : AV_ROUND_DOWN);
        qint64 out = frameAt(clip.sourceEndSample - origin, r.sampleRate, n, d,
            r.sourceRangesAreFrameAligned ? AV_ROUND_NEAR_INF : AV_ROUND_UP);
        qint64 startSample = origin + av_rescale_rnd(in, qint64(r.sampleRate) * d, n, AV_ROUND_NEAR_INF);
        qint64 endSample = origin + av_rescale_rnd(out, qint64(r.sampleRate) * d, n, AV_ROUND_NEAR_INF);
        if (startSample < clip.allowedStartSample) {
            in = frameAt(clip.sourceStartSample - origin, r.sampleRate, n, d, AV_ROUND_UP);
            startSample = origin + av_rescale_rnd(in, qint64(r.sampleRate) * d, n, AV_ROUND_NEAR_INF);
        }
        if (endSample > allowedEnd) {
            out = frameAt(clip.sourceEndSample - origin, r.sampleRate, n, d, AV_ROUND_DOWN);
            endSample = origin + av_rescale_rnd(out, qint64(r.sampleRate) * d, n, AV_ROUND_NEAR_INF);
        }
        if (out <= in || startSample < clip.allowedStartSample || endSample > allowedEnd
            || (clip.protectedStartSample >= 0 && startSample > clip.protectedStartSample)
            || (clip.protectedEndSample >= 0 && endSample < clip.protectedEndSample))
            return fail(QStringLiteral("没有同时保护语音和剪除范围的安全帧，请调整边界。"));
        if (video && video->duration.microseconds() > 0
            && out > av_rescale_rnd(video->duration.microseconds(), n, qint64(1'000'000) * d, AV_ROUND_UP))
            return fail(QStringLiteral("超出源视频最后一帧。"));
        const qint64 inputStart = clip.timelineStartSample < 0 ? inputEnd : clip.timelineStartSample;
        if (inputStart < inputEnd) return fail(QStringLiteral("输入时间线片段重叠。"));
        accumulated += (inputStart - inputEnd) * n;
        const qint64 start = av_rescale_rnd(accumulated, r.frameRateNumerator,
            qint64(r.sampleRate) * n * r.frameRateDenominator, AV_ROUND_NEAR_INF);
        accumulated += (out - in) * r.sampleRate * d;
        const qint64 end = av_rescale_rnd(accumulated, r.frameRateNumerator,
            qint64(r.sampleRate) * n * r.frameRateDenominator, AV_ROUND_NEAR_INF);
        if (end <= start || start < outputEnd)
            return fail(QStringLiteral("当前序列帧率下片段长度为零或重叠，请提高帧率。"));
        const qint64 sequenceIn = av_rescale_rnd(in, qint64(r.frameRateNumerator) * d,
            qint64(n) * r.frameRateDenominator, AV_ROUND_NEAR_INF);
        const qint64 sequenceOut = av_rescale_rnd(out, qint64(r.frameRateNumerator) * d,
            qint64(n) * r.frameRateDenominator, AV_ROUND_NEAR_INF);
        const AVRational sequenceTime{r.frameRateDenominator, r.frameRateNumerator};
        const AVRational sampleTime{1, r.sampleRate};
        // 序列时间单位的转换也必须保护语音与 CUT，不能仅校验源帧对齐。
        if (sequenceOut <= sequenceIn
            || av_compare_ts(sequenceIn, sequenceTime, clip.allowedStartSample - origin, sampleTime) < 0
            || av_compare_ts(sequenceOut, sequenceTime, allowedEnd - origin, sampleTime) > 0
            || (clip.protectedStartSample >= 0
                && av_compare_ts(sequenceIn, sequenceTime, clip.protectedStartSample - origin, sampleTime) > 0)
            || (clip.protectedEndSample >= 0
                && av_compare_ts(std::min(sequenceOut, sequenceIn + end - start), sequenceTime,
                    clip.protectedEndSample - origin, sampleTime) < 0))
            return fail(QStringLiteral("序列帧率转换后无法保护语音或剪除范围，请调整边界或采用源帧率。"));
        result.append({in, out, start, end});
        outputEnd = end;
        inputEnd = inputStart + clip.sourceEndSample - clip.sourceStartSample;
    }
    return result;
}

void writeVideoFormat(QXmlStreamWriter &xml, const MediaStreamInfo &video, int n, int d)
{
    xml.writeStartElement(QStringLiteral("samplecharacteristics"));
    writeRate(xml, n, d);
    xml.writeTextElement(QStringLiteral("width"), QString::number(video.width));
    xml.writeTextElement(QStringLiteral("height"), QString::number(video.height));
    xml.writeTextElement(QStringLiteral("anamorphic"), QStringLiteral("FALSE"));
    xml.writeTextElement(QStringLiteral("pixelaspectratio"), QStringLiteral("square"));
    xml.writeTextElement(QStringLiteral("fielddominance"), QStringLiteral("none"));
    xml.writeEndElement();
}

void writeFile(QXmlStreamWriter &xml, const RoughCutExportRequest &r, bool details)
{
    xml.writeStartElement(QStringLiteral("file"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("file-1"));
    if (details) {
        const auto *video = sourceVideo(r);
        const int n = video ? video->frameRateNumerator : r.frameRateNumerator;
        const int d = video ? video->frameRateDenominator : r.frameRateDenominator;
        xml.writeTextElement(QStringLiteral("name"), QFileInfo(r.mediaPath).fileName());
        xml.writeTextElement(QStringLiteral("pathurl"), QString::fromUtf8(QUrl::fromLocalFile(QFileInfo(r.mediaPath).absoluteFilePath()).toEncoded()));
        writeRate(xml, n, d);
        xml.writeTextElement(QStringLiteral("duration"), QString::number(video
            ? av_rescale_rnd(video->duration.microseconds(), n, qint64(1'000'000) * d, AV_ROUND_UP)
            : frameAt(r.sourceSampleCount, r.sampleRate, n, d, AV_ROUND_UP)));
        xml.writeStartElement(QStringLiteral("media"));
        if (video) {
            xml.writeStartElement(QStringLiteral("video"));
            writeVideoFormat(xml, *video, n, d);
            xml.writeEndElement();
        }
        xml.writeStartElement(QStringLiteral("audio"));
        xml.writeStartElement(QStringLiteral("samplecharacteristics"));
        xml.writeTextElement(QStringLiteral("depth"), QStringLiteral("16"));
        xml.writeTextElement(QStringLiteral("samplerate"), QString::number(r.sampleRate));
        xml.writeEndElement();
        xml.writeTextElement(QStringLiteral("channelcount"), QString::number(r.channels));
        xml.writeEndElement();
        xml.writeEndElement();
    }
    xml.writeEndElement();
}

void writeClip(QXmlStreamWriter &xml, const RoughCutExportRequest &r, int index, const ExportClip &clip, bool videoTrack, bool details, int channel = 1)
{
    const auto *video = sourceVideo(r);
    const qint64 in = video ? av_rescale_rnd(clip.in, qint64(r.frameRateNumerator) * video->frameRateDenominator,
        qint64(video->frameRateNumerator) * r.frameRateDenominator, AV_ROUND_NEAR_INF) : clip.in;
    const qint64 out = video ? av_rescale_rnd(clip.out, qint64(r.frameRateNumerator) * video->frameRateDenominator,
        qint64(video->frameRateNumerator) * r.frameRateDenominator, AV_ROUND_NEAR_INF) : clip.out;
    xml.writeStartElement(QStringLiteral("clipitem"));
    xml.writeAttribute(QStringLiteral("id"), (videoTrack ? QStringLiteral("video-%1")
        : channel == 1 ? QStringLiteral("rough-%1") : QStringLiteral("rough-%1-right")).arg(index + 1));
    if (!videoTrack && r.channels == 2) xml.writeAttribute(QStringLiteral("premiereChannelType"), QStringLiteral("stereo"));
    xml.writeTextElement(QStringLiteral("name"), r.clips.at(index).name);
    xml.writeTextElement(QStringLiteral("duration"), QString::number(video
        ? av_rescale_rnd(video->duration.microseconds(), r.frameRateNumerator,
            qint64(1'000'000) * r.frameRateDenominator, AV_ROUND_UP)
        : frameAt(r.sourceSampleCount, r.sampleRate, r.frameRateNumerator, r.frameRateDenominator, AV_ROUND_UP)));
    // Premiere 原生 XML 的 clipitem 使用序列时间单位，file 才保存源素材帧率。
    writeRate(xml, r.frameRateNumerator, r.frameRateDenominator);
    xml.writeTextElement(QStringLiteral("enabled"), QStringLiteral("TRUE"));
    xml.writeTextElement(QStringLiteral("in"), QString::number(in));
    xml.writeTextElement(QStringLiteral("out"), QString::number(out));
    // Premiere 2022 的混合帧率导入需要原生刻度，避免按序列帧率解释源 in/out。
    xml.writeTextElement(QStringLiteral("pproTicksIn"), QString::number(av_rescale(in,
        qint64(254'016'000'000) * r.frameRateDenominator, r.frameRateNumerator)));
    xml.writeTextElement(QStringLiteral("pproTicksOut"), QString::number(av_rescale(out,
        qint64(254'016'000'000) * r.frameRateDenominator, r.frameRateNumerator)));
    xml.writeTextElement(QStringLiteral("start"), QString::number(clip.start));
    xml.writeTextElement(QStringLiteral("end"), QString::number(clip.end));
    writeFile(xml, r, details);
    xml.writeStartElement(QStringLiteral("sourcetrack"));
    xml.writeTextElement(QStringLiteral("mediatype"), videoTrack ? QStringLiteral("video") : QStringLiteral("audio"));
    xml.writeTextElement(QStringLiteral("trackindex"), QString::number(videoTrack ? 1 : channel));
    xml.writeEndElement();
    if (video || r.channels == 2) {
        for (int linked = video ? 0 : 1; linked <= r.channels; ++linked) {
            const bool linkedVideo = linked == 0;
            xml.writeStartElement(QStringLiteral("link"));
            xml.writeTextElement(QStringLiteral("linkclipref"), (linkedVideo ? QStringLiteral("video-%1")
                : linked == 1 ? QStringLiteral("rough-%1") : QStringLiteral("rough-%1-right")).arg(index + 1));
            xml.writeTextElement(QStringLiteral("mediatype"), linkedVideo ? QStringLiteral("video") : QStringLiteral("audio"));
            xml.writeTextElement(QStringLiteral("trackindex"), QString::number(linkedVideo ? 1 : linked));
            xml.writeTextElement(QStringLiteral("clipindex"), QString::number(index + 1));
            if (!linkedVideo) xml.writeTextElement(QStringLiteral("groupindex"), QStringLiteral("1"));
            xml.writeEndElement();
        }
    }
    xml.writeEndElement();
}

} // namespace

bool XmemlExporter::validate(const RoughCutExportRequest &r, QString *error)
{
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (!QFileInfo::exists(r.mediaPath)) return fail(QStringLiteral("源素材不存在。"));
    if (r.sampleRate <= 0 || r.channels < 1 || r.channels > 2 || r.sourceSampleCount <= 0)
        return fail(QStringLiteral("源音频参数无效，XML 仅支持单声道或立体声。"));
    if (!supportedRate(r.frameRateNumerator, r.frameRateDenominator)) return fail(QStringLiteral("不支持的序列帧率。"));
    if (const auto *video = sourceVideo(r)) {
        if (!r.mediaInfo.cfrVerified || r.mediaInfo.variableFrameRate) return fail(QStringLiteral("源视频未确认固定帧率。"));
        if (!supportedRate(video->frameRateNumerator, video->frameRateDenominator) || video->width <= 0 || video->height <= 0
            || video->duration.microseconds() <= 0 || video->pixelAspectNumerator != video->pixelAspectDenominator)
            return fail(QStringLiteral("源视频帧率或像素格式尚未支持。"));
    }
    if (r.clips.isEmpty()) return fail(QStringLiteral("粗剪片段列表为空。"));
    return exportClips(r, error).size() == r.clips.size();
}

QList<RoughCutSourceClip> XmemlExporter::conformSourceRanges(const RoughCutExportRequest &r, QString *error)
{
    if (!validate(r, error)) return {};
    const auto *video = sourceVideo(r);
    if (!video) return r.clips;
    const auto frames = exportClips(r, error);
    auto clips = r.clips;
    const qint64 origin = av_rescale(video->startTime.microseconds(), r.sampleRate, 1'000'000);
    for (int i = 0; i < clips.size(); ++i) {
        clips[i].sourceStartSample = origin + av_rescale(frames[i].in,
            qint64(r.sampleRate) * video->frameRateDenominator, video->frameRateNumerator);
        clips[i].sourceEndSample = origin + av_rescale(frames[i].out,
            qint64(r.sampleRate) * video->frameRateDenominator, video->frameRateNumerator);
    }
    return clips;
}

QByteArray XmemlExporter::build(const RoughCutExportRequest &r)
{
    QString error;
    if (!validate(r, &error)) return {};
    const auto clips = exportClips(r, &error);
    QByteArray output;
    QXmlStreamWriter xml(&output);
    xml.setAutoFormatting(true);
    xml.writeStartDocument(QStringLiteral("1.0"));
    xml.writeDTD(QStringLiteral("<!DOCTYPE xmeml>"));
    xml.writeStartElement(QStringLiteral("xmeml"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("5"));
    xml.writeStartElement(QStringLiteral("sequence"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("sequence-1"));
    if (r.channels == 2) xml.writeAttribute(QStringLiteral("explodedTracks"), QStringLiteral("true"));
    xml.writeTextElement(QStringLiteral("name"), r.sequenceName);
    xml.writeTextElement(QStringLiteral("duration"), QString::number(clips.last().end));
    writeRate(xml, r.frameRateNumerator, r.frameRateDenominator);
    xml.writeStartElement(QStringLiteral("media"));
    bool details = true;
    if (const auto *video = sourceVideo(r)) {
        xml.writeStartElement(QStringLiteral("video"));
        xml.writeStartElement(QStringLiteral("format"));
        writeVideoFormat(xml, *video, r.frameRateNumerator, r.frameRateDenominator);
        xml.writeEndElement();
        xml.writeStartElement(QStringLiteral("track"));
        for (int i = 0; i < clips.size(); ++i) { writeClip(xml, r, i, clips[i], true, details); details = false; }
        xml.writeEndElement();
        xml.writeEndElement();
    }
    xml.writeStartElement(QStringLiteral("audio"));
    xml.writeTextElement(QStringLiteral("numOutputChannels"), QString::number(r.channels));
    xml.writeStartElement(QStringLiteral("format"));
    xml.writeStartElement(QStringLiteral("samplecharacteristics"));
    xml.writeTextElement(QStringLiteral("samplerate"), QString::number(r.sampleRate));
    xml.writeEndElement();
    xml.writeEndElement();
    // Premiere 的立体声 XML 使用两个展开声道轨，导入后合并成一个立体声轨。
    for (int channel = 1; channel <= r.channels; ++channel) {
        xml.writeStartElement(QStringLiteral("track"));
        if (r.channels == 2) {
            xml.writeAttribute(QStringLiteral("currentExplodedTrackIndex"), QString::number(channel - 1));
            xml.writeAttribute(QStringLiteral("totalExplodedTrackCount"), QStringLiteral("2"));
            xml.writeAttribute(QStringLiteral("premiereTrackType"), QStringLiteral("Stereo"));
        }
        for (int i = 0; i < clips.size(); ++i) { writeClip(xml, r, i, clips[i], false, details, channel); details = false; }
        xml.writeTextElement(QStringLiteral("enabled"), QStringLiteral("TRUE"));
        xml.writeTextElement(QStringLiteral("locked"), QStringLiteral("FALSE"));
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return output;
}

bool XmemlExporter::save(const QString &path, const RoughCutExportRequest &r, QString *error)
{
    if (!safeOutputPath(path, {r.mediaPath}, error) || !validate(r, error)) return false;
    const QByteArray data = build(r);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace subcue
