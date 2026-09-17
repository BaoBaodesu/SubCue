#include "roughcut/speech_segment_analyzer.h"

#include "asr/asr_types.h"
#include "media/audio_resampler.h"
#include "media/decoder.h"
#include "media/demuxer.h"
#include "media/ffmpeg_error.h"

#include <algorithm>
#include <cmath>

namespace subcue {
namespace {

class Detector final {
public:
    Detector(int sourceRate, qint64 sourceCount, SpeechAnalysisSettings settings)
        : sourceRate_(sourceRate), sourceCount_(sourceCount), settings_(settings),
          windowSamples_(std::max(1, settings.windowMs * kAsrSampleRate / 1000)),
          startFrames_(std::max(1, 60 / settings.windowMs)),
          endFrames_(std::max(1, settings.minimumSilenceMs / settings.windowMs))
    {
    }

    void append(const QVector<float> &samples)
    {
        pending_.append(samples);
        while (pending_.size() >= windowSamples_) {
            processWindow(pending_.constData(), windowSamples_);
            pending_.remove(0, windowSamples_);
            processed_ += windowSamples_;
        }
    }

    QVector<RecognizedPassage> finish()
    {
        if (!pending_.isEmpty()) {
            processWindow(pending_.constData(), pending_.size());
            processed_ += pending_.size();
            pending_.clear();
        }
        if (active_) closeSegment(processed_);
        mergeNearby();
        qint64 previousEnd = 0;
        for (int index = 0; index < result_.size(); ++index) {
            RecognizedPassage &segment = result_[index];
            segment.id = QStringLiteral("speech-%1").arg(index + 1);
            segment.silenceBeforeSamples = std::max<qint64>(0, segment.startSample - previousEnd);
            segment.silenceAfterSamples = index + 1 < result_.size()
                ? std::max<qint64>(0, result_.at(index + 1).startSample - segment.endSample)
                : std::max<qint64>(0, sourceCount_ - segment.endSample);
            previousEnd = segment.endSample;
        }
        return result_;
    }

private:
    qint64 sourceSample(qint64 pcmSample) const
    {
        return std::clamp<qint64>((pcmSample * sourceRate_ + kAsrSampleRate / 2)
                / kAsrSampleRate, 0, sourceCount_);
    }

    void processWindow(const float *samples, qsizetype count)
    {
        double sum = 0.0;
        double peak = 0.0;
        for (qsizetype index = 0; index < count; ++index) {
            const double value = samples[index];
            sum += value * value;
            peak = std::max(peak, std::abs(value));
        }
        const double rms = std::sqrt(sum / std::max<qsizetype>(1, count));
        const double startThreshold = std::max(settings_.minimumStartRms, noiseFloor_ * 3.0);
        const double endThreshold = std::max(settings_.minimumEndRms, noiseFloor_ * 1.8);
        if (!active_) {
            noiseFloor_ = std::clamp(noiseFloor_ * 0.98 + rms * 0.02, 0.0002, 0.02);
            if (rms >= startThreshold || peak >= startThreshold * 4.0) {
                if (++startRun_ >= startFrames_) {
                    active_ = true;
                    segmentStart_ = std::max<qint64>(0,
                        processed_ + count - static_cast<qint64>(startRun_) * windowSamples_);
                    segmentEnergy_ = rms;
                    segmentWindows_ = 1;
                    endRun_ = 0;
                }
            } else {
                startRun_ = 0;
            }
            return;
        }
        segmentEnergy_ += rms;
        ++segmentWindows_;
        const qint64 maximum = static_cast<qint64>(settings_.maximumSpeechMs)
            * kAsrSampleRate / 1000;
        if (maximum > 0 && processed_ + count - segmentStart_ >= maximum) {
            closeSegment(processed_ + count);
            return;
        }
        if (rms < endThreshold && peak < endThreshold * 4.0) {
            if (++endRun_ >= endFrames_) {
                closeSegment(processed_ + count - static_cast<qint64>(endRun_) * windowSamples_);
            }
        } else {
            endRun_ = 0;
        }
    }

    void closeSegment(qint64 endPcm)
    {
        const qint64 minimum = static_cast<qint64>(settings_.minimumSpeechMs)
            * kAsrSampleRate / 1000;
        if (endPcm - segmentStart_ >= minimum) {
            const double average = segmentEnergy_ / std::max(1, segmentWindows_);
            result_.append({{}, {}, sourceSample(segmentStart_), sourceSample(endPcm),
                0, 0, std::clamp(average / std::max(0.001, noiseFloor_ * 6.0), 0.0, 1.0),
                true, true});
        }
        active_ = false;
        startRun_ = 0;
        endRun_ = 0;
        segmentEnergy_ = 0.0;
        segmentWindows_ = 0;
    }

