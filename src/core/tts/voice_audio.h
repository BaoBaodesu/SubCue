#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <atomic>
#include <functional>
namespace subcue {
// 只处理冻结的 Take／编排；缓存与输出不是项目数据的权威来源。
class VoiceAudio final {
public:
    static QJsonArray plan(const QString &project, const QJsonObject &document, const QString &segment,
        bool allowStale, QString *error, QString *problemSegment = nullptr);
    static QByteArray key(const QJsonArray &clips);
    static QJsonArray render(const QJsonArray &clips, const QString &path, bool normalize,
        QString *error, const std::atomic<bool> *cancel = nullptr, const std::function<void(int, int)> &progress = {}, bool waveform = true);
};
}
