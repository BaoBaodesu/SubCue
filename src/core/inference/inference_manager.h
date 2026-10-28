#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QObject>
#include <QtCore/QVariantList>

#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>
#include <thread>

namespace subcue {

struct InferenceProcessResult final {
    QByteArray standardOutput;
    QByteArray standardError;
    int exitCode = -1;
    bool started = false;
    bool cancelled = false;
    bool timedOut = false;
    bool outputTruncated = false;
};

// 主程序内所有 GPU 推理统一经过此门禁，避免模型同时争用显存。
class InferenceManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY queueChanged)
public:
    class Lease final {
    public:
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
    private:
        friend class InferenceManager;
        explicit Lease(InferenceManager *manager) : manager_(manager) {}
        InferenceManager *manager_;
    };

    [[nodiscard]] static InferenceManager &instance();
    [[nodiscard]] Lease acquire();
    [[nodiscard]] std::optional<Lease> acquire(const std::atomic<bool> *cancel,
        int timeoutMs, const QString &title = {}, const QString &batchId = {});
    [[nodiscard]] QVariantList tasks() const;
    [[nodiscard]] InferenceProcessResult run(
        const QString &program,
        const QStringList &arguments,
        const QByteArray &standardInput = {},
        const std::atomic<bool> *cancel = nullptr,
        int timeoutMs = -1,
        const std::function<void(const QByteArray &)> &event = {},
        const QString &taskTitle = {}, bool gpuTask = true,
        const std::function<bool()> &stopRequested = {});

signals:
    void queueChanged();

private:
    void release();
    struct Entry { quint64 ticket; QString id; QString title; };
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Entry> queue_;
    std::thread::id owner_;
    std::optional<Entry> active_;
    quint64 nextTicket_ = 0;
    int depth_ = 0;
};

} // namespace subcue
