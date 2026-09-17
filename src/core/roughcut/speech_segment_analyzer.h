#pragma once

#include "common/app_error.h"
#include "roughcut/script_matcher.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <atomic>
#include <variant>

namespace subcue {

struct SpeechAnalysisSettings final {
    int windowMs = 20;
    int minimumSpeechMs = 100;
    int minimumSilenceMs = 200;
    int mergeGapMs = 180;
    int maximumSpeechMs = 30'000;
    double minimumStartRms = 0.003;
    double minimumEndRms = 0.002;
};

using SpeechAnalysisResult = std::variant<QVector<RecognizedPassage>, AppError>;

class SpeechSegmentAnalyzer final {
public:
    [[nodiscard]] static SpeechAnalysisResult analyzeFile(
        const QString &mediaPath, int sourceSampleRate, qint64 sourceSampleCount,
        const std::atomic<bool> *cancel = nullptr,
        const SpeechAnalysisSettings &settings = {});

    [[nodiscard]] static QVector<RecognizedPassage> analyzePcm(
        const QVector<float> &pcm16kMono, int sourceSampleRate,
        qint64 sourceSampleCount, const SpeechAnalysisSettings &settings = {});
};

} // namespace subcue
