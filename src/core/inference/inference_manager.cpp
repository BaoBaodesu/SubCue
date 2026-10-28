#include "inference/inference_manager.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <chrono>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace subcue {

#ifdef Q_OS_WIN
namespace {
// 创建时加入 Job，避免虚拟环境启动器先派生子进程再被取消的竞态。
class ProcessGroup final {
public:
    bool prepare(QProcess &process)
    {
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes_.resize(static_cast<qsizetype>(bytes));
        startup_.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data());
        if (!InitializeProcThreadAttributeList(startup_.lpAttributeList, 1, 0, &bytes)) return false;
        initialized_ = true;
        if (!UpdateProcThreadAttribute(startup_.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_, sizeof(job_), nullptr, nullptr)) return false;
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments *args) {
            startup_.StartupInfo = *args->startupInfo;
            startup_.StartupInfo.cb = sizeof(startup_);
            args->startupInfo = &startup_.StartupInfo;
            args->flags |= EXTENDED_STARTUPINFO_PRESENT;
        });
        return true;
    }
    void terminate()
    {
        if (!job_) return;
        QList<HANDLE> processes;
        QByteArray data(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 16 * sizeof(ULONG_PTR), '\0');
        while (true) {
            auto *ids = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(data.data());
            if (QueryInformationJobObject(job_, JobObjectBasicProcessIdList, ids, static_cast<DWORD>(data.size()), nullptr)) {
                for (DWORD i = 0; i < ids->NumberOfProcessIdsInList; ++i) {
                    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(ids->ProcessIdList[i]))) processes.append(process);
                }
                break;
            }
            if (GetLastError() != ERROR_MORE_DATA) break;
            data.resize(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + static_cast<qsizetype>(ids->NumberOfAssignedProcesses) * sizeof(ULONG_PTR));
        }
        TerminateJobObject(job_, 1);
        // 活动计数归零可能早于进程对象发出退出信号，返回前等待所持有的子进程句柄。
        for (const HANDLE process : processes) {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }
    }
    ~ProcessGroup()
    {
        if (job_) {
            terminate();
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            while (QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses > 0) QThread::msleep(10);
            CloseHandle(job_);
        }
        if (initialized_) DeleteProcThreadAttributeList(startup_.lpAttributeList);
    }
private:
    HANDLE job_ = nullptr;
    STARTUPINFOEXW startup_{};
    QByteArray attributes_;
    bool initialized_ = false;
};
}
#endif

InferenceManager &InferenceManager::instance()
{
    static InferenceManager manager;
    return manager;
}

InferenceManager::Lease InferenceManager::acquire()
{
    return std::move(*acquire(nullptr, -1));
}

InferenceManager::Lease::Lease(Lease &&other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)) {}

InferenceManager::Lease &InferenceManager::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        if (manager_) manager_->release();
        manager_ = std::exchange(other.manager_, nullptr);
    }
    return *this;
}

InferenceManager::Lease::~Lease() { if (manager_) manager_->release(); }

std::optional<InferenceManager::Lease> InferenceManager::acquire(const std::atomic<bool> *cancel,
    int timeoutMs, const QString &title, const QString &batchId)
{
    QElapsedTimer waiting;
    waiting.start();
    std::unique_lock lock(mutex_);
    if (cancel && cancel->load()) return {};
    if (owner_ == std::this_thread::get_id()) {
        ++depth_;
        return Lease(this);
    }
    const quint64 ticket = ++nextTicket_;
    queue_.push_back({ticket, batchId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : batchId,
        title.isEmpty() ? QStringLiteral("本地推理") : title});
    lock.unlock(); emit queueChanged(); lock.lock();
    while (true) {
        if ((cancel && cancel->load()) || (timeoutMs >= 0 && waiting.elapsed() >= timeoutMs)) {
            std::erase_if(queue_, [ticket](const Entry &entry) { return entry.ticket == ticket; });
            available_.notify_all();
            lock.unlock(); emit queueChanged();
            return {};
        }
        if (depth_ == 0 && !queue_.empty() && queue_.front().ticket == ticket) {
            active_ = queue_.front(); queue_.pop_front();
            owner_ = std::this_thread::get_id(); depth_ = 1;
            lock.unlock(); emit queueChanged();
            return Lease(this);
        }
        available_.wait_for(lock, std::chrono::milliseconds(50));
    }
}

