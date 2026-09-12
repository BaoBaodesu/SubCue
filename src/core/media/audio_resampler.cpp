#include "media/audio_resampler.h"

#include "media/ffmpeg_error.h"

namespace subcue {

bool AudioResampler::configure(
    const AVCodecContext &decoder,
    int outputSampleRate,
    int outputChannels,
    AppError *error)
{
    context_.reset();
    AVChannelLayout outputLayout{};
    av_channel_layout_default(&outputLayout, outputChannels);
    SwrContext *rawContext = nullptr;
    int result = swr_alloc_set_opts2(
        &rawContext,
        &outputLayout,
        AV_SAMPLE_FMT_FLT,
        outputSampleRate,
        &decoder.ch_layout,
        decoder.sample_fmt,
        decoder.sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&outputLayout);
    if (result < 0 || !rawContext) {
        if (rawContext) {
            swr_free(&rawContext);
        }
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("无法创建音频重采样器"));
        }
        return false;
    }
    context_.reset(rawContext);
    result = swr_init(context_.get());
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("无法初始化音频重采样器"));
        }
        context_.reset();
        return false;
    }
    sampleRate_ = outputSampleRate;
    channels_ = outputChannels;
    return true;
}

QVector<float> AudioResampler::convert(const AVFrame &frame, AppError *error)
{
    if (!context_ || channels_ <= 0) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("音频重采样器尚未配置"));
        }
        return {};
    }
    const int maximumSamples = swr_get_out_samples(context_.get(), frame.nb_samples);
    if (maximumSamples < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, maximumSamples, QStringLiteral("无法计算音频输出长度"));
        }
        return {};
    }
    QVector<float> samples(static_cast<qsizetype>(maximumSamples) * channels_);
    uint8_t *output[] = {reinterpret_cast<uint8_t *>(samples.data())};
    const int converted = swr_convert(
        context_.get(),
        output,
        maximumSamples,
        const_cast<const uint8_t **>(frame.extended_data),
        frame.nb_samples);
    if (converted < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, converted, QStringLiteral("音频重采样失败"));
        }
        return {};
    }
    samples.resize(static_cast<qsizetype>(converted) * channels_);
    return samples;
}

} // namespace subcue
