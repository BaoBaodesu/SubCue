#include "roughcut/wav_exporter.h"
#include "common/file_path_guard.h"
#include <functional>

#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_time.h"
#include "media/ffmpeg_error.h"

#include <QtCore/QSaveFile>
#include <QtCore/QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <variant>

namespace subcue {
namespace {

bool fail(QString *output, const QString &message)
{
    if (output) *output = message;
    return false;
}

template<typename T>
void appendLittleEndian(QByteArray &data, T value)
{
    const T converted = qToLittleEndian(value);
    data.append(reinterpret_cast<const char *>(&converted), sizeof(converted));
}

QByteArray waveHeader(int sampleRate, int channels, quint32 dataSize)
{
    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4); appendLittleEndian(header, quint32(36 + dataSize));
    header.append("WAVEfmt ", 8); appendLittleEndian(header, quint32(16));
    appendLittleEndian(header, quint16(3)); // IEEE float
    appendLittleEndian(header, quint16(channels));
    appendLittleEndian(header, quint32(sampleRate));
    appendLittleEndian(header, quint32(sampleRate * channels * int(sizeof(float))));
    appendLittleEndian(header, quint16(channels * int(sizeof(float))));
    appendLittleEndian(header, quint16(32));
    header.append("data", 4); appendLittleEndian(header, dataSize);
    return header;
}

std::variant<qint64, AppError> decodeClip(
    const QString &path, qint64 startSample, qint64 endSample,
    int sampleRate, int channels, const std::atomic<bool> *cancel,
    const std::function<bool(QVector<float>, qint64)> &write)
{
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(path, &error)) return error;
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream) return AppError(ErrorDomain::Decoder, streamIndex, QStringLiteral("源媒体没有音频流"));
    Decoder decoder;
    if (!decoder.open(*stream, &error)) return error;
    const qint64 startUs = startSample * 1'000'000 / sampleRate;
    if (!demuxer.seek(streamIndex,
            timestampFromMediaTime(MediaTime::fromMicroseconds(startUs), stream->time_base), &error))
        return error;
    decoder.flush();
    AudioResampler resampler;
    if (!resampler.configure(*decoder.context(), sampleRate, channels, &error)) return error;
    FramePtr frame = makeFrame();
    qint64 output = 0;
    qint64 nextSample = 0;
    const QVector<float> silence(4096 * channels);
    bool draining = false;
    for (;;) {
        if (cancel && cancel->load())
            return AppError(ErrorDomain::Media, AVERROR_EXIT, QStringLiteral("导出已取消"));
        PacketPtr packet;
        if (!draining) {
            packet = demuxer.readPacket(&error);
            if (!packet) {
                if (error.code() != 0) return error;
                draining = true;
                const int sent = decoder.send(nullptr);
                if (sent < 0 && sent != AVERROR_EOF)
                    return makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("无法刷新音频解码器"));
            } else if (packet->stream_index != streamIndex) {
                continue;
            } else {
                const int sent = decoder.send(packet.get());
                if (sent < 0 && sent != AVERROR(EAGAIN))
                    return makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("提交音频数据失败"));
            }
        }
        for (;;) {
            const int received = decoder.receive(frame.get());
            if (received == AVERROR(EAGAIN)) break;
            if (received < 0 && received != AVERROR_EOF)
                return makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("音频解码失败"));
            if (received != AVERROR_EOF && frame->best_effort_timestamp == AV_NOPTS_VALUE)
                return AppError(ErrorDomain::Media, 1, QStringLiteral("音频缺少时间戳，无法导出"));
            const qint64 frameStartSample = received == AVERROR_EOF ? nextSample
                : av_rescale_q(frame->best_effort_timestamp, stream->time_base, AVRational{1, sampleRate}) - resampler.delaySamples();
            // 解码结束后用空帧排出重采样器尾部，不能用补零代替真实尾音。
            if (received == AVERROR_EOF) av_frame_unref(frame.get());
            QVector<float> converted = resampler.convert(*frame, &error);
            av_frame_unref(frame.get());
            if (converted.isEmpty()) {
                if (error.code() != 0) return error;
                if (received == AVERROR_EOF) return output;
                continue;
            }
            const qint64 frameSamples = converted.size() / channels;
            nextSample = frameStartSample + frameSamples;
            if (nextSample <= startSample) continue;
            if (frameStartSample >= endSample) return output;
            const qint64 skip = std::clamp(startSample - frameStartSample, qint64(0), frameSamples);
            const qint64 keepEnd = std::clamp(endSample - frameStartSample, qint64(0), frameSamples);
            if (keepEnd > skip) {
                const qint64 desired = std::max<qint64>(0, frameStartSample + skip - startSample);
                while (output < desired) {
                    const qint64 count = std::min<qint64>(4096, desired - output);
                    if (cancel && cancel->load()) return AppError(ErrorDomain::Media, AVERROR_EXIT, QStringLiteral("导出已取消"));
                    if (!write(silence.first(count * channels), output)) return AppError(ErrorDomain::Media, 1, QStringLiteral("WAV 写入失败"));
                    output += count;
                }
                const qint64 overlap = std::max<qint64>(0, output - desired);
                if (keepEnd - skip > overlap) {
                    QVector<float> block = converted.mid((skip + overlap) * channels, (keepEnd - skip - overlap) * channels);
                    if (!write(block, output)) return AppError(ErrorDomain::Media, 1, QStringLiteral("WAV 写入失败"));
                    output += block.size() / channels;
                }
            }
        }
        if (draining) return output;
    }
}

} // namespace