void InferenceManager::release()
{
    std::unique_lock lock(mutex_);
    Q_ASSERT(owner_ == std::this_thread::get_id() && depth_ > 0);
    if (--depth_ > 0) return;
    owner_ = {}; active_.reset(); available_.notify_all();
    lock.unlock(); emit queueChanged();
}

QVariantList InferenceManager::tasks() const
{
    std::lock_guard lock(mutex_);
    QVariantList result;
    if (active_) result.append(QVariantMap{{QStringLiteral("id"), active_->id}, {QStringLiteral("title"), active_->title},
        {QStringLiteral("state"), QStringLiteral("running")}, {QStringLiteral("position"), 0}});
    int position = 1;
    for (const auto &entry : queue_) result.append(QVariantMap{{QStringLiteral("id"), entry.id}, {QStringLiteral("title"), entry.title},
        {QStringLiteral("state"), QStringLiteral("queued")}, {QStringLiteral("position"), position++}});
    return result;
}

InferenceProcessResult InferenceManager::run(
    const QString &program,
    const QStringList &arguments,
    const QByteArray &standardInput,
    const std::atomic<bool> *cancel,
    int timeoutMs,
    const std::function<void(const QByteArray &)> &event, const QString &taskTitle, bool gpuTask,
    const std::function<bool()> &stopRequested)
{
    InferenceProcessResult result;
    std::optional<Lease> lease;
    if (gpuTask) {
        lease = acquire(cancel, timeoutMs, taskTitle);
        if (!lease) {
            result.cancelled = cancel && cancel->load();
            result.timedOut = !result.cancelled;
            return result;
        }
    }
    QElapsedTimer waiting;
    waiting.start();
    if (cancel && cancel->load()) { result.cancelled = true; return result; }
#ifdef Q_OS_WIN
    ProcessGroup group;
#endif
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &name : {"DASHSCOPE_API_KEY", "DEEPSEEK_API_KEY", "MOONSHOT_API_KEY", "OPENAI_API_KEY"})
        environment.remove(QString::fromLatin1(name));
    process.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
    if (!group.prepare(process)) { result.standardError = QStringLiteral("无法创建受管理的 Worker 进程组：%1").arg(GetLastError()).toUtf8(); return result; }
#endif
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
    bool discardLine = false;
    constexpr qsizetype kOutputTail = 64 * 1024;
    constexpr qsizetype kMaxLine = 1024 * 1024;
    const auto trimOutput = [&](QByteArray &bytes, qsizetype limit) {
        if (bytes.size() <= limit) return;
        bytes.remove(0, bytes.size() - limit);
        result.outputTruncated = true;
    };
    const auto readOutput = [&] {
        process.setReadChannel(QProcess::StandardError);
        while (process.bytesAvailable() > 0) {
            result.standardError.append(process.read(kOutputTail));
            trimOutput(result.standardError, kOutputTail);
        }
        process.setReadChannel(QProcess::StandardOutput);
        while (process.bytesAvailable() > 0) {
            const QByteArray output = process.read(kOutputTail);
            result.standardOutput.append(output);
            trimOutput(result.standardOutput, event ? kOutputTail : kMaxLine);
            if (!event) continue;
            pendingOutput.append(output);
            qsizetype newline = -1;
            while ((newline = pendingOutput.indexOf('\n')) >= 0) {
                if (newline > kMaxLine) result.outputTruncated = true;
                const QByteArray line = discardLine || newline > kMaxLine ? QByteArray{} : pendingOutput.first(newline).trimmed();
                pendingOutput.remove(0, newline + 1);
                discardLine = false;
                if (!line.isEmpty()) event(line);
            }
            if (pendingOutput.size() > kMaxLine) {
                pendingOutput.clear(); discardLine = true; result.outputTruncated = true;
            }
        }
    };
    while (!process.waitForFinished(200)) {
        readOutput();
        if ((cancel && cancel->load(std::memory_order_acquire)) || (stopRequested && stopRequested())) {
            result.cancelled = true;
#ifdef Q_OS_WIN
            group.terminate();
#endif
            process.kill();
            process.waitForFinished();
            break;
        }
        if (timeoutMs >= 0 && timer.elapsed() >= timeoutMs) {
            result.timedOut = true;
#ifdef Q_OS_WIN
            group.terminate();
#endif
            process.kill();
            process.waitForFinished();
            break;
        }
    }
    readOutput();
    if (event && !discardLine && !pendingOutput.trimmed().isEmpty()) event(pendingOutput.trimmed());
    result.exitCode = process.exitCode();
    return result;
}

} // namespace subcue
