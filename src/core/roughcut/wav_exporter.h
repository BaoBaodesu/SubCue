#pragma once

#include "roughcut/timeline_engine.h"

#include <QtCore/QString>

#include <atomic>

namespace subcue {

class RoughCutWavExporter final {
public:
    [[nodiscard]] static bool save(
        const QString &path, const QString &sourcePath,
        const QVector<RoughCutTimelineClip> &clips, int sampleRate, int channels,
        const std::atomic<bool> *cancel = nullptr, QString *errorMessage = nullptr,
        int fadeMs = 8);
};

} // namespace subcue
