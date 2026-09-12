#include "media/media_probe.h"

#include "media/ffmpeg_error.h"
#include "media/ffmpeg_raii.h"
#include "media/ffmpeg_time.h"

extern "C" {
#include <libavutil/dict.h>
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
        streamInfo.variableFrameRate = streamInfo.type == MediaStreamType::Video
            && streamInfo.averageFrameRate > 0.0
            && streamInfo.realFrameRate > 0.0
            && std::abs(streamInfo.averageFrameRate - streamInfo.realFrameRate) > 0.01;
        info.variableFrameRate = info.variableFrameRate || streamInfo.variableFrameRate;
        info.streams.push_back(std::move(streamInfo));
    }
    return info;
}

} // namespace subcue
