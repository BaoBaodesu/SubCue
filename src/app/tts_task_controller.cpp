#include "tts_task_controller.h"
#include "application_context.h"
#include "project_session.h"
#include "project/project_store.h"
#include "tts/audio_quality.h"
#include <QtCore/QPromise>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <stdexcept>

namespace subcue {
TtsTaskController::TtsTaskController(ApplicationContext *context, ProjectSession *session, QObject *parent)
    : QObject(parent), context_(context), session_(session)
{
    workers_.setMaxThreadCount(1);
    connect(this, &TtsTaskController::busyChanged, this, [this] { if (!busy_) context_->clearCloudTask(this); });
    connect(session_, &ProjectSession::aboutToChange, this, &TtsTaskController::cancelAndWait);
}
TtsTaskController::~TtsTaskController() { cancelAndWait(); workers_.waitForDone(); }
void TtsTaskController::cancel() { cancel_ = true; }
void TtsTaskController::cancelAndWait()
{
    cancel(); task_.waitForFinished(); ++epoch_;
    recordsTask_.waitForFinished(); ++recordsEpoch_;
    segmentStates_.clear();
    records_.clear(); recordsError_.clear(); recordsLoading_ = false; emit recordsChanged();
    if (busy_) { busy_ = false; emit busyChanged(); }
}
void TtsTaskController::loadRecords(int offset, int limit, const QString &batchId)
{
    if (recordsLoading_ || session_->assetMutationBlocked()) return;
    if (!session_->unified() || session_->storagePath().isEmpty()) {
        records_.clear(); recordsError_ = QStringLiteral("请新建或打开统一工程以查看配音任务。"); emit recordsChanged(); return;
    }
    const QString path = session_->storagePath();
    const auto epoch = ++recordsEpoch_;
    recordsLoading_ = true; recordsError_.clear(); emit recordsChanged();
    const QPointer<TtsTaskController> self(this);
    recordsTask_ = context_->submitProjectStorage([self, path, epoch, offset, limit, batchId] {
        QString error;
        const auto records = ProjectStore(path).jobs(offset, limit, &error, batchId).toVariantList();
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, records, error] {
            if (!self || self->recordsEpoch_ != epoch) return;
            self->records_ = records; self->recordsError_ = error; self->recordsLoading_ = false; emit self->recordsChanged();
        }, Qt::QueuedConnection);
    });
}
bool TtsTaskController::start(std::variant<LocalTtsProvider, CloudTtsProvider> provider, QVector<TtsJob> jobs)
{
    const auto capabilities = std::visit([](const auto &value) { return value.capabilities(); }, provider);
    if (busy_ || session_->busy() || !session_->unified() || session_->storagePath().isEmpty() || jobs.isEmpty() || jobs.size() > 500) {
        status_ = busy_ ? QStringLiteral("已有批次执行中，请等待或取消。")
            : QStringLiteral("请等待当前存储操作完成，并选择 1–500 个句段。");
        emit statusChanged(); return false;
    }
    for (const auto &job : jobs) {
        if (job.input.projectId != session_->projectId() || job.input.projectGeneration != session_->generation()) {
            status_ = QStringLiteral("任务不属于当前工程或代次。"); emit statusChanged(); return false;
        }
        if (!job.input.validate(capabilities, &status_)) { emit statusChanged(); return false; }
    }
    const QString path = session_->storagePath();
    const QString batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const quint64 epoch = ++epoch_;
    if (capabilities.cloud && !jobs.first().billing.value(QStringLiteral("cloudAccountId")).toString().isEmpty()
        && !context_->trackCloudTask(this, jobs.first().billing.value(QStringLiteral("cloudAccountId")).toString(), [self = QPointer<TtsTaskController>(this)] {
            if (self) self->cancelAndWait();
        })) { status_ = QStringLiteral("正在修改云端凭据，请稍后启动任务。"); emit statusChanged(); return false; }
    segmentStates_.clear();
    for (const auto &job : jobs) segmentStates_.insert(job.input.segmentId, QStringLiteral("generating"));
    cancel_ = false; busy_ = true; status_ = QStringLiteral("正在排队生成；编辑与工作区切换可继续。");
    emit busyChanged(); emit statusChanged();
    auto promise = std::make_shared<QPromise<void>>(); promise->start(); task_ = promise->future();
    const QPointer<TtsTaskController> self(this);
    auto *context = context_; auto *cancel = &cancel_;
    workers_.start([self, context, cancel, epoch, path, batchId, promise, capabilities, provider = std::move(provider), jobs = std::move(jobs)]() mutable {
        QString error;
        QTemporaryDir output(QFileInfo(path).absoluteDir().filePath(QStringLiteral(".subcue-tts-XXXXXX")));
        QVector<QString> enqueued;
        QSet<QString> committed;
        bool retained = false;
        TtsBatchReport report;
        QJsonArray qualityFailures;
        const auto storage = [&](const std::function<void()> &operation) {
            auto pending = context->submitProjectStorage(operation);
            pending.waitForFinished();
        };
        try {
            storage([&] {
                ProjectStore store(path);
                const auto project = store.load(&error);
                if (!project || project->projectId != jobs.first().input.projectId) {
                    error = QStringLiteral("工程身份发生变化，未启动生成。"); return;
                }
                const auto assets = capabilities.referenceRequired ? store.assets(&error) : QJsonArray{};
                if (!error.isEmpty()) return;
                for (auto &job : jobs) {
                    if (cancel->load()) break;
                    if (capabilities.cloud) {
                        const auto voice = store.voiceVersion(job.input.voiceVersionId, &error);
                        const auto binding = voice ? voice->value(QStringLiteral("metadata")).toObject() : QJsonObject{};
                        const auto revision = CloudTtsProvider::voiceRevision(QFileInfo(context->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-revisions")), binding, &error);
                        if (!revision || binding.value(QStringLiteral("cloudVoiceRevision")).toString() != *revision) { if (error.isEmpty()) error = QStringLiteral("云端音色已更新，请查询并选用新版本；未派发旧输入。"); break; }
                        if (!voice || binding.value(QStringLiteral("provider")).toString() != job.input.provider
                            || binding.value(QStringLiteral("model")).toString() != job.input.model
                            || binding.value(QStringLiteral("region")).toString() != job.input.region
                            || binding.value(QStringLiteral("providerVoiceId")).toString() != job.input.providerVoiceId
                            || binding.value(QStringLiteral("businessSpace")) != job.billing.value(QStringLiteral("businessSpace"))
                            || binding.value(QStringLiteral("credentialReference")) != job.billing.value(QStringLiteral("credentialReference"))) {
                            error = QStringLiteral("云声音版本与冻结的模型、业务空间或凭据引用不一致，未派发。"); break;
                        }
                    }
                    if (capabilities.referenceRequired) {
                        const auto voice = store.voiceVersion(job.input.voiceVersionId, &error);
                        if (voice && voice->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("qualityState")).toString() != QStringLiteral("approved")) {
                            error = QStringLiteral("声音参考仍待质检，请审核并创建可用版本后生成。"); break;
                        }
                        if (!voice || voice->value(QStringLiteral("referenceAssetId")).toString() != job.input.referenceAssetId
                            || voice->value(QStringLiteral("referenceSha256")).toString() != QString::fromLatin1(job.input.referenceSha256.toHex())
                            || (capabilities.transcriptRequired
                                && voice->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("referenceText")).toString() != job.input.referenceText)) {
                            error = QStringLiteral("声音版本未保存到本项目或参考不匹配，未启动生成。"); break;
                        }
                        bool found = false;
                        for (const auto &value : assets) {
                            const auto asset = value.toObject();
                            if (asset.value(QStringLiteral("id")).toString() != job.input.referenceAssetId) continue;
                            const QString kind = asset.value(QStringLiteral("kind")).toString();
                            found = !asset.value(QStringLiteral("external")).toBool()
                                && (kind == QStringLiteral("reference") || kind == QStringLiteral("derived"))
                                && asset.value(QStringLiteral("sha256")).toString() == QString::fromLatin1(job.input.referenceSha256.toHex());
                            if (found) job.referencePath = store.resolveAudioPath(asset.value(QStringLiteral("path")).toString());
                            break;
                        }
                        if (!found) { error = QStringLiteral("声音参考未复制到本项目或版本不匹配，未启动生成。"); break; }
                    }
                    ProjectStore::JobOptions options; options.provider = job.input.provider; options.inputHash = job.input.inputHash();
                    options.batchId = batchId;
                    options.cloud = capabilities.cloud;
                    options.manualRetry = capabilities.cloud && job.billing.value(QStringLiteral("manualRetry")).toBool();
                    auto input = job.input.snapshot(); if (capabilities.cloud) input.insert(QStringLiteral("billing"), job.billing);
                    if (!store.enqueue(job.id, input, options, &error)) break;
                    enqueued.append(job.id);
                }
            });
            if (!output.isValid()) error = QStringLiteral("无法创建配音输出目录。");
            if (error.isEmpty() && !cancel->load() && enqueued.size() == jobs.size()) {
                const auto resultCallback = [&](const TtsResult &result) {
                    QString qualityError;
                    std::optional<AudioQuality> quality;
                    auto inspected = context->submitAudioProcessing([&] { quality = AudioQuality::inspect(result.audioPath, &qualityError, cancel); });
                    inspected.waitForFinished();
                    if (cancel->load()) return;
                    if (quality && (quality->sampleRate != result.sampleRate || quality->channels != result.channels || quality->sampleCount != result.sampleCount))
                        qualityError = QStringLiteral("生成结果的原生规格与实际解码样本不一致。");
                    if (quality && quality->peak == 0) qualityError = QStringLiteral("生成音频全为静音，未保存为可用候选。");
                    if (!quality || !qualityError.isEmpty()) {
                        if (qualityError.isEmpty()) qualityError = QStringLiteral("音频质检失败，未保存为可用候选。");
                        storage([&] {
                            ProjectStore store(path);
                            if (!store.recordJobResult(result.jobId, {{QStringLiteral("qualityError"), qualityError}, {QStringLiteral("mock"), result.mock}}, result.usage, &error)) return;
                            (void)store.transitionJob(result.jobId, ProjectStore::JobState::Validating, ProjectStore::JobState::Failed,
                                {{QStringLiteral("message"), qualityError}}, &error);
                        });
                        if (!error.isEmpty()) { retained = true; throw std::runtime_error("quality failure storage failed"); }
                        qualityFailures.append(QJsonObject{{QStringLiteral("jobId"), result.jobId}, {QStringLiteral("message"), qualityError}}); return;
                    }
                    const QString takeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    QString committedPath;
                    storage([&] {
                        ProjectStore store(path);
                        const auto current = store.load(&error);
                        if (!current || current->projectId != jobs.first().input.projectId) {
                            error = QStringLiteral("工程身份已变化，结果未提交。"); return;
                        }
                        if (cancel->load()) { (void)store.cancel(result.jobId); error = QStringLiteral("生成已取消。"); return; }
                        if (!store.recordJobResult(result.jobId, QJsonObject{{QStringLiteral("mock"), result.mock},
                            {QStringLiteral("sampleRate"), result.sampleRate}, {QStringLiteral("sampleCount"), result.sampleCount},
                            {QStringLiteral("elapsedMs"), result.elapsedMs}, {QStringLiteral("diagnostics"), result.diagnostics},
                            {QStringLiteral("quality"), quality->toJson()}}, result.usage, &error)
                            || !store.finish(result.jobId, takeId, result.audioPath, &error, quality->sha256, cancel)) return;
                        const auto latest = store.history(0, 1, &error);
                        if (latest.isEmpty() || latest.first().toObject().value(QStringLiteral("id")).toString() != takeId) {
                            error = QStringLiteral("已提交结果无法定位。"); return;
                        }
                        committedPath = store.resolveAudioPath(latest.first().toObject().value(QStringLiteral("audioPath")).toString());
                    });
                    if (!error.isEmpty()) { retained = !cancel->load(); throw std::runtime_error("result storage failed"); }
                    committed.insert(result.jobId);
                    QString segment;
                    for (const auto &job : jobs) if (job.id == result.jobId) { segment = job.input.segmentId; break; }
                    QMetaObject::invokeMethod(self.data(), [self, epoch, segment, id = result.jobId, takeId, committedPath, mock = result.mock] {
                        if (!self || self->epoch_ != epoch) return;
                        self->segmentStates_.insert(segment, QStringLiteral("candidate"));
                        self->status_ = QStringLiteral("批次共 %1 句 · 已完成 %2 句；新候选等待试听和应用。").arg(self->segmentStates_.size()).arg(self->segmentStates_.values().count(QVariant(QStringLiteral("candidate")))); emit self->statusChanged();
                        emit self->candidateCommitted(id, takeId, committedPath, mock);
                    }, Qt::QueuedConnection);
                };
                const auto startedCallback = [&](const QString &id) {
                    storage([&] {
                        if (!ProjectStore(path).transitionJob(id, ProjectStore::JobState::Queued, ProjectStore::JobState::Running, {}, &error)) return;
                    });
                    if (!error.isEmpty()) throw std::runtime_error("job start storage failed");
                };
                report = std::visit([&](const auto &value) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, CloudTtsProvider>) {
                        return value.generate(jobs, output.path(), cancel, resultCallback, -1, startedCallback,
                            [&](const QString &id, const QJsonObject &usage, const QJsonObject &) {
                                storage([&] { if (!ProjectStore(path).recordJobUsage(id, usage, &error)) return; });
                                if (!error.isEmpty()) throw std::runtime_error("cloud usage storage failed");
                                QMetaObject::invokeMethod(self.data(), [self, epoch, usage] {
                                    if (!self || self->epoch_ != epoch) return;
                                    self->status_ = usage.value(QStringLiteral("yuan")).isDouble() ? QStringLiteral("本句实际 Token：输入 %1，输出 %2；折算 ¥%3，本批已知 ¥%4。正在下载与校验。").arg(usage.value(QStringLiteral("inputTokens")).toInteger()).arg(usage.value(QStringLiteral("outputTokens")).toInteger()).arg(usage.value(QStringLiteral("yuan")).toDouble(), 0, 'f', 6).arg(usage.value(QStringLiteral("batchKnownYuan")).toDouble(), 0, 'f', 6) : QStringLiteral("用量未知，保留候选后暂停后续请求。"); emit self->statusChanged();
                                }, Qt::QueuedConnection);
                            }, [&](const QString &audio, QString *problem) {
                                std::optional<AudioQuality> quality;
                                auto inspecting = context->submitAudioProcessing([&] { quality = AudioQuality::inspect(audio, problem, cancel); });
                                inspecting.waitForFinished(); return quality;
                            });
                    } else return value.generate(jobs, output.path(), cancel, resultCallback, -1, startedCallback);
                }, provider);
                for (const auto &failure : qualityFailures) report.failures.append(failure);
                if (error.isEmpty()) error = report.error;
                if (error.isEmpty() && !report.failures.isEmpty()) error = QStringLiteral("部分句子生成失败，已提交候选保留。");
            }
            storage([&] {
                ProjectStore store(path);
                for (const auto &id : enqueued) {
                    bool uncertain = false;
                    for (const auto &failure : report.failures) if (failure.toObject().value(QStringLiteral("jobId")).toString() == id && failure.toObject().value(QStringLiteral("code")).toString() == QStringLiteral("outcome_unknown")) {
                        (void)store.transitionJob(id, ProjectStore::JobState::Running, ProjectStore::JobState::OutcomeUnknown, failure.toObject()); uncertain = true;
                    }
                    if (uncertain) continue;
                    if (cancel->load()) { (void)store.cancel(id); continue; }
                    QJsonObject diagnostics{{QStringLiteral("message"), error.isEmpty() ? QStringLiteral("生成未完成，请查看批次记录。") : error}};
                    for (const auto &failure : report.failures)
                        if (failure.toObject().value(QStringLiteral("jobId")).toString() == id) diagnostics = failure.toObject();
                    // 完成记录不可变；仅处理未到终态的句子。
                    if (!store.transitionJob(id, ProjectStore::JobState::Queued, ProjectStore::JobState::Failed, diagnostics))
                        if (!store.transitionJob(id, ProjectStore::JobState::Running, ProjectStore::JobState::Failed, diagnostics))
                            (void)store.transitionJob(id, ProjectStore::JobState::Validating, ProjectStore::JobState::Failed, diagnostics);
                }
            });
        } catch (...) {
            error = QStringLiteral("配音任务发生异常，已停止；云端费用可能不确定，请检查持久任务记录。"); retained = true;
            storage([&] { for (const auto &id : enqueued) {
                (void)ProjectStore(path).transitionJob(id, ProjectStore::JobState::Running, capabilities.cloud ? ProjectStore::JobState::OutcomeUnknown : ProjectStore::JobState::Interrupted, {{QStringLiteral("message"), error}});
                (void)ProjectStore(path).transitionJob(id, ProjectStore::JobState::Queued, ProjectStore::JobState::Interrupted, {{QStringLiteral("message"), error}});
                (void)ProjectStore(path).transitionJob(id, ProjectStore::JobState::Validating, ProjectStore::JobState::Interrupted, {{QStringLiteral("message"), error}});
            } });
        }
        // 在发送完成信号和完成 Future 前清理，不能把目录析构留到所有者停止等待之后。
        if (!retained && output.isValid()) {
            output.setAutoRemove(false);
            if (!QDir(output.path()).removeRecursively()) {
                // Windows 硬取消 GPU 进程后，空工作目录的内核句柄可能稍后才解除占用。
                // 只等待已清空的本任务目录；不重复扫描资产或掩盖仍存在的文件。
                if (QDir(output.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
                    QElapsedTimer released; released.start();
                    while (!QDir().rmdir(output.path()) && released.elapsed() < 2000) QThread::msleep(20);
                }
                if (QFileInfo::exists(output.path())) retained = true;
            }
        }
        if (retained) { output.setAutoRemove(false); error += QStringLiteral(" 未提交文件或清理失败目录保留在：%1").arg(output.path()); }
        const bool success = error.isEmpty() && !cancel->load() && report.success();
        const QString message = success ? QStringLiteral("候选已提交，未自动应用或覆盖编辑。")
            : (cancel->load() ? QStringLiteral("生成已取消，已提交候选保留。") + error : error);
        QMetaObject::invokeMethod(self.data(), [self, epoch, success, message, jobs, committed, cancelled = cancel->load()] {
            if (!self || self->epoch_ != epoch) return;
            for (const auto &job : jobs) {
                // Provider 返回音频不代表候选已落库；取消或写入失败不能计为完成。
                const bool completed = committed.contains(job.id);
                self->segmentStates_.insert(job.input.segmentId, completed ? QStringLiteral("candidate") : cancelled ? QStringLiteral("pending") : QStringLiteral("failed"));
            }
            self->busy_ = false; self->status_ = message + QStringLiteral(" 共 %1 句，完成 %2 句，失败 %3 句。").arg(self->segmentStates_.size()).arg(self->segmentStates_.values().count(QVariant(QStringLiteral("candidate")))).arg(self->segmentStates_.values().count(QVariant(QStringLiteral("failed")))); emit self->busyChanged(); emit self->statusChanged();
            emit self->finished(success, message);
        }, Qt::QueuedConnection);
        promise->finish();
    });
    return true;
}
} // namespace subcue

