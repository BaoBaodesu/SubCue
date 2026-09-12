#include "playback/frame_index.h"

#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"
#include "media/ffmpeg_time.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace subcue {

bool FrameIndex::build(const QString &path, AppError *error)
{
    clear();
    Demuxer demuxer;
    if (!demuxer.open(path, error)) {
        return false;
    }
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_VIDEO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, streamIndex, QStringLiteral("媒体中没有可索引的视频流"), path);
        }
        return false;
    }

    Decoder decoder;
    if (!decoder.open(*stream, error)) {
        return false;
    }

    qint64 keyframeTimestamp = timestampFromMediaTime(MediaTime::fromMicroseconds(0), stream->time_base);
    if (keyframeTimestamp == AV_NOPTS_VALUE) {
        keyframeTimestamp = 0;
    }
    FramePtr frame = makeFrame();
    if (!frame) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配视频帧"));
        }
        return false;
    }

    PacketPtr pending;
    bool flushed = false;
    int frameIndex = 0;
    for (;;) {
        const int received = decoder.receive(frame.get());
        if (received >= 0) {
            if (!decoder.ensureSoftwareFrame(frame.get(), error)) {
                return false;
            }
            FrameIndexEntry entry;
            entry.index = frameIndex++;
            entry.pts = mediaTimeFromTimestamp(frame->best_effort_timestamp, stream->time_base);
            entry.seekTimestamp = keyframeTimestamp;
            entry.keyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
            if (entry.keyframe && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                entry.seekTimestamp = frame->best_effort_timestamp;
                keyframeTimestamp = entry.seekTimestamp;
            }
            entries_.push_back(entry);
            av_frame_unref(frame.get());
            continue;
        }
        if (received != AVERROR(EAGAIN) && received != AVERROR_EOF) {
            if (error) {
                *error = makeFfmpegError(ErrorDomain::Decoder, received, QStringLiteral("建立帧索引失败"));
            }
            return false;
        }

        if (!pending) {
            if (flushed) {
                break;
            }
            pending = demuxer.readPacket(error);
            while (pending && pending->stream_index != streamIndex) {
                pending = demuxer.readPacket(error);
            }
            if (!pending) {
                const int flushedSend = decoder.send(nullptr);
                flushed = true;
                if (flushedSend < 0 && flushedSend != AVERROR(EAGAIN) && flushedSend != AVERROR_EOF) {
                    if (error) {
                        *error = makeFfmpegError(ErrorDomain::Decoder, flushedSend, QStringLiteral("无法刷新帧索引解码器"));
                    }
                    return false;
                }
                continue;
            }
            if ((pending->flags & AV_PKT_FLAG_KEY) != 0) {
                keyframeTimestamp = pending->pts != AV_NOPTS_VALUE ? pending->pts : pending->dts;
                if (keyframeTimestamp == AV_NOPTS_VALUE) {
                    keyframeTimestamp = 0;
                }
            }
        }

        const int sent = decoder.send(pending.get());
        if (sent == AVERROR(EAGAIN)) {
            continue;
        }
        if (sent < 0) {
            if (error) {
                *error = makeFfmpegError(ErrorDomain::Decoder, sent, QStringLiteral("提交帧索引数据失败"));
            }
            return false;
        }
        pending.reset();
    }

    if (entries_.isEmpty()) {
        if (error) {
            *error = AppError(ErrorDomain::Decoder, AVERROR_EOF, QStringLiteral("媒体中没有可索引的视频帧"), path);
        }
        return false;
    }
    return true;
}

void FrameIndex::clear()
{
    entries_.clear();
}

int FrameIndex::findIndexAtOrAfter(MediaTime pts) const
{
    for (int index = 0; index < entries_.size(); ++index) {
        if (entries_.at(index).pts.microseconds() >= pts.microseconds()) {
            return index;
        }
    }
    return entries_.isEmpty() ? -1 : entries_.size() - 1;
}

int FrameIndex::keyframeIndexAtOrBefore(int frameIndex) const
{
    if (entries_.isEmpty() || frameIndex < 0) {
        return -1;
    }
    const int lastIndex = static_cast<int>(entries_.size()) - 1;
    int index = frameIndex < lastIndex ? frameIndex : lastIndex;
    while (index > 0 && !entries_.at(index).keyframe) {
        --index;
    }
    return index;
}

} // namespace subcue
