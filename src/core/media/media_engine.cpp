#include "media/media_engine.h"

#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"
#include "media/media_probe.h"
#include "media/video_frame_converter.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace subcue {
namespace {

AppError endOfStreamError(QString message)
{
    return AppError(ErrorDomain::Decoder, AVERROR_EOF, std::move(message), ffmpegErrorString(AVERROR_EOF));
}

} // namespace

ProbeResult MediaEngine::probe(const QString &path) const
{
    return MediaProbe::probe(path);
}

VideoFrameResult MediaEngine::decodeFirstVideoFrame(const QString &path, QSize targetSize) const
{
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(path, &error)) {
        return error;
    }
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_VIDEO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream) {
        return makeFfmpegError(
            ErrorDomain::Decoder,
            streamIndex,
            QStringLiteral("媒体中没有可解码的视频流"),
            path);
    }

    Decoder decoder;
    if (!decoder.open(*stream, &error)) {
        return error;
    }
    FramePtr frame = makeFrame();
    if (!frame) {
        return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频帧"));
    }
    VideoFrameConverter converter;

    while (PacketPtr packet = demuxer.readPacket(&error)) {
        if (packet->stream_index != streamIndex) {
            continue;
        }
        int result = decoder.send(packet.get());
        if (result < 0 && result != AVERROR(EAGAIN)) {
            return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("提交视频数据失败"));
        }
        while (result >= 0) {
            result = decoder.receive(frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("视频解码失败"));
            }
            QImage image = converter.convert(*frame, targetSize, &error);
            av_frame_unref(frame.get());
            if (!image.isNull()) {
                return image;
            }
            return error;
        }
    }
    return endOfStreamError(QStringLiteral("媒体中没有可显示的视频帧"));
}

AudioBufferResult MediaEngine::decodeAudio(
    const QString &path,
    MediaTime maximumDuration,
    int outputSampleRate,
    int outputChannels) const
{
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(path, &error)) {
        return error;
    }
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream) {
        return makeFfmpegError(
            ErrorDomain::Decoder,
            streamIndex,
            QStringLiteral("媒体中没有可解码的音频流"),
            path);
    }

    Decoder decoder;
    if (!decoder.open(*stream, &error)) {
        return error;
    }
    AudioResampler resampler;
    if (!resampler.configure(*decoder.context(), outputSampleRate, outputChannels, &error)) {
        return error;
    }
    FramePtr frame = makeFrame();
    if (!frame) {
        return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配音频帧"));
    }

    AudioBuffer audio;
    audio.sampleRate = outputSampleRate;
    audio.channels = outputChannels;
    const qint64 maximumValues = maximumDuration.microseconds() <= 0
        ? std::numeric_limits<qint64>::max()
        : av_rescale_q(
            maximumDuration.microseconds(),
            kMicrosecondTimeBase,
            AVRational{1, outputSampleRate * outputChannels});

    while (audio.samples.size() < maximumValues) {
        PacketPtr packet = demuxer.readPacket(&error);
        if (!packet) {
            break;
        }
        if (packet->stream_index != streamIndex) {
            continue;
        }
        int result = decoder.send(packet.get());
        if (result < 0 && result != AVERROR(EAGAIN)) {
            return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("提交音频数据失败"));
        }
        while (result >= 0 && audio.samples.size() < maximumValues) {
            result = decoder.receive(frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("音频解码失败"));
            }
            if (audio.startTime.microseconds() < 0) {
                audio.startTime = mediaTimeFromTimestamp(frame->best_effort_timestamp, stream->time_base);
            }
            QVector<float> samples = resampler.convert(*frame, &error);
            av_frame_unref(frame.get());
            if (samples.isEmpty()) {
                return error;
            }
            const qsizetype remaining = static_cast<qsizetype>(std::min<qint64>(
                maximumValues - audio.samples.size(), samples.size()));
            audio.samples.append(samples.cbegin(), samples.cbegin() + remaining);
        }
    }
    if (audio.samples.isEmpty()) {
        return endOfStreamError(QStringLiteral("媒体中没有可解码的音频样本"));
    }
    return audio;
}

} // namespace subcue
