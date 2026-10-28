#include "tts/audio_quality.h"
#include "media/demuxer.h"
#include "media/decoder.h"
#include "media/ffmpeg_error.h"
#include "roughcut/rough_cut_project.h"
#include <QtCore/QJsonArray>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace subcue {
namespace {
template<class T> T sample(const uint8_t *data)
{
    T value; std::memcpy(&value, data, sizeof(value)); return value;
}
double normalized(const uint8_t *data, AVSampleFormat format)
{
    switch (format) {
    case AV_SAMPLE_FMT_U8: return (sample<uint8_t>(data) - 128) / 128.0;
    case AV_SAMPLE_FMT_S16: return sample<int16_t>(data) / 32768.0;
    case AV_SAMPLE_FMT_S32: return sample<int32_t>(data) / 2147483648.0;
    case AV_SAMPLE_FMT_S64: return static_cast<double>(sample<int64_t>(data)) / 9223372036854775808.0;
    case AV_SAMPLE_FMT_FLT: return sample<float>(data);
    case AV_SAMPLE_FMT_DBL: return sample<double>(data);
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}
}

QJsonObject AudioQuality::toJson() const
{
    return {{QStringLiteral("version"), 1}, {QStringLiteral("sampleRate"), sampleRate}, {QStringLiteral("channels"), channels},
        {QStringLiteral("sampleCount"), QString::number(sampleCount)}, {QStringLiteral("peak"), peak}, {QStringLiteral("rms"), rms},
        {QStringLiteral("dcOffset"), dcOffset}, {QStringLiteral("clippedFraction"), clippedFraction},
        {QStringLiteral("silentFraction"), silentFraction}, {QStringLiteral("maxFrameBytes"), static_cast<qint64>(maxFrameBytes)},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)}, {QStringLiteral("sha256"), QString::fromLatin1(sha256.toHex())}};
}
QJsonObject AudioQuality::referenceReview() const
{
    auto result = toJson(); auto notes = warnings;
    const double seconds = sampleRate > 0 ? static_cast<double>(sampleCount) / sampleRate : 0;
    if (seconds < 3) notes.append(QStringLiteral("参考短于 3 秒，请确认内容充分且未截断发音；建议选择较完整的参考。"));
    if (seconds > 30) notes.append(QStringLiteral("参考长于 30 秒，建议裁切为较短、清晰的样本并复核转写。"));
    result.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(notes)); return result;
}

