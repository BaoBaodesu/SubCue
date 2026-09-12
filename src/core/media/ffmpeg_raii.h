#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>

namespace subcue {

struct FormatContextDeleter final {
    void operator()(AVFormatContext *context) const noexcept
    {
        avformat_close_input(&context);
    }
};

struct CodecContextDeleter final {
    void operator()(AVCodecContext *context) const noexcept
    {
        avcodec_free_context(&context);
    }
};

struct PacketDeleter final {
    void operator()(AVPacket *packet) const noexcept
    {
        av_packet_free(&packet);
    }
};

struct FrameDeleter final {
    void operator()(AVFrame *frame) const noexcept
    {
        av_frame_free(&frame);
    }
};

struct SwrContextDeleter final {
    void operator()(SwrContext *context) const noexcept
    {
        swr_free(&context);
    }
};

struct SwsContextDeleter final {
    void operator()(SwsContext *context) const noexcept
    {
        sws_freeContext(context);
    }
};

struct BufferRefDeleter final {
    void operator()(AVBufferRef *ref) const noexcept
    {
        av_buffer_unref(&ref);
    }
};

struct CodecParametersDeleter final {
    void operator()(AVCodecParameters *parameters) const noexcept
    {
        avcodec_parameters_free(&parameters);
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using CodecParametersPtr = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;

[[nodiscard]] inline PacketPtr makePacket()
{
    return PacketPtr(av_packet_alloc());
}

[[nodiscard]] inline FramePtr makeFrame()
{
    return FramePtr(av_frame_alloc());
}

} // namespace subcue
