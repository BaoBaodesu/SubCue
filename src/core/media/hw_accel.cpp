#include "media/hw_accel.h"

#include "media/ffmpeg_error.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

namespace subcue {

QVector<HwAccelType> hwAccelFallbackOrder()
{
    return {
        HwAccelType::D3D11VA,
        HwAccelType::D3D12VA,
        HwAccelType::QSV,
        HwAccelType::CUDA,
        HwAccelType::Software,
    };
}

QString hwAccelName(HwAccelType type)
{
    switch (type) {
    case HwAccelType::D3D11VA:
        return QStringLiteral("d3d11va");
    case HwAccelType::D3D12VA:
        return QStringLiteral("d3d12va");
    case HwAccelType::QSV:
        return QStringLiteral("qsv");
    case HwAccelType::CUDA:
        return QStringLiteral("cuda");
    case HwAccelType::Software:
        return QStringLiteral("software");
    }
    return QStringLiteral("unknown");
}

AVHWDeviceType avDeviceType(HwAccelType type) noexcept
{
    switch (type) {
    case HwAccelType::D3D11VA:
        return AV_HWDEVICE_TYPE_D3D11VA;
    case HwAccelType::D3D12VA:
        return AV_HWDEVICE_TYPE_D3D12VA;
    case HwAccelType::QSV:
        return AV_HWDEVICE_TYPE_QSV;
    case HwAccelType::CUDA:
        return AV_HWDEVICE_TYPE_CUDA;
    case HwAccelType::Software:
        return AV_HWDEVICE_TYPE_NONE;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

bool codecSupportsHwAccel(const AVCodec *codec, HwAccelType type, AVPixelFormat *pixelFormat)
{
    if (!codec || type == HwAccelType::Software) {
        return type == HwAccelType::Software;
    }
    const AVHWDeviceType deviceType = avDeviceType(type);
    for (int index = 0;; ++index) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
        if (!config) {
            break;
        }
        if (config->device_type == deviceType
            && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            if (pixelFormat) {
                *pixelFormat = config->pix_fmt;
            }
            return true;
        }
    }
    return false;
}

BufferRefPtr createHwDevice(HwAccelType type, AppError *error)
{
    if (type == HwAccelType::Software) {
        return {};
    }
    const int previousLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    AVBufferRef *rawDevice = nullptr;
    const int result = av_hwdevice_ctx_create(&rawDevice, avDeviceType(type), nullptr, nullptr, 0);
    av_log_set_level(previousLevel);
    if (result < 0 || rawDevice == nullptr) {
        if (error) {
            *error = makeFfmpegError(
                ErrorDomain::Decoder,
                result,
                QStringLiteral("无法创建硬件解码设备"),
                hwAccelName(type));
        }
        return {};
    }
    return BufferRefPtr(rawDevice);
}

bool transferFrameToSoftware(const AVFrame &source, AVFrame &destination, AppError *error)
{
    const int result = av_hwframe_transfer_data(&destination, &source, 0);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Decoder, result, QStringLiteral("无法下载硬件视频帧"));
        }
        return false;
    }
    destination.pts = source.pts;
    destination.pkt_dts = source.pkt_dts;
    destination.best_effort_timestamp = source.best_effort_timestamp;
    destination.duration = source.duration;
    destination.time_base = source.time_base;
    return true;
}

} // namespace subcue
