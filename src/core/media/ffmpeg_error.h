#pragma once

#include "common/app_error.h"

#include <QtCore/QString>

namespace subcue {

[[nodiscard]] QString ffmpegErrorString(int errorCode);
[[nodiscard]] AppError makeFfmpegError(
    ErrorDomain domain,
    int errorCode,
    QString userMessage,
    QString context = {});

} // namespace subcue
