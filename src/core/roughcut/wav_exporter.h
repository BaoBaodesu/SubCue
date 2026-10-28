#pragma once

#include "roughcut/timeline_engine.h"

#include <QtCore/QString>

#include <atomic>
#include <functional>

namespace subcue {

class RoughCutWavExporter final {
public:
    // 固定缓冲流式转换，坐标使用输出采样率；供多素材配音编排复用。
    [[nodiscard]] static bool stream(const QString &sourcePath, qint64 startSample, qint64 endSample,
        const std::function<bool(QVector<float>, qint64)> &write, const std::atomic<bool> *cancel,
        QString *errorMessage);
    [[nodiscard]] static bool save(
        const QString &path, const QString &sourcePath,
        const QVector<RoughCutTimelineClip> &clips, int sampleRate, int channels,
        const std::atomic<bool> *cancel = nullptr, QString *errorMessage = nullptr,
        int fadeMs = 0);
};

} // namespace subcue
