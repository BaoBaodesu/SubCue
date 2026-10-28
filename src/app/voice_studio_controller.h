#pragma once
#include <QtCore/QAbstractListModel>
#include <QtCore/QJsonObject>
#include <QtCore/QFuture>
#include <QtCore/QPointer>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QUrl>
#include <QtGui/QUndoStack>
#include <atomic>
#include <memory>
#include <functional>
#include "tts/local_tts_provider.h"

namespace subcue {
class ApplicationContext;
class ProjectSession;
class TtsTaskController;
class VoiceStudioController final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantMap sessionState READ sessionState NOTIFY changed)
    Q_PROPERTY(bool modified READ modified NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool modifying READ modifying NOTIFY changed)
    Q_PROPERTY(bool candidatesLoading READ candidatesLoading NOTIFY candidatesChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selectedIds READ selectedIds NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectionProperties READ selectionProperties NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(QVariantList projectVoices READ projectVoices NOTIFY candidatesChanged)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(int candidateOffset READ candidateOffset NOTIFY candidatesChanged)
    Q_PROPERTY(QVariantMap qwenStatus READ qwenStatus NOTIFY changed)
    Q_PROPERTY(QVariantMap flashPreview READ flashPreview NOTIFY changed)
    Q_PROPERTY(QVariantMap flashConnection READ flashConnection NOTIFY changed)
    Q_PROPERTY(QString defaultVoiceVersionId READ defaultVoiceVersionId NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *filteredModel READ filteredModel CONSTANT)
    Q_PROPERTY(QVariantMap generationPreview READ generationPreview NOTIFY generationPreviewChanged)
public:
    enum Role { Id = Qt::UserRole + 1, ParagraphId, Body, Spoken, Manual, Notes, VoiceVersion, Gain, Pause, SelectedTake, Stale, ParagraphNumber, VoiceName, Locked, GenerationState };
    VoiceStudioController(ApplicationContext *context, ProjectSession *session, QObject *parent = nullptr);
    ~VoiceStudioController() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool modified() const;
    bool busy() const { return busy_ || candidatesLoading_; }
    bool modifying() const { return busy_; }
    bool candidatesLoading() const { return candidatesLoading_; }
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }
    QString statusText() const { return status_; }
    QString selectedId() const { return selectedId_; }
    QVariantMap sessionState() const { return document_.contains(QStringLiteral("session")) ? document_.value(QStringLiteral("session")).toObject().toVariantMap() : viewState_.toVariantMap(); }
    Q_INVOKABLE void persistSessionState(const QVariantMap &state);
    Q_INVOKABLE void setSessionState(const QVariantMap &state);
    QVariantMap selected() const;
    QStringList selectedIds() const { return selectedIds_; }
    QVariantMap selectionProperties() const;
    Q_INVOKABLE void selectMany(const QString &id, bool toggle = false, bool range = false);
    Q_INVOKABLE void setLocked(bool locked);
    Q_INVOKABLE void editSelection(const QVariantMap &fields);
    QVariantList preview() const { return preview_; }
    QVariantList projectVoices() const { return projectVoices_; }
    QVariantList candidates() const { return candidates_; }
    int candidateOffset() const { return candidateOffset_; }
    QVariantMap qwenStatus() const { return qwenStatus_.toVariantMap(); }
    QVariantMap flashPreview() const { return flashPreview_.toVariantMap(); }
    QVariantMap flashConnection() const { return flashConnection_.toVariantMap(); }
    QString defaultVoiceVersionId() const { return document_.value(QStringLiteral("defaultVoiceVersionId")).toString(); }
    QAbstractItemModel *filteredModel() const { return filter_; }
    QVariantMap generationPreview() const { return generationPreview_; }
    Q_INVOKABLE void confirmGeneration();
    Q_INVOKABLE void discardGeneration();
    Q_INVOKABLE void setManuscriptFilter(const QString &text, const QString &state, int paragraph = 0);
    Q_INVOKABLE int filteredIndex(const QString &id) const;
    Q_INVOKABLE QString adjacentFilteredId(int direction, bool pending = false) const;
    void setTasks(TtsTaskController *tasks);
    QJsonObject snapshot() const { return document_; }
    void reset(const QJsonObject &document);
    void restoreRecoveredDocument(const QJsonObject &document) { commit(document, QStringLiteral("已恢复配音草稿；中断任务不会自动重新发送。")); }
    void markSaved(const QJsonObject &document);
    Q_INVOKABLE void select(const QString &id);
    Q_INVOKABLE void editSegment(const QString &id, const QString &text, const QString &spoken, bool manual, const QString &notes);
    Q_INVOKABLE bool commitDraft(const QVariantMap &draft);
    Q_INVOKABLE void previewText(const QString &text, const QString &engine = {});
    Q_INVOKABLE void previewSplit(int offset);
    Q_INVOKABLE void previewMerge();
    Q_INVOKABLE void previewResegment(const QString &text);
    Q_INVOKABLE QString paragraphText() const;
    Q_INVOKABLE int segmentIndex(const QString &id) const;
    Q_INVOKABLE void previewDelete();
    Q_INVOKABLE void previewJson(const QUrl &file, bool targeted);
    Q_INVOKABLE void exportJson(const QUrl &file);
    Q_INVOKABLE void applyPreview();
    Q_INVOKABLE void discardPreview();
    Q_INVOKABLE void setArrangement(double gain, int pauseMs, const QString &startSample, const QString &endSample);
    Q_INVOKABLE void bindVoiceVersion(const QString &versionId, const QString &scope = QStringLiteral("selected"), const QString &displayName = {});
    Q_INVOKABLE void restoreDefaultVoice();
    Q_INVOKABLE int inheritedVoiceCount() const;
    Q_INVOKABLE QVariantList replacementRules() const;
    Q_INVOKABLE void previewReplacements(const QVariantList &rules);
    Q_INVOKABLE void previewNormalization();
    Q_INVOKABLE void previewSubtitleImport();
    Q_INVOKABLE void previewRoughCutImport();
    Q_INVOKABLE void previewSubtitleSync(bool text, bool timing);
    Q_INVOKABLE QVariantList subtitleChoices() const;
    Q_INVOKABLE void previewSubtitleRelink(const QString &subtitleId);
    Q_INVOKABLE void moveSelected(int direction, bool paragraph = false);
    Q_INVOKABLE void applySafeCandidates(bool replaceApplied = false);
    Q_INVOKABLE void loadCandidates(int offset = 0, const QString &segmentId = {});
    Q_INVOKABLE QVariantList archivedSegments(int offset = 0) const;
    Q_INVOKABLE void applyCandidate(const QString &takeId, const QString &cropPolicy = {});
    Q_INVOKABLE bool candidateIsCurrent(const QString &takeId) const;
    Q_INVOKABLE void auditionSelectedTake();
    Q_INVOKABLE QVariantMap exportSummary() const;
    Q_INVOKABLE QVariantMap engineCapabilities(const QString &engine) const;
    Q_INVOKABLE QVariantMap generationReadiness(const QString &engine, const QString &python, const QString &directory) const;
    Q_INVOKABLE void clearUnsupportedParameters(const QString &engine);
    Q_INVOKABLE void generateMock(const QString &python, const QString &scope, bool review = false);
    Q_INVOKABLE void checkQwen(const QString &python, const QString &directory) { checkLocal(QStringLiteral("qwen"), python, directory); }
    Q_INVOKABLE void generateQwen(const QString &python, const QString &directory, const QString &scope) { generateLocal(QStringLiteral("qwen"), python, directory, scope); }
    Q_INVOKABLE void checkLocal(const QString &engine, const QString &python, const QString &directory);
    Q_INVOKABLE void generateLocal(const QString &engine, const QString &python, const QString &directory, const QString &scope, bool review = false);
    Q_INVOKABLE void prepareFlash(const QString &scope, double budget, bool regenerate = false);
    Q_INVOKABLE void confirmFlash();
    Q_INVOKABLE void discardFlash() { flashPreview_ = {}; flashJobs_.clear(); emit changed(); }
    Q_INVOKABLE bool saveFlashCredential(const QString &reference, const QString &key);
    Q_INVOKABLE bool flashCredentialSaved(const QString &reference) const;
    Q_INVOKABLE void testFlashConnection(const QString &workspace, const QString &reference, const QString &draftKey = {}, bool ignoreSaved = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void cancel() { cancel_ = true; }
    void cancelAndWait();
signals:
    void documentReset();
    void changed();
    void selectionChanged();
    void previewChanged();
    void candidatesChanged();
    void auditionRequested(const QString &path, const QString &sha256);
    void generationPreviewChanged();
private:
    QSortFilterProxyModel *filter_ = nullptr;
    void propose(const QJsonObject &document, const QVariantList &differences, const QString &message);
    bool commit(const QJsonObject &document, const QString &description);
    QString workerPath() const;
    void replace(const QJsonObject &document);
    ApplicationContext *context_;
    ProjectSession *session_;
    QJsonObject document_;
    QJsonObject saved_;
    QJsonObject viewState_;
    QJsonObject pending_;
    QJsonObject previewBase_;
    QVariantList preview_;
    QVariantList candidates_;
    QVariantList projectVoices_;
    QJsonObject generationStates_;
    QPointer<TtsTaskController> tasks_;
    QString selectedId_;
    QStringList selectedIds_;
    QString status_;
    QUndoStack undo_;
    QFuture<void> task_;
    QFuture<void> candidateTask_;
    std::shared_ptr<std::atomic<bool>> candidateCancel_;
    quint64 candidateEpoch_ = 0;
    bool candidatesLoading_ = false;
    std::atomic<bool> cancel_{false};
    quint64 epoch_ = 0;
    bool busy_ = false;
    int candidateOffset_ = 0;
    QJsonObject qwenStatus_;
    QString qwenPython_, qwenDirectory_;
    QJsonObject flashPreview_, flashDocument_;
    QJsonObject flashConnection_;
    QVector<TtsJob> flashJobs_;
    QVariantMap generationPreview_;
    std::function<void()> pendingGeneration_;
};
}
