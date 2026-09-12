#include "media/decoder.h"

#include "common/logging.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"

#include <utility>

namespace subcue {
namespace {

AVPixelFormat chooseHwPixelFormat(AVCodecContext *context, const AVPixelFormat *pixelFormats)
{
    const auto *pixelFormat = static_cast<const AVPixelFormat *>(context->opaque);
    if (!pixelFormat) {
        return AV_PIX_FMT_NONE;
    }
    for (const AVPixelFormat *candidate = pixelFormats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == *pixelFormat) {
            return *candidate;
        }
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

bool Decoder::open(const AVStream &stream, AppError *error)
{
    return open(stream, HwAccelType::Software, error);
}

bool Decoder::open(const AVStream &stream, HwAccelType accel, AppError *error)
{
    close();
    attempts_.clear();
    if (!storeStream(stream, error)) {
        return false;
    }
    AppError attemptError(ErrorDomain::Decoder, 0, QString());
    const bool opened = openStored(accel, &attemptError);
    attempts_.push_back(HwAccelAttempt{accel, opened, opened ? QString() : attemptError.technicalDetails()});
    if (!opened && error) {
        *error = std::move(attemptError);
    }
    return opened;
}

bool Decoder::openWithFallback(const AVStream &stream, AppError *error)
{
    close();
    attempts_.clear();
    if (!storeStream(stream, error)) {
        return false;
    }

    AppError lastError(ErrorDomain::Decoder, AVERROR(ENOSYS), QStringLiteral("没有可用的解码器"));
    for (const HwAccelType type : hwAccelFallbackOrder()) {
        AppError attemptError(ErrorDomain::Decoder, 0, QString());
        if (openStored(type, &attemptError)) {
            attempts_.push_back(HwAccelAttempt{type, true, QString()});
            if (type != HwAccelType::Software) {
                qCInfo(subcuePlaybackLog) << "Using hardware decoder" << hwAccelName(type);
            }
            return true;
        }
        attempts_.push_back(HwAccelAttempt{type, false, attemptError.technicalDetails()});
        lastError = std::move(attemptError);
        qCInfo(subcuePlaybackLog) << "Hardware decoder unavailable" << hwAccelName(type)
                                  << lastError.technicalDetails();
    }
    if (error) {
        *error = std::move(lastError);
    }
    return false;
}

bool Decoder::fallbackToSoftware(AppError *error)
{
    return handleRuntimeFailure(error);
}

bool Decoder::handleRuntimeFailure(AppError *error)
{
    const HwAccelType previous = activeType_;
    qCInfo(subcuePlaybackLog) << "Reopening software decoder after failure from" << hwAccelName(previous);
    closeContext();
    AppError reopenError(ErrorDomain::Decoder, 0, QString());
    if (!openStored(HwAccelType::Software, &reopenError)) {
        attempts_.push_back(HwAccelAttempt{HwAccelType::Software, false, reopenError.technicalDetails()});
        if (error) {
            *error = std::move(reopenError);
        }
        return false;
    }
    attempts_.push_back(HwAccelAttempt{HwAccelType::Software, true, QStringLiteral("runtime-fallback")});
    return true;
}

void Decoder::close()
{
    closeContext();
    codecpar_.reset();
    pktTimeBase_ = {0, 1};
    attempts_.clear();
}

void Decoder::closeContext()
{
    context_.reset();
    hwDevice_.reset();
    hwPixelFormat_ = AV_PIX_FMT_NONE;
    activeType_ = HwAccelType::Software;
}

void Decoder::flush()
{
    if (context_) {
        avcodec_flush_buffers(context_.get());
    }
}

int Decoder::send(const AVPacket *packet) noexcept
{
    return context_ ? avcodec_send_packet(context_.get(), packet) : AVERROR(EINVAL);
}

int Decoder::receive(AVFrame *frame) noexcept
{
    return context_ ? avcodec_receive_frame(context_.get(), frame) : AVERROR(EINVAL);
}

bool Decoder::ensureSoftwareFrame(AVFrame *frame, AppError *error)
{
    if (!frame) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("视频帧无效"));
        }
        return false;
    }
    if (!frame->hw_frames_ctx) {
        return true;
    }
    FramePtr software = makeFrame();
    if (!software) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配软件视频帧"));
        }
        return false;
    }
    if (!transferFrameToSoftware(*frame, *software, error)) {
        return false;
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, software.get());
    return true;
}

