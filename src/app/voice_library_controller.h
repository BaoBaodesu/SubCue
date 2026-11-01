#pragma once
#include <QtCore/QObject>
#include <QtCore/QJsonObject>
#include <QtCore/QFuture>
#include <QtCore/QVariantList>
#include <QtCore/QUrl>
#include <atomic>
#include <functional>
#include <memory>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThreadPool>

namespace subcue {
class ApplicationContext;
class ProjectSession;
class VoiceLibrary;
class VoiceLibraryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY changed)
    Q_PROPERTY(bool creating READ creating NOTIFY changed)
    Q_PROPERTY(QVariantMap creationCapabilities READ creationCapabilities NOTIFY changed)
    Q_PROPERTY(QVariantMap creationDraft READ creationDraft NOTIFY changed)
    Q_PROPERTY(bool statusIsWarning READ statusIsWarning NOTIFY changed)
    Q_PROPERTY(qint64 previewCacheBytes READ previewCacheBytes NOTIFY changed)
    Q_PROPERTY(QVariantMap voiceCatalog READ voiceCatalog NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString selectedProfile READ selectedProfile NOTIFY changed)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY changed)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY changed)
    Q_PROPERTY(bool includeArchived READ includeArchived NOTIFY changed)
    Q_PROPERTY(int profileOffset READ profileOffset NOTIFY changed)
    Q_PROPERTY(int versionOffset READ versionOffset NOTIFY changed)
    Q_PROPERTY(QVariantMap review READ review NOTIFY changed)
    Q_PROPERTY(QVariantList samples READ samples NOTIFY changed)
    Q_PROPERTY(QVariantMap derived READ derived NOTIFY changed)
    Q_PROPERTY(QVariantMap cloudClone READ cloudClone NOTIFY changed)
public:
    VoiceLibraryController(ApplicationContext *context, ProjectSession *session, QString path = {}, QObject *parent = nullptr);
    ~VoiceLibraryController() override;
    bool previewLoading() const { return previewLoading_; }
    bool creating() const { return creating_; }
    QVariantMap creationCapabilities() const;
    QVariantMap creationDraft() const;
    bool statusIsWarning() const { return statusIsWarning_; }
    Q_INVOKABLE void beginCreation();
    Q_INVOKABLE void cancelCreation();
    Q_INVOKABLE void setCreationTranscriptRequired(bool required);
    Q_INVOKABLE void setCreationSampleSelected(int index, bool selected);
    Q_INVOKABLE void confirmCreationSample(int index, bool confirmed, const QString &language);
    Q_INVOKABLE void cropCreationSample(int index, double startSeconds, double endSeconds);
    Q_INVOKABLE void analyzeCreationSamples();
    Q_INVOKABLE void setCreationPrompt(int index);
    Q_INVOKABLE void removeCreationSample(int index);
    Q_INVOKABLE void editCreationTranscript(int index, const QString &transcript);
    Q_INVOKABLE void createFromSamples(const QString &name, bool transcriptRequired = true);
    qint64 previewCacheBytes() const;
    QVariantMap voiceCatalog() const { return catalog_.toVariantMap(); }
    Q_INVOKABLE void discoverVoices(bool refresh = true, const QString &manualId = {});
    Q_INVOKABLE void useDiscoveredVoice(const QString &id);
    Q_INVOKABLE void cancelPreview();
    Q_INVOKABLE void previewCloudVoice(const QString &id);
    bool busy() const { return busy_; }
    QString statusText() const { return status_; }
    QString selectedProfile() const { return selected_; }
    QVariantList profiles() const { return profiles_; }
    QVariantList versions() const { return versions_; }
    bool includeArchived() const { return includeArchived_; }
    int profileOffset() const { return profileOffset_; }
    int versionOffset() const { return versionOffset_; }
    QVariantMap review() const { return review_; }
    QVariantList samples() const { return samples_; }
    QVariantMap derived() const { return derived_; }
    QVariantMap cloudClone() const { return cloudClone_.toVariantMap(); }
    Q_INVOKABLE void previewCloudClone(const QString &url, const QString &prefix, double priceCny, const QString &priceSource, const QString &priceDate);
    Q_INVOKABLE void confirmCloudClone(bool rightsAccepted, bool privacyAccepted, bool costAccepted);
    Q_INVOKABLE void discardCloudClone();
    Q_INVOKABLE void reloadCloudClone();
    Q_INVOKABLE void acknowledgeCloudClone(bool checked);
    Q_INVOKABLE void useCreatedCloudClone();
    Q_INVOKABLE void previewCloudDeletion(const QString &voiceId);
    Q_INVOKABLE void confirmCloudDeletion(const QString &voiceId, bool accepted);
    Q_INVOKABLE void previewCloudUpdate(const QString &voiceId, const QString &url);
    Q_INVOKABLE void confirmCloudUpdate(const QString &voiceId, bool rightsAccepted, bool privacyAccepted, bool costAccepted);
    Q_INVOKABLE void refresh(const QString &profileId = {}, bool includeArchived = false, int profileOffset = 0, int versionOffset = 0);
    Q_INVOKABLE void createProfile(const QString &name);
    Q_INVOKABLE void renameProfile(const QString &name);
    Q_INVOKABLE void archiveProfile();
    Q_INVOKABLE void restoreProfile();
    Q_INVOKABLE void addLocalVersion(const QUrl &reference, const QString &transcript, bool transcriptRequired = true);
    Q_INVOKABLE QVariantList builtinVoices() const;
    Q_INVOKABLE void useBuiltinVoice(const QString &voiceId, bool currentProfile = false);
    Q_INVOKABLE void addCloudVersion(const QString &voiceId, const QString &businessSpace = {}, const QString &credentialReference = {});
    Q_INVOKABLE void savePreset(const QString &versionId, const QString &name, const QString &usageScope);
    Q_INVOKABLE void copyToProject(const QString &versionId);
    Q_INVOKABLE void setDefaultVersion(const QString &versionId);
    Q_INVOKABLE void inspectVersion(const QString &versionId);
    Q_INVOKABLE void recognizeEmotion(const QString &python, const QString &modelDirectory, bool sampleBatch = false);
    Q_INVOKABLE void discardEmotionPrediction();
    Q_INVOKABLE void approveReview(const QString &transcript, bool acceptWarnings, const QString &referenceLanguage = {}, const QString &manualEmotion = {}, bool emotionReviewed = false, bool transcriptRequired = true);
    Q_INVOKABLE void prepareSamples(const QList<QUrl> &files, bool split = true);
    Q_INVOKABLE void editSample(int index, const QString &transcript, bool selected);
    Q_INVOKABLE void transcribeSamples();
    Q_INVOKABLE void saveSamples(bool transcriptRequired = true);
    Q_INVOKABLE void deriveReview(double startSeconds, double endSeconds, bool removeDc, bool reduceNoise);
    Q_INVOKABLE void approveDerived(const QString &transcript, bool acceptWarnings, bool transcriptReviewed = false, const QString &referenceLanguage = {}, const QString &manualEmotion = {}, bool transcriptRequired = true);
    Q_INVOKABLE void cancel();
    void cancelAndWait();
