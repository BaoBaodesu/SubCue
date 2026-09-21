#include "ai/review_audio_encoder.h"

#include "asr/audio_chunk_extractor.h"
#include "asr/asr_types.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_raii.h"

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
        : (whence == SEEK_CUR ? out->position : out->data.size());
    out->position = std::clamp<qint64>(base + offset, 0, out->data.size());
    return out->position;
}

void appendLe16(QByteArray *out, quint16 value)
{
    char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
    out->append(bytes, 2);
}

void appendLe32(QByteArray *out, quint32 value)
{
    char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    out->append(bytes, 4);
}

} // namespace

QByteArray ReviewAudioEncoder::encodeWav16kMono(const QVector<float> &pcm16kMono)
{
    QByteArray data;
    data.reserve(44 + pcm16kMono.size() * 2);
    const quint32 dataBytes = static_cast<quint32>(pcm16kMono.size() * 2);
    data.append("RIFF", 4);
    appendLe32(&data, 36 + dataBytes);
    data.append("WAVE", 4);
    data.append("fmt ", 4);
    appendLe32(&data, 16);
    appendLe16(&data, 1);
    appendLe16(&data, 1);
    appendLe32(&data, static_cast<quint32>(kAsrSampleRate));
    appendLe32(&data, static_cast<quint32>(kAsrSampleRate * 2));
    appendLe16(&data, 2);
    appendLe16(&data, 16);
    data.append("data", 4);
    appendLe32(&data, dataBytes);
    for (float sample : pcm16kMono) {
        const int16_t pcm = static_cast<int16_t>(
            av_clip_int16(static_cast<int>(std::lrint(std::clamp(sample, -1.0f, 1.0f) * 32767.0f))));
        appendLe16(&data, static_cast<quint16>(pcm));
    }
    return data;
}

