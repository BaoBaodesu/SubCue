#pragma once

#include "tts/tts_types.h"
#include <QtCore/QVector>
#include <QtCore/QJsonArray>
#include <functional>
#include <atomic>

namespace subcue {
struct TtsJob {
    QString id;
    TtsRequest input;
    QString referencePath;
    QString simulation; // 仅 Mock 故障测试使用。
    QJsonObject billing; // 云端价格快照不参与音频输入指纹。
};
struct TtsBatchReport {
    QVector<TtsResult> results;
    QJsonArray failures;
    QString error;
    bool canceled = false;
    [[nodiscard]] bool success() const { return error.isEmpty() && !canceled && failures.isEmpty(); }
};

// 同步 Provider 由任务所有者放到后台调用；回调在调用线程执行。
class LocalTtsProvider final {
public:
    enum class Kind { QwenBase, Mock, Index25, GptProPlus };
    LocalTtsProvider(Kind kind, QString python, QString worker, QString modelDirectory = {});
    [[nodiscard]] TtsCapabilities capabilities() const;
    [[nodiscard]] QJsonObject check(QString *error, const std::atomic<bool> *cancel = nullptr) const;
    [[nodiscard]] TtsBatchReport generate(const QVector<TtsJob> &jobs, const QString &outputDirectory,
        const std::atomic<bool> *cancel = nullptr,
        const std::function<void(const TtsResult &)> &onResult = {}, int timeoutMs = -1,
        const std::function<void(const QString &)> &onStarted = {}) const;
private:
    Kind kind_;
    QString python_;
    QString worker_;
    QString modelDirectory_;
};
} // namespace subcue
