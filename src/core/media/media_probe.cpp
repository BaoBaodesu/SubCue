#include "media/media_probe.h"

#include "media/ffmpeg_error.h"
#include "media/ffmpeg_raii.h"
#include "media/ffmpeg_time.h"
#include "media/demuxer.h"
#include "media/decoder.h"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/display.h>
}

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <cmath>
#include <utility>

namespace subcue {
namespace {

MediaStreamType streamType(AVMediaType type)
{
    switch (type) {
    case AVMEDIA_TYPE_VIDEO:
        return MediaStreamType::Video;
    case AVMEDIA_TYPE_AUDIO:
        return MediaStreamType::Audio;
    case AVMEDIA_TYPE_SUBTITLE:
        return MediaStreamType::Subtitle;
    default:
        return MediaStreamType::Unknown;
    }
}

double rationalValue(AVRational value)
{
    return value.num > 0 && value.den > 0 ? av_q2d(value) : 0.0;
}

} // namespace

ProbeResult MediaProbe::probe(const QString &path)
{
    if (!QFileInfo::exists(path)) {
        return AppError(ErrorDomain::Media, AVERROR(ENOENT), QStringLiteral("媒体文件不存在"), path);
    }

    AVFormatContext *rawContext = nullptr;
    const QByteArray nativePath = path.toUtf8();
    int result = avformat_open_input(&rawContext, nativePath.constData(), nullptr, nullptr);
    if (result < 0) {
        return makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("无法打开媒体文件"), path);
    }
    FormatContextPtr context(rawContext);
    result = avformat_find_stream_info(context.get(), nullptr);
    if (result < 0) {
        return makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("无法读取媒体信息"), path);
    }

    MediaInfo info;
    info.path = QFileInfo(path).absoluteFilePath();
    if (context->iformat && context->iformat->name) {
        info.formatName = QString::fromUtf8(context->iformat->name);
    }
    if (context->duration != AV_NOPTS_VALUE) {
        info.duration = mediaTimeFromTimestamp(context->duration, AV_TIME_BASE_Q);
    }
    info.videoStreamIndex = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    info.audioStreamIndex = av_find_best_stream(context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    info.streams.reserve(static_cast<qsizetype>(context->nb_streams));
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        const AVStream *stream = context->streams[index];
        const AVCodecParameters *parameters = stream->codecpar;
        MediaStreamInfo streamInfo;
        streamInfo.index = static_cast<int>(index);
        streamInfo.type = streamType(parameters->codec_type);
        streamInfo.codecName = QString::fromUtf8(avcodec_get_name(parameters->codec_id));
        streamInfo.timeBaseNumerator = stream->time_base.num;
        streamInfo.timeBaseDenominator = stream->time_base.den;
        if (stream->duration != AV_NOPTS_VALUE) {
            streamInfo.duration = mediaTimeFromTimestamp(stream->duration, stream->time_base);
        }
        if (const AVDictionaryEntry *language = av_dict_get(stream->metadata, "language", nullptr, 0)) {
            streamInfo.language = QString::fromUtf8(language->value);
        }
        streamInfo.width = parameters->width;
        streamInfo.height = parameters->height;
        streamInfo.sampleRate = parameters->sample_rate;
        streamInfo.channels = parameters->ch_layout.nb_channels;
        streamInfo.averageFrameRate = rationalValue(stream->avg_frame_rate);
        streamInfo.realFrameRate = rationalValue(stream->r_frame_rate);
        const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
        streamInfo.frameRateNumerator = rate.num;
        streamInfo.frameRateDenominator = rate.den > 0 ? rate.den : 1;
        streamInfo.startTime = stream->start_time == AV_NOPTS_VALUE ? MediaTime{}
            : mediaTimeFromTimestamp(stream->start_time, stream->time_base);
        if (parameters->sample_aspect_ratio.num > 0 && parameters->sample_aspect_ratio.den > 0) {
            streamInfo.pixelAspectNumerator = parameters->sample_aspect_ratio.num;
            streamInfo.pixelAspectDenominator = parameters->sample_aspect_ratio.den;
        }
        streamInfo.variableFrameRate = streamInfo.type == MediaStreamType::Video
            && streamInfo.averageFrameRate > 0.0
            && streamInfo.realFrameRate > 0.0
            && std::abs(streamInfo.averageFrameRate - streamInfo.realFrameRate) > 0.01;
        info.variableFrameRate = info.variableFrameRate || streamInfo.variableFrameRate;
        info.streams.push_back(std::move(streamInfo));
    }
    return info;
}

