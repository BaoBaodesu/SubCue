#pragma once

#include "common/app_error.h"
#include "media/ffmpeg_raii.h"
#include "media/hw_accel.h"

#include <QtCore/QVector>

namespace subcue {

class Demuxer;

class Decoder final {
public:
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    Decoder(Decoder &&) = delete;
    Decoder &operator=(Decoder &&) = delete;
    ~Decoder() = default;

    [[nodiscard]] bool open(const AVStream &stream, AppError *error = nullptr);
    [[nodiscard]] bool open(const AVStream &stream, HwAccelType accel, AppError *error = nullptr);
    [[nodiscard]] bool openWithFallback(const AVStream &stream, AppError *error = nullptr);
    [[nodiscard]] bool fallbackToSoftware(AppError *error = nullptr);
    [[nodiscard]] bool handleRuntimeFailure(AppError *error = nullptr);
    void close();
    void flush();

    [[nodiscard]] int send(const AVPacket *packet) noexcept;
    [[nodiscard]] int receive(AVFrame *frame) noexcept;
    [[nodiscard]] bool pullFrame(Demuxer &demuxer, int streamIndex, AVFrame *frame, AppError *error = nullptr);
    [[nodiscard]] bool ensureSoftwareFrame(AVFrame *frame, AppError *error = nullptr);

    [[nodiscard]] AVCodecContext *context() const noexcept { return context_.get(); }
    [[nodiscard]] HwAccelType activeType() const noexcept { return activeType_; }
    [[nodiscard]] const QVector<HwAccelAttempt> &attempts() const noexcept { return attempts_; }

private:
    [[nodiscard]] bool storeStream(const AVStream &stream, AppError *error);
    [[nodiscard]] bool openStored(HwAccelType accel, AppError *error);
    void closeContext();

    CodecContextPtr context_;
    BufferRefPtr hwDevice_;
    CodecParametersPtr codecpar_;
    AVRational pktTimeBase_{0, 1};
    HwAccelType activeType_ = HwAccelType::Software;
    AVPixelFormat hwPixelFormat_ = AV_PIX_FMT_NONE;
    QVector<HwAccelAttempt> attempts_;
};

} // namespace subcue
