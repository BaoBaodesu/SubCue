#include "asr/audio_chunk_extractor.h"

#include "asr/asr_types.h"
#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_raii.h"
#include "media/ffmpeg_time.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace subcue {
namespace {

struct AvioBuffer final {
    QByteArray data;
    qint64 position = 0;
};

int writeAvio(void *opaque, const uint8_t *buffer, int size)
{
    auto *out = static_cast<AvioBuffer *>(opaque);
    if (out->position + size > out->data.size()) {
        out->data.resize(out->position + size);
    }
    std::memcpy(out->data.data() + out->position, buffer, size);
    out->position += size;
    return size;
}

int64_t seekAvio(void *opaque, int64_t offset, int whence)
{
    auto *out = static_cast<AvioBuffer *>(opaque);
    if (whence == AVSEEK_SIZE) return out->data.size();
    whence &= ~AVSEEK_FORCE;
    const int64_t base = whence == SEEK_SET ? 0
        : whence == SEEK_CUR ? out->position
        : whence == SEEK_END ? out->data.size() : -1;
    if (base < 0 || offset < -base || base + offset > out->data.size()) {
        return AVERROR(EINVAL);
    }
    out->position = base + offset;
    return out->position;
}

AppError cancelError()
{
    return asrCancelledError();
}

void trimConvertedSamples(
    qsizetype convertedCount,
    qint64 frameStartUs,
    const AudioChunkWindow &window,
    qsizetype &skipSamples,
    qsizetype &keepSamples)
{
    const qint64 frameDurationUs = convertedCount > 0
        ? av_rescale_q(static_cast<qint64>(convertedCount), AVRational{1, kAsrSampleRate}, kMicrosecondTimeBase)
        : 0;
    const qint64 frameEndUs = frameStartUs + frameDurationUs;
    const qint64 windowStartUs = static_cast<qint64>(window.startSeconds * 1'000'000.0);
    const qint64 windowEndUs = static_cast<qint64>(window.endSeconds * 1'000'000.0);
    if (convertedCount <= 0 || frameEndUs <= windowStartUs || frameStartUs >= windowEndUs) {
        skipSamples = convertedCount;
        keepSamples = 0;
        return;
    }
    skipSamples = 0;
    if (frameStartUs < windowStartUs) {
        skipSamples = static_cast<qsizetype>(
            av_rescale_q(windowStartUs - frameStartUs, kMicrosecondTimeBase, AVRational{1, kAsrSampleRate}));
        skipSamples = std::clamp<qsizetype>(skipSamples, 0, convertedCount);
    }
    qsizetype endSkip = 0;
    if (frameEndUs > windowEndUs) {
        endSkip = static_cast<qsizetype>(
            av_rescale_q(frameEndUs - windowEndUs, kMicrosecondTimeBase, AVRational{1, kAsrSampleRate}));
        endSkip = std::clamp<qsizetype>(endSkip, 0, convertedCount - skipSamples);
    }
    keepSamples = convertedCount - skipSamples - endSkip;
}

AVSampleFormat pickFlacSampleFormat(const AVCodec &codec)
{
    const void *configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, &codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) >= 0
        && configs) {
        const auto *formats = static_cast<const AVSampleFormat *>(configs);
        for (int index = 0; index < count; ++index) {
            if (formats[index] == AV_SAMPLE_FMT_S16) {
                return AV_SAMPLE_FMT_S16;
            }
        }
        if (count > 0 && formats[0] != AV_SAMPLE_FMT_NONE) {
            return formats[0];
        }
    }
    return AV_SAMPLE_FMT_S16;
}

} // namespace