bool RoughCutWavExporter::save(
    const QString &path, const QString &sourcePath,
    const QVector<RoughCutTimelineClip> &clips, int sampleRate, int channels,
    const std::atomic<bool> *cancel, QString *errorMessage, int fadeMs)
{
    if (path.isEmpty() || sourcePath.isEmpty() || clips.isEmpty()
        || sampleRate <= 0 || channels <= 0)
        return fail(errorMessage, QStringLiteral("精简 WAV 导出参数无效"));
    if (!safeOutputPath(path, {sourcePath}, errorMessage)) return false;
    qint64 expectedEnd = 0;
    for (const auto &clip : clips) {
        if (clip.sourceStartSample < 0 || clip.sourceEndSample <= clip.sourceStartSample
            || clip.timelineStartSample < expectedEnd)
            return fail(errorMessage, QStringLiteral("WAV 片段范围无效或时间线重叠"));
        expectedEnd = clip.timelineStartSample + clip.sourceEndSample - clip.sourceStartSample;
    }
    if (quint64(expectedEnd) > (std::numeric_limits<quint32>::max() - 36ULL) / (channels * sizeof(float)))
        return fail(errorMessage, QStringLiteral("精简 WAV 超过标准 RIFF 4 GiB 限制"));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return fail(errorMessage, output.errorString());
    if (output.write(waveHeader(sampleRate, channels, 0)) != 44)
        return fail(errorMessage, output.errorString());
    quint64 writtenBytes = 0;
    qint64 timelineEnd = 0;
    const int fadeSamples = std::max(0, fadeMs) * sampleRate / 1000;
    QVector<float> silence(4096 * channels);
    for (const RoughCutTimelineClip &clip : clips) {
        const qint64 gapFrames = std::max<qint64>(0, clip.timelineStartSample - timelineEnd);
        qint64 remaining = gapFrames;
        while (remaining > 0) {
            if (cancel && cancel->load()) return fail(errorMessage, QStringLiteral("导出已取消"));
            const qint64 frames = std::min<qint64>(remaining, 4096);
            const qint64 bytes = frames * channels * sizeof(float);
            if (output.write(reinterpret_cast<const char *>(silence.constData()), bytes) != bytes)
                return fail(errorMessage, output.errorString());
            writtenBytes += bytes;
            remaining -= frames;
        }
        const qint64 clipFrames = clip.sourceEndSample - clip.sourceStartSample;
        const qint64 fade = std::min<qint64>(fadeSamples, clipFrames / 2);
        const auto write = [&](QVector<float> block, qint64 position) {
            for (qsizetype frame = 0; frame < block.size() / channels; ++frame) {
                const qint64 at = position + frame;
                float gain = 1.0f;
                if (at < fade) gain = std::sin(float(at + 1) / float(fade + 1) * std::numbers::pi_v<float> / 2.0f);
                else if (at >= clipFrames - fade) gain = std::cos(float(at - (clipFrames - fade) + 1) / float(fade + 1) * std::numbers::pi_v<float> / 2.0f);
                for (int channel = 0; channel < channels; ++channel) block[frame * channels + channel] *= gain;
            }
            const qint64 bytes = block.size() * sizeof(float);
            if (output.write(reinterpret_cast<const char *>(block.constData()), bytes) != bytes) return false;
            writtenBytes += bytes;
            return true;
        };
        auto decoded = decodeClip(sourcePath, clip.sourceStartSample, clip.sourceEndSample,
                                  sampleRate, channels, cancel, write);
        if (std::holds_alternative<AppError>(decoded))
            return fail(errorMessage, std::get<AppError>(decoded).userMessage());
        qint64 frames = std::get<qint64>(decoded);
        while (frames < clipFrames) {
            if (cancel && cancel->load()) return fail(errorMessage, QStringLiteral("导出已取消"));
            const qint64 count = std::min<qint64>(4096, clipFrames - frames);
            if (!write(silence.first(count * channels), frames)) return fail(errorMessage, output.errorString());
            frames += count;
        }
        timelineEnd = clip.timelineStartSample + frames;
        if (writtenBytes > std::numeric_limits<quint32>::max() - 36ULL)
            return fail(errorMessage, QStringLiteral("精简 WAV 超过标准 RIFF 4 GiB 限制"));
    }
    if (!output.seek(0) || output.write(waveHeader(sampleRate, channels,
            static_cast<quint32>(writtenBytes))) != 44)
        return fail(errorMessage, output.errorString());
    if (cancel && cancel->load()) return fail(errorMessage, QStringLiteral("导出已取消"));
    return output.commit() || fail(errorMessage, output.errorString());
}

} // namespace subcue