ProbeResult MediaProbe::verifyFrameRate(const QString &path, const std::atomic<bool> *cancel)
{
    ProbeResult result = probe(path);
    if (std::holds_alternative<AppError>(result)) return result;
    MediaInfo info = std::get<MediaInfo>(result);
    if (info.videoStreamIndex < 0) return info;
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    Decoder decoder;
    if (!demuxer.open(path, &error)) return error;
    const AVStream *stream = demuxer.stream(info.videoStreamIndex);
    if (!stream || !decoder.open(*stream, &error)) return error;
    if (stream->codecpar->field_order > AV_FIELD_PROGRESSIVE)
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("暂不支持隔行视频 XML，请先转换为逐行固定帧率素材。"));
    const auto *display = av_packet_side_data_get(stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (display && display->size >= 9 * sizeof(int32_t)
        && std::abs(std::remainder(av_display_rotation_get(reinterpret_cast<const int32_t *>(display->data)), 360.0)) > 0.01)
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("暂不支持带旋转元数据的视频 XML，请先转换为正确方向的素材。"));
    const auto &video = info.streams.at(info.videoStreamIndex);
    if (video.frameRateNumerator <= 0 || video.frameRateDenominator <= 0)
        return AppError(ErrorDomain::Validation, 1, QStringLiteral("无法确认源视频帧率。"));
    const AVRational period{video.frameRateDenominator, video.frameRateNumerator};
    FramePtr frame = makeFrame();
    qint64 first = AV_NOPTS_VALUE;
    qint64 count = 0;
    // 完整扫描只保留时间戳与计数，不保留帧历史。
    bool draining = false;
    while (!cancel || !cancel->load()) {
        if (!draining) {
            PacketPtr packet = demuxer.readPacket(&error);
            if (!packet) { if (error.code() != 0) return error; draining = true; (void)decoder.send(nullptr); }
            else {
                if (packet->stream_index != info.videoStreamIndex) continue;
                const int sent = decoder.send(packet.get());
                if (sent < 0) return makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("帧率检查解码失败"));
            }
        }
        for (;;) {
            const int received = decoder.receive(frame.get());
            if (received == AVERROR(EAGAIN)) break;
            if (received == AVERROR_EOF) {
                if (count <= 1) return AppError(ErrorDomain::Validation, 1, QStringLiteral("视频帧数不足，无法确认固定帧率。"));
                info.cfrVerified = true;
                info.variableFrameRate = false;
                return info;
            }
            if (received < 0) return makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("帧率检查失败"));
            if (frame->flags & AV_FRAME_FLAG_INTERLACED)
                return AppError(ErrorDomain::Validation, 1, QStringLiteral("暂不支持隔行视频 XML，请先转换为逐行素材。"));
            const qint64 pts = frame->best_effort_timestamp;
            av_frame_unref(frame.get());
            if (pts == AV_NOPTS_VALUE)
                return AppError(ErrorDomain::Validation, 1, QStringLiteral("视频缺少时间戳，无法确认固定帧率。"));
            if (first == AV_NOPTS_VALUE) first = pts;
            if (std::abs(pts - first - av_rescale_q(count, period, stream->time_base)) > 1) {
                info.variableFrameRate = true;
                info.cfrVerified = false;
                return info;
            }
            ++count;
        }
        if (draining) break;
    }
    return AppError(ErrorDomain::Media, AVERROR_EXIT, QStringLiteral("帧率检查已取消。"));
}

} // namespace subcue