signals:
    void changed();
    void operationFinished(bool success);
    void auditionRequested(const QString &path, const QString &sha256);
    void voiceSelected(const QString &versionId, const QString &name);
private:
    void confirmCloudVoiceMutation(const QString &voiceId, bool accepted, bool update);
    void storeCloudVoice(const QJsonObject &metadata, const QString &name, bool currentProfile = false);
    void submit(std::function<bool(VoiceLibrary &, QString &, QString &)> operation, const QString &successText,
        int profileOffset = -1, int versionOffset = 0, int includeArchived = -1);
    ApplicationContext *context_;
    ProjectSession *session_;
    QString path_;
    QString selected_;
    QString status_;
    QVariantList profiles_;
    QVariantList versions_;
    QFuture<void> task_;
    QFuture<void> audioTask_;
    QVariantMap review_;
    QVariantList samples_;
    QVariantMap derived_;
    std::shared_ptr<QTemporaryDir> sampleDirectory_;
    std::shared_ptr<QTemporaryDir> derivedDirectory_;
    QString samplesProfile_;
    QThreadPool requests_;
    QJsonObject catalog_;
    QJsonObject cloudClone_;
    QVariantMap cloudPreview_;
    std::shared_ptr<QTemporaryDir> cloudPreviewDirectory_;
    QByteArray catalogIdentity_;
    std::atomic<bool> cancel_{false};
    quint64 epoch_ = 0;
    bool busy_ = false;
    std::atomic<bool> previewCancel_{false};
    bool previewLoading_ = false;
    bool creating_ = false;
    bool committingCreation_ = false;
    bool creationTranscriptRequired_ = true;
    bool creationAnalyzed_ = false;
    int creationPromptIndex_ = -1;
    bool statusIsWarning_ = false;
    bool cloudVoiceMutation_ = false;
    bool includeArchived_ = false;
    int profileOffset_ = 0;
    int versionOffset_ = 0;
};
}
