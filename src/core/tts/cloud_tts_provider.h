#pragma once
#include "tts/local_tts_provider.h"
#include "asr/http_client.h"
#include "tts/audio_quality.h"

namespace subcue {
// 按实际用量记录原价账本；未知用量不返回零金额。
class TtsPricing final {
public:
    static QJsonObject flashPrice();
    static std::optional<double> amount(const QJsonObject &usage, const QJsonObject &price);
};

class CloudTtsProvider final {
public:
    CloudTtsProvider(QString workspace, QString key, double budget, IHttpClient *http = nullptr);
    TtsCapabilities capabilities() const;
    static std::optional<QString> voiceRevision(const QString &directory, const QJsonObject &binding, QString *error = nullptr);
    static bool saveVoiceRevision(const QString &directory, const QJsonObject &binding, const QString &revision, bool blocked, QString *error = nullptr);
    TtsBatchReport generate(const QVector<TtsJob> &jobs, const QString &outputDirectory,
        const std::atomic<bool> *cancel, const std::function<void(const TtsResult &)> &onResult,
        int timeoutMs, const std::function<void(const QString &)> &onStarted,
        const std::function<void(const QString &, const QJsonObject &, const QJsonObject &)> &onUsage = {},
        const std::function<std::optional<AudioQuality>(const QString &, QString *)> &inspect = {}) const;
private:
    QString workspace_, key_;
    double budget_;
    IHttpClient *http_;
};
}
