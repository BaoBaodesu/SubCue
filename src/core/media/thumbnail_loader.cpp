#include "media/thumbnail_loader.h"

#include "common/logging.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"
#include "media/video_frame_converter.h"

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
    return AppError(ErrorDomain::Media, AVERROR(EINTR), QStringLiteral("缩略图生成已取消"));
}

AppError staleError()
{
    return AppError(ErrorDomain::Media, AVERROR(ECANCELED), QStringLiteral("缩略图生成已被更新的请求替换"));
}

} // namespace

ThumbnailLoader::Result ThumbnailLoader::decode(
    const QString &path,
    MediaTime pts,
    QSize size,
    int streamIndex,
    const std::atomic<bool> *cancel,
    const std::atomic<quint64> *generation,
    quint64 expectedGeneration) const
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return AppError(ErrorDomain::Validation, AVERROR(EINVAL), QStringLiteral("缩略图尺寸无效"));
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
        streamIndex = demuxer.bestStream(AVMEDIA_TYPE_VIDEO);
    }
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
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
    if (pts.microseconds() > 0) {
        const qint64 timestamp = timestampFromMediaTime(pts, stream->time_base);
        if (!demuxer.seek(streamIndex, timestamp, &error)) {
            return error;
        }
        decoder.flush();
    }

    FramePtr frame = makeFrame();
    if (!frame) {
        return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频帧"));
    }

    VideoFrameConverter converter;
    QImage lastImage;
    bool haveFrame = false;
    while (!isCancelled(cancel) && !isStale(generation, expectedGeneration)) {
        AppError decodeError(ErrorDomain::Decoder, 0, QString());
        if (!decoder.pullFrame(demuxer, streamIndex, frame.get(), &decodeError)) {
            if (!haveFrame) {
                if (decodeError.code() != 0) {
                    return decodeError;
                }
                return AppError(ErrorDomain::Decoder, AVERROR_EOF, QStringLiteral("媒体中没有可显示的视频帧"));
            }
            break;
        }
        QImage image = converter.convert(*frame, size, &error);
        if (image.isNull()) {
            return error;
        }
        lastImage = std::move(image);
        haveFrame = true;
        const MediaTime framePts = mediaTimeFromTimestamp(frame->best_effort_timestamp, stream->time_base);
        if (framePts.microseconds() >= pts.microseconds()) {
            break;
        }
        av_frame_unref(frame.get());
    }
    if (isCancelled(cancel)) {
        return cancelError();
    }
    if (isStale(generation, expectedGeneration)) {
        return staleError();
    }
    if (!haveFrame) {
        return AppError(ErrorDomain::Decoder, AVERROR_EOF, QStringLiteral("媒体中没有可显示的视频帧"));
    }
    qCDebug(subcueMediaLog) << "Decoded thumbnail" << path << pts.microseconds() << size;
    return lastImage;
}

} // namespace subcue
