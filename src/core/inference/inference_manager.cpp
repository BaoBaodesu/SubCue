#include "inference/inference_manager.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>

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
    const Lease lease = acquire();
    InferenceProcessResult result;
    QProcess process;
    process.start(program, arguments);
    result.started = process.waitForStarted(10'000);
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
    const auto readOutput = [&] {
        result.standardError.append(process.readAllStandardError());
        const QByteArray output = process.readAllStandardOutput();
        result.standardOutput.append(output);
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