MediaResult<QVector<float>> AudioChunkExtractor::decodePcm16kMono(
    const QString &mediaPath,
    const AudioChunkWindow &window,
    const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return cancelError();
    }
    if (window.endSeconds < window.startSeconds) {
        return AppError(ErrorDomain::Validation, static_cast<int>(AsrErrorCode::InvalidRequest),
            QStringLiteral("音频分片时间无效"));
    }

    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(mediaPath, &error)) {
        return error;
    }
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        return makeFfmpegError(
            ErrorDomain::Decoder,
            streamIndex,
            QStringLiteral("媒体中没有可解码的音频流"),
            mediaPath);
    }

    Decoder decoder;
    if (!decoder.open(*stream, &error)) {
        return error;
    }
    if (window.startSeconds > 0.0) {
        const qint64 timestamp = timestampFromMediaTime(
            MediaTime::fromMicroseconds(static_cast<qint64>(window.startSeconds * 1'000'000.0)),
            stream->time_base);
        if (!demuxer.seek(streamIndex, timestamp, &error)) {
            return error;
        }
        decoder.flush();
    }

    AudioResampler resampler;
    if (!resampler.configure(*decoder.context(), kAsrSampleRate, 1, &error)) {
        return error;
    }
    FramePtr frame = makeFrame();
    if (!frame) {
        return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配音频帧"));
    }

    QVector<float> samples;
    const qint64 windowEndUs = static_cast<qint64>(window.endSeconds * 1'000'000.0);
    while (!asrCancelled(cancel)) {
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
        while (result >= 0) {
            result = decoder.receive(frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("音频解码失败"));
            }
            const MediaTime frameStart = mediaTimeFromTimestamp(frame->best_effort_timestamp, stream->time_base);
            if (frameStart.microseconds() >= windowEndUs && window.endSeconds > window.startSeconds) {
                av_frame_unref(frame.get());
                return samples;
            }
            QVector<float> converted = resampler.convert(*frame, &error);
            const qint64 frameStartUs = frameStart.microseconds() >= 0 ? frameStart.microseconds() : 0;
            av_frame_unref(frame.get());
            if (converted.isEmpty()) {
                return error;
            }
            qsizetype skipSamples = 0;
            qsizetype keepSamples = 0;
            trimConvertedSamples(converted.size(), frameStartUs, window, skipSamples, keepSamples);
            if (keepSamples <= 0) {
                continue;
            }
            samples.append(converted.cbegin() + skipSamples, converted.cbegin() + skipSamples + keepSamples);
        }
    }
    if (asrCancelled(cancel)) {
        return cancelError();
    }
    return samples;
}

