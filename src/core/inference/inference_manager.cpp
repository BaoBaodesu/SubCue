#include "inference/inference_manager.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QThread>

namespace subcue {

InferenceManager &InferenceManager::instance()
{
    static InferenceManager manager;
    return manager;
}

InferenceManager::Lease InferenceManager::acquire()
{
    return Lease(mutex_);
}

InferenceProcessResult InferenceManager::run(
    const QString &program,
    const QStringList &arguments,
    const QByteArray &standardInput,
    const std::atomic<bool> *cancel,
    int timeoutMs,
    const std::function<void(const QByteArray &)> &event)
{
    InferenceProcessResult result;
    Lease lease(mutex_, std::defer_lock);
    QElapsedTimer waiting;
    waiting.start();
    while (!lease.try_lock()) {
        if (cancel && cancel->load()) { result.cancelled = true; return result; }
        if (timeoutMs >= 0 && waiting.elapsed() >= timeoutMs) { result.timedOut = true; return result; }
        QThread::msleep(50);
    }
    if (cancel && cancel->load()) { result.cancelled = true; return result; }
    QProcess process;
    process.start(program, arguments);
    waiting.restart();
    while (!(result.started = process.waitForStarted(100)) && process.state() == QProcess::Starting) {
        if (cancel && cancel->load()) { result.cancelled = true; break; }
        if (waiting.elapsed() >= 10'000) { result.timedOut = true; break; }
    }
    if (!result.started && process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); }
    if (!result.started) {
        result.standardError = process.readAllStandardError();
        return result;
    }
    if (!standardInput.isEmpty()) {
        process.write(standardInput);
    }
    process.closeWriteChannel();

    QElapsedTimer timer;
    timer.start();
    QByteArray pendingOutput;
    const bool keepFullStdout = !event;
    constexpr qsizetype kEventStdoutTail = 64 * 1024;
    const auto trimStdoutTail = [&] {
        if (keepFullStdout || result.standardOutput.size() <= kEventStdoutTail) return;
        result.standardOutput.remove(0, result.standardOutput.size() - kEventStdoutTail);
    };
    const auto readOutput = [&] {
        result.standardError.append(process.readAllStandardError());
        const QByteArray output = process.readAllStandardOutput();
        result.standardOutput.append(output);
        trimStdoutTail();
        pendingOutput.append(output);
        qsizetype newline = -1;
        while ((newline = pendingOutput.indexOf('\n')) >= 0) {
            const QByteArray line = pendingOutput.first(newline).trimmed();
            pendingOutput.remove(0, newline + 1);
            if (event && !line.isEmpty()) event(line);
        }
    };
    while (!process.waitForFinished(200)) {
        readOutput();
        if (cancel && cancel->load(std::memory_order_acquire)) {
            result.cancelled = true;
            process.kill();
            process.waitForFinished();
            break;
        }
        if (timeoutMs >= 0 && timer.elapsed() >= timeoutMs) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished();
            break;
        }
    }
    readOutput();
    if (event && !pendingOutput.trimmed().isEmpty()) event(pendingOutput.trimmed());
    result.exitCode = process.exitCode();
    result.standardError.append(process.readAllStandardError());
    return result;
}

} // namespace subcue
