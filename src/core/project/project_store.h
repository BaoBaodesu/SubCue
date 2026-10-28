#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <optional>
#include <atomic>

namespace subcue {

// 编辑检查点与自动提交的生成记录共享数据库，但分别提交。
class ProjectStore final {
public:
    enum class BackupMode { ExternalMediaReferences, Portable };
    enum class JobState { Queued, Running, Validating, Completed, Failed, Canceled, Interrupted, OutcomeUnknown };
    struct JobOptions {
        QString provider;
        QString batchId;
        QString retryOf;
        QByteArray inputHash;
        bool manualRetry = false;
        bool cloud = false;
    };
    struct Snapshot {
        QString projectId;
        qint64 revision = 0;
        QJsonObject modules;
    };
    explicit ProjectStore(QString path);
    [[nodiscard]] bool create(QString *error = nullptr);
    [[nodiscard]] bool create(const QString &projectId, QString *error = nullptr);
    [[nodiscard]] std::optional<Snapshot> load(QString *error = nullptr) const;
    // 仅在事务提交成功后回传版本，失败不修改调用方的版本。
    [[nodiscard]] bool save(const Snapshot &snapshot, QString *error = nullptr,
                            qint64 *committedRevision = nullptr,
                            const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] bool enqueue(const QString &jobId, const QJsonObject &input, QString *error = nullptr);
    [[nodiscard]] bool enqueue(const QString &jobId, const QJsonObject &input,
        const JobOptions &options, QString *error = nullptr);
    [[nodiscard]] bool transitionJob(const QString &jobId, JobState expected, JobState next,
        const QJsonObject &diagnostics = {}, QString *error = nullptr);
    [[nodiscard]] bool recordJobResult(const QString &jobId, const QJsonObject &result,
        const QJsonObject &usage, QString *error = nullptr);
    [[nodiscard]] bool recordJobUsage(const QString &jobId, const QJsonObject &usage, QString *error = nullptr);
    [[nodiscard]] QJsonArray jobs(int offset = 0, int limit = 50, QString *error = nullptr, const QString &batchId = {}) const;
    // 仅在取得工程会话后调用；恢复状态，不重新派发任何推理或云请求。
    [[nodiscard]] std::optional<int> recoverJobs(QString *error = nullptr);
    [[nodiscard]] static QString jobStateName(JobState state);
    [[nodiscard]] bool finish(const QString &jobId, const QString &takeId,
                              const QString &audioPath, QString *error = nullptr);
    [[nodiscard]] bool finish(const QString &jobId, const QString &takeId, const QString &audioPath,
        QString *error, const QByteArray &expectedSha256, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] bool cancel(const QString &jobId, QString *error = nullptr);
    [[nodiscard]] QJsonArray history(int offset = 0, int limit = 50, QString *error = nullptr) const;
    QJsonArray voiceBindings(const QStringList &versionIds, QString *error = nullptr) const;
    QJsonObject voiceGenerationStates(const QJsonObject &document, QString *error = nullptr, const std::atomic<bool> *cancel = nullptr) const;
    [[nodiscard]] QJsonArray segmentTakes(const QString &segmentId, int offset = 0, int limit = 50, QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> importAsset(const QString &sourcePath, const QString &kind,
        QString *error = nullptr, const std::atomic<bool> *cancel = nullptr,
        const QByteArray &expectedSha256 = {});
    [[nodiscard]] QJsonArray assets(QString *error = nullptr) const;
    [[nodiscard]] bool importVoiceVersion(const QString &voiceId, const QString &versionId,
        const QString &referencePath, const QByteArray &referenceHash, const QJsonObject &metadata,
        QString *error = nullptr, const std::atomic<bool> *cancel = nullptr);
    [[nodiscard]] std::optional<QJsonObject> voiceVersion(const QString &versionId, QString *error = nullptr) const;
    [[nodiscard]] QString resolveAudioPath(const QString &storedPath) const;
    [[nodiscard]] bool backup(const QString &destination, QString *error = nullptr,
        BackupMode mode = BackupMode::ExternalMediaReferences,
        const std::atomic<bool> *cancel = nullptr) const;
private:
    QString path_;
};

} // namespace subcue
