#include "roughcut/wav_exporter.h"

#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_time.h"

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

std::variant<QVector<float>, AppError> decodeClip(
    const QString &path, qint64 startSample, qint64 endSample,
    int sampleRate, int channels, const std::atomic<bool> *cancel)
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
    const qint64 endUs = endSample * 1'000'000 / sampleRate;
    if (!demuxer.seek(streamIndex,
            timestampFromMediaTime(MediaTime::fromMicroseconds(startUs), stream->time_base), &error))
        return error;
    decoder.flush();
    AudioResampler resampler;
    if (!resampler.configure(*decoder.context(), sampleRate, channels, &error)) return error;
    FramePtr frame = makeFrame();
    QVector<float> output;
    output.reserve(static_cast<qsizetype>(endSample - startSample) * channels);
    bool draining = false;
    for (;;) {
        if (cancel && cancel->load())
            return AppError(ErrorDomain::Media, AVERROR_EXIT, QStringLiteral("导出已取消"));
        PacketPtr packet;
        if (!draining) {
            packet = demuxer.readPacket(&error);
            if (!packet) {
                draining = true;
                (void)decoder.send(nullptr);
            } else if (packet->stream_index != streamIndex) {
                continue;
            } else {
                const int sent = decoder.send(packet.get());
                if (sent < 0 && sent != AVERROR(EAGAIN)) return error;
            }
        }
        for (;;) {
            const int received = decoder.receive(frame.get());
            if (received == AVERROR(EAGAIN)) break;
            if (received == AVERROR_EOF) return output;
            if (received < 0) return error;
            const qint64 frameStartUs = mediaTimeFromTimestamp(
                frame->best_effort_timestamp, stream->time_base).microseconds();
            QVector<float> converted = resampler.convert(*frame, &error);
            av_frame_unref(frame.get());
            if (converted.isEmpty()) return error;
            const qint64 frameSamples = converted.size() / channels;
            const qint64 frameEndUs = frameStartUs + frameSamples * 1'000'000 / sampleRate;
            if (frameEndUs <= startUs) continue;
            if (frameStartUs >= endUs) return output;
            const qint64 skip = frameStartUs < startUs
                ? std::clamp((startUs - frameStartUs) * sampleRate / 1'000'000,
                    qint64(0), frameSamples) : 0;
            const qint64 keepEnd = frameEndUs > endUs
                ? std::clamp(frameSamples - (frameEndUs - endUs) * sampleRate / 1'000'000,
                    qint64(0), frameSamples) : frameSamples;
            if (keepEnd > skip) output.append(converted.cbegin() + skip * channels,
                                               converted.cbegin() + keepEnd * channels);
        }
        if (draining) return output;
    }
}

void applyEdgeFades(QVector<float> &samples, int channels, int fadeSamples)
{
    const int frames = samples.size() / channels;
    const int count = std::min(fadeSamples, frames / 2);
    for (int frame = 0; frame < count; ++frame) {
        const float angle = float(frame + 1) / float(count + 1)
            * std::numbers::pi_v<float> / 2.0f;
        const float in = std::sin(angle);
        const float out = std::cos(angle);
        for (int channel = 0; channel < channels; ++channel) {
            samples[frame * channels + channel] *= in;
            samples[(frames - count + frame) * channels + channel] *= out;
        }
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
            const qint64 frames = std::min<qint64>(remaining, 4096);
            const qint64 bytes = frames * channels * sizeof(float);
            if (output.write(reinterpret_cast<const char *>(silence.constData()), bytes) != bytes)
                return fail(errorMessage, output.errorString());
            writtenBytes += bytes;
            remaining -= frames;
        }
        auto decoded = decodeClip(sourcePath, clip.sourceStartSample, clip.sourceEndSample,
                                  sampleRate, channels, cancel);
        if (std::holds_alternative<AppError>(decoded))
            return fail(errorMessage, std::get<AppError>(decoded).userMessage());
        QVector<float> samples = std::get<QVector<float>>(std::move(decoded));
        applyEdgeFades(samples, channels, fadeSamples);
        const qint64 bytes = static_cast<qint64>(samples.size()) * sizeof(float);
        if (output.write(reinterpret_cast<const char *>(samples.constData()), bytes) != bytes)
            return fail(errorMessage, output.errorString());
        writtenBytes += bytes;
        timelineEnd = clip.timelineStartSample + samples.size() / channels;
        if (writtenBytes > std::numeric_limits<quint32>::max())
            return fail(errorMessage, QStringLiteral("精简 WAV 超过标准 RIFF 4 GiB 限制"));
    }
    if (!output.seek(0) || output.write(waveHeader(sampleRate, channels,
            static_cast<quint32>(writtenBytes))) != 44)
        return fail(errorMessage, output.errorString());
    return output.commit() || fail(errorMessage, output.errorString());
}

} // namespace subcue
