#pragma once
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QJsonObject>
#include <atomic>
#include <optional>

namespace subcue {
struct AudioQuality final {
    int sampleRate = 0;
    int channels = 0;
    qint64 sampleCount = 0;
    double peak = 0;
    double rms = 0;
    double dcOffset = 0;
    double clippedFraction = 0;
    double silentFraction = 0;
    qsizetype maxFrameBytes = 0;
    QByteArray sha256;
    QStringList warnings;
    QJsonObject toJson() const;
    QJsonObject referenceReview() const;
    // 完整解码，仅保留当前帧和固定通道统计，不缓存整篇 PCM。
    static std::optional<AudioQuality> inspect(const QString &path, QString *error = nullptr,
        const std::atomic<bool> *cancel = nullptr);
};
}
