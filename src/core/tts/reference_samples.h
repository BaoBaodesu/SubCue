#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <atomic>
#include <functional>

namespace subcue {
// 仅保留一个有界检测分块；输出均为临时派生文件，调用者审核后复制到声音库。
class ReferenceSamples final {
public:
    static QJsonArray prepare(const QStringList &paths, const QString &directory, QString *error,
        const std::atomic<bool> *cancel = nullptr, const std::function<void(int, int)> &progress = {});
    static QJsonArray prepare(const QStringList &paths, const QString &directory, QString *error,
        const std::atomic<bool> *cancel, const std::function<void(int, int)> &progress, bool split);
    static bool derive(const QString &source, const QString &target, double startSeconds, double endSeconds,
        bool removeDc, bool reduceNoise, QString *error, const std::atomic<bool> *cancel = nullptr);
};
}