MediaResult<QByteArray> AudioChunkExtractor::encodeFlac(const QVector<float> &pcm16kMono)
{
    if (pcm16kMono.isEmpty()) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::InvalidRequest),
            QStringLiteral("没有可编码的音频样本"));
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) {
        return AppError(ErrorDomain::Asr, AVERROR_ENCODER_NOT_FOUND,
            QStringLiteral("当前 FFmpeg 构建未启用 FLAC 编码器"));
    }

    AVFormatContext *format = nullptr;
    int result = avformat_alloc_output_context2(&format, nullptr, "flac", nullptr);
    if (result < 0 || !format) {
        return makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("无法创建 FLAC 封装"));
    }

    AVStream *stream = avformat_new_stream(format, nullptr);
    CodecContextPtr encoder(avcodec_alloc_context3(codec));
    if (!stream || !encoder) {
        avformat_free_context(format);
        return AppError(ErrorDomain::Asr, AVERROR(ENOMEM), QStringLiteral("无法创建 FLAC 编码器"));
    }

    encoder->sample_rate = kAsrSampleRate;
    encoder->sample_fmt = pickFlacSampleFormat(*codec);
    av_channel_layout_default(&encoder->ch_layout, 1);
    encoder->time_base = AVRational{1, kAsrSampleRate};
    if (format->oformat && (format->oformat->flags & AVFMT_GLOBALHEADER)) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    result = avcodec_open2(encoder.get(), codec, nullptr);
    if (result < 0) {
        avformat_free_context(format);
        return makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("无法打开 FLAC 编码器"));
    }
    result = avcodec_parameters_from_context(stream->codecpar, encoder.get());
    if (result < 0) {
        avformat_free_context(format);
        return makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("无法写入 FLAC 流参数"));
    }
    stream->time_base = encoder->time_base;

    AvioBuffer buffer;
    unsigned char *avioMemory = static_cast<unsigned char *>(av_malloc(4096));
    if (!avioMemory) {
        avformat_free_context(format);
        return AppError(ErrorDomain::Asr, AVERROR(ENOMEM), QStringLiteral("无法分配 FLAC 缓冲"));
    }
    AVIOContext *avio = avio_alloc_context(avioMemory, 4096, 1, &buffer, nullptr, writeAvio, seekAvio);
    if (!avio) {
        av_free(avioMemory);
        avformat_free_context(format);
        return AppError(ErrorDomain::Asr, AVERROR(ENOMEM), QStringLiteral("无法创建 FLAC 输出"));
    }
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    result = avformat_write_header(format, nullptr);
    if (result < 0) {
        format->pb = nullptr;
        avformat_free_context(format);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("无法写入 FLAC 文件头"));
    }

    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 4096;
    FramePtr frame = makeFrame();
    PacketPtr packet = makePacket();
    if (!frame || !packet) {
        av_write_trailer(format);
        format->pb = nullptr;
        avformat_free_context(format);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return AppError(ErrorDomain::Asr, AVERROR(ENOMEM), QStringLiteral("无法分配 FLAC 帧"));
    }
    frame->format = encoder->sample_fmt;
    frame->sample_rate = encoder->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);

    auto failEncode = [&](AppError failed) -> MediaResult<QByteArray> {
        format->pb = nullptr;
        avformat_free_context(format);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return failed;
    };

    qint64 pts = 0;
    qsizetype offset = 0;
    while (offset < pcm16kMono.size()) {
        const int samples = static_cast<int>(
            std::min<qsizetype>(frameSize, pcm16kMono.size() - offset));
        frame->format = encoder->sample_fmt;
        frame->sample_rate = encoder->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
        frame->nb_samples = samples;
        result = av_frame_get_buffer(frame.get(), 0);
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("无法分配 FLAC 样本缓冲")));
        }
        const AVSampleFormat packed = av_get_packed_sample_fmt(encoder->sample_fmt);
        if (packed == AV_SAMPLE_FMT_S16) {
            auto *dst = reinterpret_cast<int16_t *>(frame->data[0]);
            for (int index = 0; index < samples; ++index) {
                const float value = std::clamp(pcm16kMono.at(offset + index), -1.0f, 1.0f);
                dst[index] = static_cast<int16_t>(
                    av_clip_int16(static_cast<int>(std::lrint(value * 32767.0f))));
            }
        } else if (packed == AV_SAMPLE_FMT_S32) {
            auto *dst = reinterpret_cast<int32_t *>(frame->data[0]);
            for (int index = 0; index < samples; ++index) {
                const float value = std::clamp(pcm16kMono.at(offset + index), -1.0f, 1.0f);
                dst[index] = av_clipl_int32(
                    static_cast<int64_t>(std::llround(static_cast<double>(value) * 2147483647.0)));
            }
        } else if (packed == AV_SAMPLE_FMT_FLT) {
            std::memcpy(frame->data[0], pcm16kMono.constData() + offset,
                static_cast<size_t>(samples) * sizeof(float));
        } else {
            return failEncode(AppError(ErrorDomain::Asr, AVERROR(EINVAL),
                QStringLiteral("不支持的 FLAC 采样格式")));
        }
        frame->pts = pts;
        pts += samples;
        offset += samples;

        result = avcodec_send_frame(encoder.get(), frame.get());
        av_frame_unref(frame.get());
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("提交 FLAC 帧失败")));
        }
        while (result >= 0) {
            result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("FLAC 编码失败")));
            }
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            result = av_interleaved_write_frame(format, packet.get());
            av_packet_unref(packet.get());
            if (result < 0) {
                return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("写入 FLAC 数据失败")));
            }
        }
    }

    result = avcodec_send_frame(encoder.get(), nullptr);
    if (result < 0) {
        return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("刷新 FLAC 编码器失败")));
    }
    while (result >= 0) {
        result = avcodec_receive_packet(encoder.get(), packet.get());
        if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
            break;
        }
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("FLAC 编码失败")));
        }
        av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        result = av_interleaved_write_frame(format, packet.get());
        av_packet_unref(packet.get());
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Asr, result, QStringLiteral("写入 FLAC 数据失败")));
        }
    }

    av_write_trailer(format);
    avio_flush(avio);
    QByteArray flac = std::move(buffer.data);
    format->pb = nullptr;
    avformat_free_context(format);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    if (!flac.startsWith("fLaC")) {
        return AppError(ErrorDomain::Asr, AVERROR_INVALIDDATA, QStringLiteral("FLAC 编码结果无效"));
    }
    return flac;
}

MediaResult<PreparedAudioChunk> AudioChunkExtractor::extract(
    const QString &mediaPath,
    const AudioChunkWindow &window,
    const std::atomic<bool> *cancel,
    bool encodeFlac)
{
    MediaResult<QVector<float>> pcm = decodePcm16kMono(mediaPath, window, cancel);
    if (std::holds_alternative<AppError>(pcm)) {
        return std::get<AppError>(pcm);
    }
    PreparedAudioChunk chunk;
    chunk.window = window;
    chunk.pcm16kMono = std::get<QVector<float>>(std::move(pcm));
    if (encodeFlac) {
        MediaResult<QByteArray> flac = AudioChunkExtractor::encodeFlac(chunk.pcm16kMono);
        if (std::holds_alternative<AppError>(flac)) {
            return std::get<AppError>(flac);
        }
        chunk.flac = std::get<QByteArray>(std::move(flac));
    }
    return chunk;
}

} // namespace subcue
