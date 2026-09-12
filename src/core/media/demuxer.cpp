#include "media/demuxer.h"

#include "media/ffmpeg_error.h"

#include <QtCore/QFile>

namespace subcue {

bool Demuxer::open(const QString &path, AppError *error)
{
    close();
    AVFormatContext *rawContext = nullptr;
    const QByteArray nativePath = path.toUtf8();
    int result = avformat_open_input(&rawContext, nativePath.constData(), nullptr, nullptr);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("无法打开媒体文件"), path);
        }
        return false;
    }
    context_.reset(rawContext);
    result = avformat_find_stream_info(context_.get(), nullptr);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("无法读取媒体流"), path);
        }
        close();
        return false;
    }
    return true;
}

void Demuxer::close()
{
    context_.reset();
}

PacketPtr Demuxer::readPacket(AppError *error)
{
    if (!context_) {
        if (error) {
            *error = AppError(ErrorDomain::Media, AVERROR(EINVAL), QStringLiteral("媒体尚未打开"));
        }
        return {};
    }
    PacketPtr packet = makePacket();
    if (!packet) {
        if (error) {
            *error = AppError(ErrorDomain::Media, AVERROR(ENOMEM), QStringLiteral("无法分配媒体数据包"));
        }
        return {};
    }
    const int result = av_read_frame(context_.get(), packet.get());
    if (result < 0) {
        if (error && result != AVERROR_EOF) {
            *error = makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("读取媒体数据失败"));
        }
        return {};
    }
    return packet;
}

bool Demuxer::seek(int streamIndex, qint64 timestamp, AppError *error)
{
    if (!context_ || streamIndex < 0 || streamIndex >= static_cast<int>(context_->nb_streams)) {
        if (error) {
            *error = AppError(ErrorDomain::Media, AVERROR(EINVAL), QStringLiteral("Seek 参数无效"));
        }
        return false;
    }
    const int result = av_seek_frame(context_.get(), streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        if (error) {
            *error = makeFfmpegError(ErrorDomain::Media, result, QStringLiteral("媒体定位失败"));
        }
        return false;
    }
    return true;
}

const AVStream *Demuxer::stream(int index) const noexcept
{
    if (!context_ || index < 0 || index >= static_cast<int>(context_->nb_streams)) {
        return nullptr;
    }
    return context_->streams[index];
}

int Demuxer::bestStream(AVMediaType type) const noexcept
{
    return context_ ? av_find_best_stream(context_.get(), type, -1, -1, nullptr, 0) : AVERROR(EINVAL);
}

} // namespace subcue