std::optional<AudioQuality> AudioQuality::inspect(const QString &path, QString *error, const std::atomic<bool> *cancel)
{
    const auto failure = [error](const QString &text) -> std::optional<AudioQuality> { if (error) *error = text; return {}; };
    const auto canceled = [cancel] { return cancel && cancel->load(); };
    if (canceled()) return failure(QStringLiteral("音频质检已取消。"));
    const auto hash = RoughCutProjectSerializer::mediaSha256(path, error, cancel);
    if (hash.size() != 32) return failure(canceled() ? QStringLiteral("音频质检已取消。") : QStringLiteral("音频文件无法读取或计算指纹。"));
    AppError mediaError(ErrorDomain::Media, 0, {});
    Demuxer demuxer;
    if (!demuxer.openLocal(path, cancel, &mediaError)) return failure(canceled() ? QStringLiteral("音频质检已取消。") : mediaError.userMessage());
    const int index = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const auto *stream = demuxer.stream(index);
    if (!stream || !stream->codecpar) return failure(QStringLiteral("文件没有可解码的音频流。"));
    Decoder decoder;
    if (!decoder.open(*stream, &mediaError)) return failure(mediaError.userMessage());
    AudioQuality result;
    result.sampleRate = decoder.context()->sample_rate; result.channels = decoder.context()->ch_layout.nb_channels;
    if (result.sampleRate <= 0 || result.channels <= 0 || result.channels > 32) return failure(QStringLiteral("音频采样率或声道数无效。"));
    auto frame = makeFrame();
    if (!frame) return failure(QStringLiteral("无法创建音频帧。"));
    std::array<double, 32> sum{};
    double energy = 0;
    qint64 clipped = 0, silent = 0;
    bool flushed = false;
    for (;;) {
        if (canceled()) return failure(QStringLiteral("音频质检已取消。"));
        const int received = decoder.receive(frame.get());
        if (received == AVERROR_EOF) break;
        if (received == AVERROR(EAGAIN)) {
            if (flushed) return failure(QStringLiteral("音频解码未正常结束。"));
            mediaError = AppError(ErrorDomain::Media, 0, {});
            auto packet = demuxer.readPacket(&mediaError);
            while (packet && packet->stream_index != index) {
                if (canceled()) return failure(QStringLiteral("音频质检已取消。"));
                packet = demuxer.readPacket(&mediaError);
            }
            if (mediaError.code() != 0) return failure(canceled() ? QStringLiteral("音频质检已取消。") : mediaError.userMessage());
            if (packet && ((packet->flags & AV_PKT_FLAG_CORRUPT) || packet->size > 16 * 1024 * 1024))
                return failure(QStringLiteral("音频数据损坏或数据包超过处理上限。"));
            if (!packet) flushed = true;
            const int sent = decoder.send(packet.get());
            if (sent < 0 && sent != AVERROR_EOF) return failure(makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("音频解码提交失败")).userMessage());
            continue;
        }
        if (received < 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT)) return failure(QStringLiteral("音频解码失败或含损坏帧。"));
        const auto format = static_cast<AVSampleFormat>(frame->format);
        const auto packed = av_get_packed_sample_fmt(format);
        const int bytes = av_get_bytes_per_sample(format);
        const qint64 frameBytes = static_cast<qint64>(frame->nb_samples) * result.channels * bytes;
        if (bytes <= 0 || frame->nb_samples <= 0 || frameBytes > 4 * 1024 * 1024
            || frame->sample_rate != result.sampleRate || frame->ch_layout.nb_channels != result.channels
            || result.sampleCount > std::numeric_limits<qint64>::max() / result.channels - frame->nb_samples)
            return failure(QStringLiteral("音频帧规格变化或超过处理上限。"));
        result.maxFrameBytes = std::max(result.maxFrameBytes, static_cast<qsizetype>(frameBytes));
        for (int i = 0; i < frame->nb_samples; ++i) {
            for (int channel = 0; channel < result.channels; ++channel) {
                const auto *data = av_sample_fmt_is_planar(format) ? frame->extended_data[channel] : frame->extended_data[0];
                if (!data) return failure(QStringLiteral("音频帧缺少样本数据。"));
                const auto offset = (av_sample_fmt_is_planar(format) ? i : i * result.channels + channel) * bytes;
                const double value = normalized(data + offset, packed);
                if (!std::isfinite(value) || std::abs(value) > 16.0) return failure(QStringLiteral("音频含非有限值或异常幅度样本。"));
                result.peak = std::max(result.peak, std::abs(value));
                sum[channel] += value; energy += value * value;
                if (std::abs(value) >= 0.999) ++clipped;
                if (std::abs(value) < 0.001) ++silent;
            }
        }
        result.sampleCount += frame->nb_samples;
        av_frame_unref(frame.get());
    }
    if (canceled()) return failure(QStringLiteral("音频质检已取消。"));
    if (result.sampleCount == 0) return failure(QStringLiteral("音频没有有效样本。"));
    if (RoughCutProjectSerializer::mediaSha256(path, error, cancel) != hash)
        return failure(canceled() ? QStringLiteral("音频质检已取消。") : QStringLiteral("质检期间音频发生变化，请重新检查。"));
    result.sha256 = hash;
    const double count = static_cast<double>(result.sampleCount) * result.channels;
    result.rms = std::sqrt(energy / count); result.clippedFraction = clipped / count; result.silentFraction = silent / count;
    for (int channel = 0; channel < result.channels; ++channel) result.dcOffset = std::max(result.dcOffset, std::abs(sum[channel] / result.sampleCount));
    if (result.clippedFraction > 0) result.warnings.append(QStringLiteral("检测到削波或接近满幅的样本，请试听。"));
    if (result.silentFraction > 0.8) result.warnings.append(QStringLiteral("静音比例较高，请检查漏句或参考内容。"));
    if (result.rms < 0.005) result.warnings.append(QStringLiteral("音频电平较低，请试听。"));
    if (result.dcOffset > 0.01) result.warnings.append(QStringLiteral("直流偏移较高，可试听去直流派生样本。"));
    return result;
}
}
