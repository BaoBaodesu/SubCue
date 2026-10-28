#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"

#include <QtCore/QString>

#include <optional>
#include <atomic>

namespace subcue {

class Demuxer final {
public:
    [[nodiscard]] bool open(const QString &path, AppError *error = nullptr);
    // 取消标志由调用者持有，必须存活到 close；本地入口禁止嵌套网络协议。
    [[nodiscard]] bool openLocal(const QString &path, const std::atomic<bool> *cancel, AppError *error = nullptr);
    void close();

    [[nodiscard]] PacketPtr readPacket(AppError *error = nullptr);
    [[nodiscard]] bool seek(int streamIndex, qint64 timestamp, AppError *error = nullptr);

    [[nodiscard]] AVFormatContext *context() const noexcept { return context_.get(); }
    [[nodiscard]] const AVStream *stream(int index) const noexcept;
    [[nodiscard]] int bestStream(AVMediaType type) const noexcept;

private:
    bool openInput(const QString &path, const std::atomic<bool> *cancel, bool localOnly, AppError *error);
    FormatContextPtr context_;
};

} // namespace subcue
