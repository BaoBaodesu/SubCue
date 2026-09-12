#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QString>

#include <optional>

namespace subcue {

class Demuxer final {
public:
    [[nodiscard]] bool open(const QString &path, AppError *error = nullptr);
    void close();

    [[nodiscard]] PacketPtr readPacket(AppError *error = nullptr);
    [[nodiscard]] bool seek(int streamIndex, qint64 timestamp, AppError *error = nullptr);

    [[nodiscard]] AVFormatContext *context() const noexcept { return context_.get(); }
    [[nodiscard]] const AVStream *stream(int index) const noexcept;
    [[nodiscard]] int bestStream(AVMediaType type) const noexcept;

private:
    FormatContextPtr context_;
};

} // namespace subcue
