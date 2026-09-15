#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <functional>
#include <mutex>

namespace subcue {

struct InferenceProcessResult final {
    QByteArray standardOutput;
    QByteArray standardError;
    int exitCode = -1;
    bool started = false;
    bool cancelled = false;
    bool timedOut = false;
};

// 主程序内所有 GPU 推理统一经过此门禁，避免模型同时争用显存。
class InferenceManager final {
public:
    using Lease = std::unique_lock<std::recursive_mutex>;

    [[nodiscard]] static InferenceManager &instance();
    [[nodiscard]] Lease acquire();
    [[nodiscard]] InferenceProcessResult run(
        const QString &program,
        const QStringList &arguments,
        const QByteArray &standardInput = {},
        const std::atomic<bool> *cancel = nullptr,
        int timeoutMs = -1,
        const std::function<void(const QByteArray &)> &event = {});

private:
    std::recursive_mutex mutex_;
};

} // namespace subcue
