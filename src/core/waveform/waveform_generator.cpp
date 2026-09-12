#include "waveform/waveform_generator.h"

#include "common/logging.h"
#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"

#include <utility>

namespace subcue {
namespace {

bool isCancelled(const std::atomic<bool> *cancel)
{
    return cancel && cancel->load(std::memory_order_acquire);
}

bool isStale(const std::atomic<quint64> *generation, quint64 expectedGeneration)
{
    return generation && generation->load(std::memory_order_acquire) != expectedGeneration;
}

AppError cancelError()
{
    return AppError(ErrorDomain::Media, AVERROR(EINTR), QStringLiteral("波形生成已取消"));
}

AppError staleError()
{
    return AppError(ErrorDomain::Media, AVERROR(ECANCELED), QStringLiteral("波形生成已被更新的请求替换"));
}

bool appendConverted(
    AudioResampler &resampler,
    AVFrame &frame,
    QVector<float> &samples,
    AppError *error)
{
    QVector<float> converted = resampler.convert(frame, error);
    av_frame_unref(&frame);
    if (converted.isEmpty()) {
        return false;
    }
    samples.append(converted.constBegin(), converted.constEnd());
    return true;
}

} // namespace

WaveformGenerator::Result WaveformGenerator::generate(
    const QString &path,
    int streamIndex,
    int sampleRate,
    const std::atomic<bool> *cancel,
    const std::atomic<quint64> *generation,
    quint64 expectedGeneration) const
{
    if (sampleRate <= 0) {
        return AppError(ErrorDomain::Validation, AVERROR(EINVAL), QStringLiteral("波形采样率无效"));
    }
    if (isCancelled(cancel)) {
        return cancelError();
    }
    if (isStale(generation, expectedGeneration)) {
        return staleError();
    }

    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(path, &error)) {
        return error;
    }
    if (streamIndex < 0) {
        streamIndex = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    }
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
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
    if (!resampler.configure(*decoder.context(), sampleRate, 1, &error)) {
        return error;
    }
    FramePtr frame = makeFrame();
    if (!frame) {
        return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配音频帧"));
    }

    QVector<float> samples;
    while (!isCancelled(cancel) && !isStale(generation, expectedGeneration)) {
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
            if (!appendConverted(resampler, *frame, samples, &error)) {
                return error;
            }
        }
    }
    if (isCancelled(cancel)) {
        return cancelError();
    }
    if (isStale(generation, expectedGeneration)) {
        return staleError();
    }

    const int flushed = decoder.send(nullptr);
    if (flushed < 0 && flushed != AVERROR_EOF && flushed != AVERROR(EAGAIN)) {
        return makeFfmpegError(ErrorDomain::Decoder, flushed, QStringLiteral("无法刷新音频解码器"));
    }
    int result = 0;
    while (result >= 0) {
        result = decoder.receive(frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            return makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("音频解码失败"));
        }
        if (!appendConverted(resampler, *frame, samples, &error)) {
            return error;
        }
    }
    if (samples.isEmpty()) {
        return AppError(ErrorDomain::Decoder, AVERROR_EOF, QStringLiteral("媒体中没有可解码的音频样本"));
    }

    qCDebug(subcueWaveformLog) << "Built waveform pyramid" << path << "samples" << samples.size()
                               << "rate" << sampleRate;
    return std::make_shared<WaveformPyramid>(
        WaveformPyramid::fromMonoFloat(samples.constData(), samples.size(), sampleRate));
}

} // namespace subcue