    void mergeNearby()
    {
        if (result_.size() < 2) return;
        const qint64 mergeGap = static_cast<qint64>(settings_.mergeGapMs) * sourceRate_ / 1000;
        QVector<RecognizedPassage> merged;
        for (RecognizedPassage segment : result_) {
            const qint64 gap = merged.isEmpty() ? -1
                : segment.startSample - merged.constLast().endSample;
            if (gap > 0 && gap <= mergeGap) {
                merged.last().endSample = segment.endSample;
                merged.last().vadConfidence = std::max(merged.constLast().vadConfidence,
                                                       segment.vadConfidence);
            } else {
                merged.append(std::move(segment));
            }
        }
        result_ = std::move(merged);
    }

    int sourceRate_ = 0;
    qint64 sourceCount_ = 0;
    SpeechAnalysisSettings settings_;
    int windowSamples_ = 0;
    int startFrames_ = 0;
    int endFrames_ = 0;
    QVector<float> pending_;
    QVector<RecognizedPassage> result_;
    qint64 processed_ = 0;
    qint64 segmentStart_ = 0;
    double noiseFloor_ = 0.0005;
    double segmentEnergy_ = 0.0;
    int segmentWindows_ = 0;
    int startRun_ = 0;
    int endRun_ = 0;
    bool active_ = false;
};

} // namespace

QVector<RecognizedPassage> SpeechSegmentAnalyzer::analyzePcm(
    const QVector<float> &pcm16kMono, int sourceSampleRate,
    qint64 sourceSampleCount, const SpeechAnalysisSettings &settings)
{
    if (pcm16kMono.isEmpty() || sourceSampleRate <= 0 || sourceSampleCount <= 0) return {};
    Detector detector(sourceSampleRate, sourceSampleCount, settings);
    detector.append(pcm16kMono);
    return detector.finish();
}

SpeechAnalysisResult SpeechSegmentAnalyzer::analyzeFile(
    const QString &mediaPath, int sourceSampleRate, qint64 sourceSampleCount,
    const std::atomic<bool> *cancel, const SpeechAnalysisSettings &settings)
{
    if (sourceSampleRate <= 0 || sourceSampleCount <= 0) {
        return AppError(ErrorDomain::Validation, AVERROR(EINVAL), QStringLiteral("源音频采样信息无效"));
    }
    AppError error(ErrorDomain::Media, 0, QString());
    Demuxer demuxer;
    if (!demuxer.open(mediaPath, &error)) return error;
    const int streamIndex = demuxer.bestStream(AVMEDIA_TYPE_AUDIO);
    const AVStream *stream = demuxer.stream(streamIndex);
    if (!stream) return AppError(ErrorDomain::Decoder, streamIndex, QStringLiteral("媒体中没有音频流"));
    Decoder decoder;
    if (!decoder.open(*stream, &error)) return error;
    AudioResampler resampler;
    if (!resampler.configure(*decoder.context(), kAsrSampleRate, 1, &error)) return error;
    FramePtr frame = makeFrame();
    if (!frame) return AppError(ErrorDomain::Decoder, AVERROR(ENOMEM), QStringLiteral("无法分配音频帧"));
    Detector detector(sourceSampleRate, sourceSampleCount, settings);
    bool draining = false;
    for (;;) {
        if (cancel && cancel->load()) return asrCancelledError();
        PacketPtr packet;
        if (!draining) {
            packet = demuxer.readPacket(&error);
            if (!packet) {
                draining = true;
                const int sent = decoder.send(nullptr);
                if (sent < 0 && sent != AVERROR_EOF) return makeFfmpegError(
                    ErrorDomain::Decoder, sent, QStringLiteral("无法刷新音频解码器"));
            } else if (packet->stream_index != streamIndex) {
                continue;
            } else {
                const int sent = decoder.send(packet.get());
                if (sent < 0 && sent != AVERROR(EAGAIN)) return makeFfmpegError(
                    ErrorDomain::Decoder, sent, QStringLiteral("提交音频数据失败"));
            }
        }
        for (;;) {
            const int received = decoder.receive(frame.get());
            if (received == AVERROR(EAGAIN)) break;
            if (received == AVERROR_EOF) return detector.finish();
            if (received < 0) return makeFfmpegError(
                ErrorDomain::Decoder, received, QStringLiteral("音频解码失败"));
            QVector<float> samples = resampler.convert(*frame, &error);
            av_frame_unref(frame.get());
            if (samples.isEmpty()) return error;
            detector.append(samples);
        }
        if (draining) return detector.finish();
    }
}

} // namespace subcue