MediaResult<QByteArray> ReviewAudioEncoder::encodeAac16kMono(const QVector<float> &pcm16kMono)
{
    if (pcm16kMono.isEmpty()) {
        return AppError(ErrorDomain::Ai, AVERROR(EINVAL), QStringLiteral("没有可编码的审查音频"));
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        return AppError(ErrorDomain::Ai, AVERROR_ENCODER_NOT_FOUND,
            QStringLiteral("当前 FFmpeg 构建未启用 AAC 编码器"));
    }

    AVFormatContext *format = nullptr;
    int result = avformat_alloc_output_context2(&format, nullptr, "adts", nullptr);
    if (result < 0 || !format) {
        return makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("无法创建 AAC 封装"));
    }
    AVStream *stream = avformat_new_stream(format, nullptr);
    CodecContextPtr encoder(avcodec_alloc_context3(codec));
    if (!stream || !encoder) {
        avformat_free_context(format);
        return AppError(ErrorDomain::Ai, AVERROR(ENOMEM), QStringLiteral("无法创建 AAC 编码器"));
    }
    encoder->sample_rate = kAsrSampleRate;
    encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    encoder->bit_rate = 48'000;
    av_channel_layout_default(&encoder->ch_layout, 1);
    encoder->time_base = AVRational{1, kAsrSampleRate};
    result = avcodec_open2(encoder.get(), codec, nullptr);
    if (result < 0) {
        avformat_free_context(format);
        return makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("无法打开 AAC 编码器"));
    }
    result = avcodec_parameters_from_context(stream->codecpar, encoder.get());
    if (result < 0) {
        avformat_free_context(format);
        return makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("无法写入 AAC 流参数"));
    }
    stream->time_base = encoder->time_base;

    AvioBuffer buffer;
    unsigned char *avioMemory = static_cast<unsigned char *>(av_malloc(4096));
    if (!avioMemory) {
        avformat_free_context(format);
        return AppError(ErrorDomain::Ai, AVERROR(ENOMEM), QStringLiteral("无法分配 AAC 缓冲"));
    }
    AVIOContext *avio = avio_alloc_context(avioMemory, 4096, 1, &buffer, nullptr, writeAvio, seekAvio);
    if (!avio) {
        av_free(avioMemory);
        avformat_free_context(format);
        return AppError(ErrorDomain::Ai, AVERROR(ENOMEM), QStringLiteral("无法创建 AAC 输出"));
    }
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    result = avformat_write_header(format, nullptr);
    auto failEncode = [&](AppError failed) -> MediaResult<QByteArray> {
        format->pb = nullptr;
        avformat_free_context(format);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return failed;
    };
    if (result < 0) {
        return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("无法写入 AAC 文件头")));
    }

    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 1024;
    FramePtr frame = makeFrame();
    PacketPtr packet = makePacket();
    if (!frame || !packet) {
        return failEncode(AppError(ErrorDomain::Ai, AVERROR(ENOMEM), QStringLiteral("无法分配 AAC 帧")));
    }

    qint64 pts = 0;
    qsizetype offset = 0;
    while (offset < pcm16kMono.size()) {
        const int samples = static_cast<int>(
            std::min<qsizetype>(frameSize, pcm16kMono.size() - offset));
        frame->format = encoder->sample_fmt;
        frame->sample_rate = encoder->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
        frame->nb_samples = frameSize;
        result = av_frame_get_buffer(frame.get(), 0);
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("无法分配 AAC 样本缓冲")));
        }
        auto *dst = reinterpret_cast<float *>(frame->data[0]);
        for (int index = 0; index < frameSize; ++index) {
            dst[index] = index < samples
                ? std::clamp(pcm16kMono.at(offset + index), -1.0f, 1.0f)
                : 0.0f;
        }
        frame->pts = pts;
        pts += frameSize;
        offset += samples;
        result = avcodec_send_frame(encoder.get(), frame.get());
        av_frame_unref(frame.get());
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("提交 AAC 帧失败")));
        }
        while (result >= 0) {
            result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("AAC 编码失败")));
            }
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            result = av_interleaved_write_frame(format, packet.get());
            av_packet_unref(packet.get());
            if (result < 0) {
                return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("写入 AAC 数据失败")));
            }
        }
    }
    result = avcodec_send_frame(encoder.get(), nullptr);
    if (result < 0) {
        return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("刷新 AAC 编码器失败")));
    }
    while (result >= 0) {
        result = avcodec_receive_packet(encoder.get(), packet.get());
        if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("AAC 编码失败")));
        }
        av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        result = av_interleaved_write_frame(format, packet.get());
        av_packet_unref(packet.get());
        if (result < 0) {
            return failEncode(makeFfmpegError(ErrorDomain::Ai, result, QStringLiteral("写入 AAC 数据失败")));
        }
    }
    av_write_trailer(format);
    avio_flush(avio);
    QByteArray encoded = std::move(buffer.data);
    format->pb = nullptr;
    avformat_free_context(format);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    if (encoded.isEmpty()) {
        return AppError(ErrorDomain::Ai, AVERROR_INVALIDDATA, QStringLiteral("AAC 编码结果为空"));
    }
    return encoded;
}

MediaResult<OmniAudioClip> ReviewAudioEncoder::encodeWindow(
    const QString &mediaPath,
    qint64 startMs,
    qint64 endMs,
    const std::atomic<bool> *cancel)
{
    OmniAudioClip clip;
    clip.windowStartMs = std::max<qint64>(0, startMs);
    const qint64 requestedEnd = std::max(clip.windowStartMs + 1, endMs);
    clip.windowEndMs = std::min<qint64>(
        requestedEnd, clip.windowStartMs + kOmniSubtitleWindowMaxMs);
    clip.sampleRate = kAsrSampleRate;
    AudioChunkWindow window;
    window.startMs = clip.windowStartMs;
    window.endMs = clip.windowEndMs;
    window.startSeconds = static_cast<double>(window.startMs) / 1000.0;
    window.endSeconds = static_cast<double>(window.endMs) / 1000.0;
    MediaResult<QVector<float>> pcm = AudioChunkExtractor::decodePcm16kMono(mediaPath, window, cancel);
    if (std::holds_alternative<AppError>(pcm)) {
        return std::get<AppError>(pcm);
    }
    QVector<float> samples = std::get<QVector<float>>(std::move(pcm));
    MediaResult<QByteArray> aac = encodeAac16kMono(samples);
    if (std::holds_alternative<QByteArray>(aac)) {
        clip.data = std::get<QByteArray>(std::move(aac));
        clip.format = QStringLiteral("aac");
        samples.clear();
        samples.squeeze();
        return clip;
    }
    clip.data = encodeWav16kMono(samples);
    clip.format = QStringLiteral("wav");
    samples.clear();
    samples.squeeze();
    return clip;
}

} // namespace subcue
