#include "media/ffmpeg_error.h"

extern "C" {
#include <libavutil/error.h>
}

#include <array>
#include <utility>

namespace subcue {

QString ffmpegErrorString(int errorCode)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) {
        return QStringLiteral("Unknown FFmpeg error (%1)").arg(errorCode);
    }
    return QString::fromUtf8(buffer.data());
}

AppError makeFfmpegError(ErrorDomain domain, int errorCode, QString userMessage, QString context)
{
    const QString detail = context.isEmpty()
        ? ffmpegErrorString(errorCode)
        : QStringLiteral("%1: %2").arg(std::move(context), ffmpegErrorString(errorCode));
    return AppError(domain, errorCode, std::move(userMessage), detail);
}

} // namespace subcue