bool Decoder::pullFrame(Demuxer &demuxer, int streamIndex, AVFrame *frame, AppError *error)
{
    if (!context_ || !frame) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("解码器尚未打开"));
        }
        return false;
    }

    PacketPtr pending;
    bool flushed = false;
    for (;;) {
        const int received = receive(frame);
        if (received >= 0) {
            return ensureSoftwareFrame(frame, error);
        }
        if (received != AVERROR(EAGAIN) && received != AVERROR_EOF) {
            if (error) {
                *error = makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("视频解码失败"));
            }
            return false;
        }

        if (!pending) {
            if (flushed) {
                return false;
            }
            pending = demuxer.readPacket(error);
            while (pending && pending->stream_index != streamIndex) {
                pending = demuxer.readPacket(error);
            }
            if (!pending) {
                const int flushedSend = send(nullptr);
                flushed = true;
                if (flushedSend < 0 && flushedSend != AVERROR(EAGAIN) && flushedSend != AVERROR_EOF) {
                    if (error) {
                        *error = makeFfmpegError(ErrorDomain::Decoder, flushedSend, QStringLiteral("无法刷新解码器"));
                    }
                    return false;
                }
                continue;
            }
        }

        const int sent = send(pending.get());
        if (sent == AVERROR(EAGAIN)) {
            continue;
        }
        if (sent < 0) {
            if (error) {
                *error = makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("提交视频数据失败"));
            }
            return false;
        }
        pending.reset();
    }
}

bool Decoder::storeStream(const AVStream &stream, AppError *error)
{
    if (!stream.codecpar) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("媒体流缺少解码参数"));
        }
        return false;
    }
    codecpar_.reset(avcodec_parameters_alloc());
    if (!codecpar_) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配解码参数"));
        }
        return false;
    }
    const int result = avcodec_parameters_copy(codecpar_.get(), stream.codecpar);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("无法复制解码参数"));
        }
        codecpar_.reset();
        return false;
    }
    pktTimeBase_ = stream.time_base;
    return true;
}

bool Decoder::openStored(HwAccelType accel, AppError *error)
{
    closeContext();
    if (!codecpar_) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(EINVAL), QStringLiteral("解码参数尚未保存"));
        }
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(codecpar_->codec_id);
    if (!codec) {
        if (error) {
            *error = AppError(
                ErrorDomain::Decoder,
                AVERROR_DECODER_NOT_FOUND,
                QStringLiteral("找不到媒体解码器"),
                QString::fromUtf8(avcodec_get_name(codecpar_->codec_id)));
        }
        return false;
    }

    AVPixelFormat hwPixelFormat = AV_PIX_FMT_NONE;
    BufferRefPtr hwDevice;
    if (accel != HwAccelType::Software) {
        if (!codecSupportsHwAccel(codec, accel, &hwPixelFormat)) {
            if (error) {
                *error = AppError(
                    ErrorDomain::Decoder,
                    AVERROR(ENOSYS),
                    QStringLiteral("解码器不支持该硬件加速"),
                    hwAccelName(accel));
            }
            return false;
        }
        hwDevice = createHwDevice(accel, error);
        if (!hwDevice) {
            return false;
        }
    }

    context_.reset(avcodec_alloc_context3(codec));
    if (!context_) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配解码器"));
        }
        return false;
    }
    int result = avcodec_parameters_to_context(context_.get(), codecpar_.get());
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("无法读取解码参数"));
        }
        closeContext();
        return false;
    }
    context_->pkt_timebase = pktTimeBase_;
    if (hwDevice) {
        hwPixelFormat_ = hwPixelFormat;
        context_->opaque = &hwPixelFormat_;
        context_->get_format = chooseHwPixelFormat;
        context_->hw_device_ctx = av_buffer_ref(hwDevice.get());
        if (!context_->hw_device_ctx) {
            if (error) {
                *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法引用硬件解码设备"));
            }
            closeContext();
            return false;
        }
        hwDevice_ = std::move(hwDevice);
    }

    result = avcodec_open2(context_.get(), codec, nullptr);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(
                ErrorDomain::Decoder,
                result,
                QStringLiteral("无法启动媒体解码器"),
                hwAccelName(accel));
        }
        closeContext();
        return false;
    }
    activeType_ = accel;
    return true;
}

} // namespace subcue
