#include "project_session.h"
#include "application_context.h"
#include "app_controller.h"
#include "rough_cut_controller.h"
#include "voice_studio_controller.h"
#include "tts_task_controller.h"
#include "voice_library_controller.h"
#include "voice_audio_controller.h"
#include "tts/voice_document.h"
#include "common/file_path_guard.h"
#include "media/media_probe.h"
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QLockFile>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

namespace subcue {
namespace {
void setProjectIdentity(Project &subtitle, RoughCutProject &roughCut, const QString &id)
{
    auto subtitleIdentity = subtitle.metadata.value(QStringLiteral("projectIdentity")).toObject();
    subtitleIdentity.insert(QStringLiteral("id"), id);
    subtitle.metadata.insert(QStringLiteral("projectIdentity"), subtitleIdentity);
    auto roughIdentity = roughCut.state.value(QStringLiteral("projectIdentity")).toObject();
    roughIdentity.insert(QStringLiteral("id"), id);
    roughCut.state.insert(QStringLiteral("projectIdentity"), roughIdentity);
}

bool prepareModules(const QString &path, const ProjectStorageAdapter::Snapshot &snapshot,
                    Project &subtitle, RoughCutProject &roughCut, QString *error)
{
    if (const auto nativeSubtitle = std::get_if<Project>(&snapshot.data)) subtitle = *nativeSubtitle;
    else if (const auto nativeRoughCut = std::get_if<RoughCutProject>(&snapshot.data)) roughCut = *nativeRoughCut;
    else {
        const auto &data = std::get<ProjectStore::Snapshot>(snapshot.data);
        if (data.modules.contains(QStringLiteral("voiceStudio")) && (!data.modules.value(QStringLiteral("voiceStudio")).isObject()
            || !VoiceDocument::validate(data.modules.value(QStringLiteral("voiceStudio")).toObject(), error))) return false;
        ProjectStorageAdapter adapter(path, ProjectStorageAdapter::Format::UnifiedSqlite);
        if (data.modules.contains(QStringLiteral("subtitle"))) {
            const auto module = adapter.loadModule(QStringLiteral("subtitle"), error);
            if (!module || !std::holds_alternative<Project>(*module)) return false;
            subtitle = std::get<Project>(*module);
        }
        if (data.modules.contains(QStringLiteral("roughcut"))) {
            const auto module = adapter.loadModule(QStringLiteral("roughcut"), error);
            if (!module || !std::holds_alternative<RoughCutProject>(*module)) return false;
            roughCut = std::get<RoughCutProject>(*module);
        }
    }
    setProjectIdentity(subtitle, roughCut, snapshot.projectId);
    return true;
}

bool subtitleAvailable(Project &project, const std::atomic<bool> *cancel, QString *error)
{
    if (project.mediaPath.isEmpty() || !QFileInfo::exists(project.mediaPath)) return false;
    const auto hash = RoughCutProjectSerializer::mediaSha256(project.mediaPath, error, cancel);
    const QString expected = project.mediaFingerprint.value(QStringLiteral("sha256")).toString();
    if (hash.size() != 32 || (!expected.isEmpty() && QString::fromLatin1(hash.toHex()) != expected)) return false;
    project.mediaFingerprint.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.toHex()));
    project.mediaFingerprint.insert(QStringLiteral("size"), QFileInfo(project.mediaPath).size());
    project.mediaFingerprint.insert(QStringLiteral("mtime"), QFileInfo(project.mediaPath).lastModified().toMSecsSinceEpoch());
    return true;
}
void releaseRecoveryWorkspace(const QString &path, const QString &root)
{
    if (path.isEmpty()) return;
    const QFileInfo directory(QFileInfo(path).absolutePath());
    const auto allowed = QDir(QDir(root).filePath(QStringLiteral("workspaces"))).canonicalPath();
    if (allowed.isEmpty() || directory.isSymLink() || directory.isJunction()
        || !directory.fileName().startsWith(QStringLiteral("SubCue-draft-"))
        || !directory.canonicalFilePath().startsWith(allowed + QLatin1Char('/'), Qt::CaseInsensitive)) return;
    QLockFile lock(path + QStringLiteral(".session.lock")); lock.setStaleLockTime(0);
    if (lock.tryLock(0)) { lock.unlock(); QDir(directory.absoluteFilePath()).removeRecursively(); }
}
}

ProjectSession::ProjectSession(ApplicationContext *context, AppController *editor, RoughCutController *roughCut, QObject *parent)
    : QObject(parent), context_(context), editor_(editor), roughCut_(roughCut)
{
    Q_ASSERT(context_ && editor_ && roughCut_ && !context_->projectSession);
    context_->projectSession = this;
    connect(editor_, &AppController::modifiedChanged, this, &ProjectSession::modifiedChanged);
    connect(roughCut_, &RoughCutController::modifiedChanged, this, &ProjectSession::modifiedChanged);
    recoveryIdleTimer_.setSingleShot(true); recoveryIdleTimer_.setInterval(2000);
    recoveryDeadlineTimer_.setSingleShot(true); recoveryDeadlineTimer_.setInterval(10000);
    connect(this, &ProjectSession::modifiedChanged, this, &ProjectSession::scheduleRecovery);
    connect(&recoveryIdleTimer_, &QTimer::timeout, this, &ProjectSession::persistRecovery);
    connect(&recoveryDeadlineTimer_, &QTimer::timeout, this, &ProjectSession::persistRecovery);
    saveWaitTimer_.setInterval(100);
    connect(&saveWaitTimer_, &QTimer::timeout, this, [this] {
        if (!savePending() || shuttingDown_) { saveWaitTimer_.stop(); return; }
        if (busy_ || editor_->busy() || roughCut_->busy() || (voice_ && voice_->busy())
            || (tts_ && tts_->busy()) || (library_ && library_->busy()) || (audio_ && audio_->busy())) return;
        const auto destination = pendingSave_;
        const auto module = pendingSaveModule_;
        cancelPendingSave();
        saveProject(destination, module);
    });
}

QString ProjectSession::recoveryRoot() const
{
    return QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("recovery"));
}

void ProjectSession::scheduleRecovery()
{
    if (shuttingDown_ || !unified() || (!modified() && recoveryDrafts_.isEmpty())) return;
    recoveryIdleTimer_.start();
    if (!recoveryDeadlineTimer_.isActive()) recoveryDeadlineTimer_.start();
}

void ProjectSession::setRecoveryDrafts(const QVariantMap &drafts)
{
    const auto next = QJsonObject::fromVariantMap(drafts);
    if (next == recoveryDrafts_) return;
    recoveryDrafts_ = next; scheduleRecovery();
}

void ProjectSession::persistRecovery()
{
    if (!unified() || shuttingDown_ || (!modified() && recoveryDrafts_.isEmpty())) return;
    if (busy_ || recoveryTask_.isRunning()) { recoveryIdleTimer_.start(); return; }
    recoveryIdleTimer_.stop(); recoveryDeadlineTimer_.stop();
    const auto subtitle = editor_->projectSnapshot(); const auto rough = roughCut_->projectSnapshot();
    const auto voice = voice_ ? voice_->snapshot() : QJsonObject{};
    QJsonObject record{{QStringLiteral("version"), 1}, {QStringLiteral("projectId"), projectId()},
        {QStringLiteral("storagePath"), storagePath()}, {QStringLiteral("namedPath"), path_},
        {QStringLiteral("storageVersion"), QString::fromLatin1(checkpoint_.storageVersion.toHex())},
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("voice"), voice}, {QStringLiteral("drafts"), recoveryDrafts_}};
    if (draftStorage_) draftStorage_->setAutoRemove(false);
    const auto root = recoveryRoot(); const auto storage = draftStorage_; const QPointer<ProjectSession> self(this);
    recoveryTask_ = context_->submitProjectStorage([self, storage, root, subtitle, rough, record = std::move(record)]() mutable {
        QString error; QTemporaryDir staging;
        const auto subtitleFile = staging.filePath(QStringLiteral("subtitle.json"));
        const auto roughFile = staging.filePath(QStringLiteral("rough.sqlite"));
        bool success = staging.isValid() && ProjectSerializer::save(subtitleFile, subtitle, &error)
            && RoughCutProjectSerializer::save(roughFile, rough, &error);
        for (const auto &entry : {qMakePair(QStringLiteral("subtitle"), subtitleFile), qMakePair(QStringLiteral("roughcut"), roughFile)}) {
            QFile file(entry.second);
            if (!success || !file.open(QIODevice::ReadOnly) || file.size() > 64LL * 1024 * 1024) { success = false; break; }
            record.insert(entry.first, QString::fromLatin1(file.readAll().toBase64()));
        }
        if (success) {
            success = QDir().mkpath(root);
            QSaveFile file(QDir(root).filePath(record.value(QStringLiteral("projectId")).toString() + QStringLiteral(".json")));
            const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
            success = success && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        }
        if (!success && self) QMetaObject::invokeMethod(self.data(), [self, error] {
            if (!self || self->shuttingDown_) return;
            self->status_ = QStringLiteral("恢复草稿暂存失败：%1").arg(error.isEmpty() ? QStringLiteral("请检查存储空间和权限") : error); emit self->statusChanged();
        }, Qt::QueuedConnection);
    });
}

QVariantList ProjectSession::recoverableProjects() const
{
    QVariantList projects;
    const QDir root(recoveryRoot());
    for (const auto &info : root.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Time)) {
        if (info.isSymLink() || info.size() > 180LL * 1024 * 1024) continue;
        QFile file(info.absoluteFilePath()); if (!file.open(QIODevice::ReadOnly)) continue;
        const auto record = QJsonDocument::fromJson(file.readAll()).object();
        if (record.value(QStringLiteral("version")).toInt() != 1 || QUuid(record.value(QStringLiteral("projectId")).toString()).isNull()
            || !QFileInfo::exists(record.value(QStringLiteral("storagePath")).toString())) continue;
        projects.append(QVariantMap{{QStringLiteral("id"), record.value(QStringLiteral("projectId")).toString()},
            {QStringLiteral("path"), record.value(QStringLiteral("namedPath")).toString()}, {QStringLiteral("time"), record.value(QStringLiteral("time")).toString()}});
    }
    return projects;
}

bool ProjectSession::discardRecovery(const QString &id)
{
    if (QUuid(id).isNull() || (active_ && id == projectId())) return false;
    QFile file(QDir(recoveryRoot()).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 180LL * 1024 * 1024) return false;
    const auto record = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    if (record.value(QStringLiteral("version")).toInt() != 1 || record.value(QStringLiteral("projectId")).toString() != id
        || !QFileInfo::exists(record.value(QStringLiteral("storagePath")).toString())) return false;
    QLockFile lock(record.value(QStringLiteral("storagePath")).toString() + QStringLiteral(".session.lock"));
    lock.setStaleLockTime(0); if (!lock.tryLock(0)) return false; lock.unlock();
    if (record.value(QStringLiteral("namedPath")).toString().isEmpty()) releaseRecoveryWorkspace(record.value(QStringLiteral("storagePath")).toString(), recoveryRoot());
    return file.remove();
}

bool ProjectSession::recoverProject(const QString &id)
{
    if (!ready() || QUuid(id).isNull()) return false;
    QFile file(QDir(recoveryRoot()).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 180LL * 1024 * 1024) return false;
    recoveryOverlay_ = QJsonDocument::fromJson(file.readAll()).object();
    if (recoveryOverlay_.value(QStringLiteral("projectId")).toString() != id || recoveryOverlay_.value(QStringLiteral("version")).toInt() != 1) { recoveryOverlay_ = {}; return false; }
    if (openPrepared(recoveryOverlay_.value(QStringLiteral("storagePath")).toString(), {})) return true;
    recoveryOverlay_ = {}; return false;
}

void ProjectSession::setVoiceServices(TtsTaskController *tasks, VoiceLibraryController *library, VoiceAudioController *audio)
{
    tts_ = tasks; library_ = library; audio_ = audio;
}
bool ProjectSession::storageTasksRunning() const
{
    return busy_ || savePending() || recoveryTask_.isRunning() || (editor_ && editor_->busy()) || (roughCut_ && roughCut_->busy())
        || (voice_ && voice_->busy()) || (tts_ && tts_->busy()) || (library_ && library_->busy()) || (audio_ && audio_->busy());
}

void ProjectSession::cancelPendingSave()
{
    saveWaitTimer_.stop(); pendingSave_.clear(); pendingSaveModule_.clear(); emit busyChanged();
}

ProjectSession::~ProjectSession()
{
    shutdown();
    if (context_ && context_->projectSession == this) context_->projectSession = nullptr;
}

bool ProjectSession::modified() const
{
    return active_ && ((editor_ && editor_->modified()) || (roughCut_ && roughCut_->modified()) || (voice_ && voice_->modified()));
}
QJsonObject ProjectSession::voiceSnapshot() const
{
    return unified() ? std::get<ProjectStore::Snapshot>(checkpoint_.data).modules.value(QStringLiteral("voiceStudio")).toObject() : QJsonObject{};
}
QJsonArray ProjectSession::subtitleReferences() const
{
    QJsonArray result;
    if (!editor_) return result;
    for (const auto &track : editor_->projectSnapshot().tracks) for (const auto &cue : track.subtitles) {
        QJsonObject reference{{QStringLiteral("originProjectId"), projectId()}, {QStringLiteral("id"), cue.id}, {QStringLiteral("text"), cue.text},
            {QStringLiteral("startUs"), QString::number(cue.start.microseconds())}, {QStringLiteral("endUs"), QString::number(cue.end.microseconds())}, {QStringLiteral("trackId"), track.id}};
        reference.insert(QStringLiteral("sourceVersion"), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(reference).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())); result.append(reference);
    }
    return result;
}
QStringList ProjectSession::inputPaths() const
{
    QStringList paths{path_, storagePath()};
    if (editor_) paths.append(editor_->projectSnapshot().mediaPath);
    if (roughCut_) { paths.append(roughCut_->projectSnapshot().mediaPath); paths.append(roughCut_->projectSnapshot().scriptPath); }
    return paths;
}
QJsonArray ProjectSession::roughCutReferences() const
{
    QJsonArray result; if (!roughCut_) return result; const auto project = roughCut_->projectSnapshot(); if (project.sampleRate <= 0 || project.mediaSha256.size() != 32) return result;
    for (const auto &value : project.state.value(QStringLiteral("timeline")).toArray()) {
        const auto clip = value.toObject(); if (clip.value(QStringLiteral("end")).toInteger() <= clip.value(QStringLiteral("start")).toInteger()) continue;
        QJsonObject source{{QStringLiteral("originProjectId"), projectId()}, {QStringLiteral("mediaSha256"), QString::fromLatin1(project.mediaSha256.toHex())}, {QStringLiteral("analysisVersion"), project.analysisVersion},
            {QStringLiteral("sampleRate"), project.sampleRate}, {QStringLiteral("sourceStartSample"), QString::number(clip.value(QStringLiteral("start")).toInteger())}, {QStringLiteral("sourceEndSample"), QString::number(clip.value(QStringLiteral("end")).toInteger())},
            {QStringLiteral("sequenceStartSample"), QString::number(clip.value(QStringLiteral("timelineStart")).toInteger())}, {QStringLiteral("text"), clip.value(QStringLiteral("text"))}};
        if (const auto unified = std::get_if<ProjectStore::Snapshot>(&checkpoint_.data)) source.insert(QStringLiteral("mediaAssetId"), unified->modules.value(QStringLiteral("roughcut")).toObject().value(QStringLiteral("mediaAssetId")));
        // 映射依赖分析版本和原始坐标，不把粗剪行号／片段下标作为永久关联。
        auto identity = source; identity.remove(QStringLiteral("sequenceStartSample"));
        source.insert(QStringLiteral("id"), QUuid::createUuidV5(QUuid(projectId()), QJsonDocument(identity).toJson(QJsonDocument::Compact)).toString(QUuid::WithoutBraces)); result.append(source);
    }
    return result;
}
void ProjectSession::setVoiceController(VoiceStudioController *controller)
{
    if (voice_) disconnect(voice_, nullptr, this, nullptr);
    voice_ = controller;
    if (voice_) { voice_->reset(voiceSnapshot()); connect(voice_, &VoiceStudioController::changed, this, &ProjectSession::modifiedChanged); }
}

bool ProjectSession::ready() const
{
    return !busy_ && !savePending() && !shuttingDown_ && editor_ && roughCut_ && !editor_->busy() && !roughCut_->busy();
}

bool ProjectSession::isUnifiedUrl(const QUrl &url)
{
    return QFileInfo(url.toLocalFile()).suffix().compare(QStringLiteral("subcue-project"), Qt::CaseInsensitive) == 0;
}

void ProjectSession::setBusy(bool busy, const QString &status)
{
    busy_ = busy;
    if (!status.isEmpty()) { status_ = status; emit statusChanged(); }
    emit busyChanged();
}

void ProjectSession::newProject()
{
    if (!ready()) return;
    emit aboutToChange();
    recoveryTask_.waitForFinished();
    if (!projectId().isEmpty()) QFile::remove(QDir(recoveryRoot()).filePath(projectId() + QStringLiteral(".json")));
    recoveryIdleTimer_.stop(); recoveryDeadlineTimer_.stop(); recoveryDrafts_ = {};
    const auto recoveredStorage = std::exchange(recoveredStoragePath_, {}); const auto recoveryDirectory = recoveryRoot();
    draftLock_.reset();
    const QPointer<ProjectSession> self(this);
    const auto previousStorage = std::move(draftStorage_);
    if (previousStorage) previousStorage->setAutoRemove(true);
    QDir().mkpath(QDir(recoveryRoot()).filePath(QStringLiteral("workspaces")));
    draftStorage_ = std::make_shared<QTemporaryDir>(QDir(recoveryRoot()).filePath(QStringLiteral("workspaces/SubCue-draft-XXXXXX")));
    draftLock_ = std::make_unique<QLockFile>(draftStorage_->filePath(QStringLiteral("draft.subcue-project.session.lock")));
    draftLock_->setStaleLockTime(0); (void)draftLock_->tryLock(0);
    ++checkpoint_.sessionGeneration;
    checkpoint_.storageVersion.clear();
    checkpoint_.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    checkpoint_.data = ProjectStore::Snapshot{checkpoint_.projectId, 0, {}};
    path_.clear(); active_ = true;
    // 临时存储由会话持有；串行队列保证音色复制和生成排在建库之后。
    task_ = context_->submitProjectStorage([self, previousStorage, storage = draftStorage_, id = checkpoint_.projectId, recoveredStorage, recoveryDirectory] {
        if (!self) return;
        self->manager_.close();
        releaseRecoveryWorkspace(recoveredStorage, recoveryDirectory);
        QString error;
        if (!storage->isValid() || !ProjectStore(storage->filePath(QStringLiteral("draft.subcue-project"))).create(id, &error)) {
            QMetaObject::invokeMethod(self.data(), [self, storage, error] {
                if (!self || self->shuttingDown_ || self->draftStorage_ != storage || !self->path_.isEmpty()) return;
                self->status_ = error.isEmpty() ? QStringLiteral("无法创建临时工作区，请检查临时目录权限和磁盘空间。") : error;
                emit self->statusChanged();
            }, Qt::QueuedConnection);
        }
    });
    Project subtitle;
    RoughCutProject roughCut;
    subtitle.metadata.insert(QStringLiteral("projectIdentity"), QJsonObject{{QStringLiteral("id"), checkpoint_.projectId}});
    roughCut.state.insert(QStringLiteral("projectIdentity"), QJsonObject{{QStringLiteral("id"), checkpoint_.projectId}});
    editor_->applyProject(subtitle, {}, false);
    roughCut_->applyProject(roughCut, {}, {}, false);
    if (voice_) voice_->reset(VoiceDocument::empty());
    status_ = QStringLiteral("未命名工程 · 可直接编辑、配音和导出，保存工程以便下次继续。");
    emit changed(); emit modifiedChanged(); emit statusChanged();
}

bool ProjectSession::openProject(const QUrl &url, const QString &requestingModule)
{
    recoveryOverlay_ = {};
    return openPrepared(url.toLocalFile(), requestingModule);
}

bool ProjectSession::convertProject(const QString &sourcePath, const QString &module, const QUrl &destination)
{
    if (module != QStringLiteral("subtitle") && module != QStringLiteral("roughcut")) return false;
    return openPrepared(destination.toLocalFile(), {}, module, sourcePath);
}

bool ProjectSession::restoreProject(const QUrl &source, const QUrl &destination)
{
    return openPrepared(destination.toLocalFile(), {}, {}, source.toLocalFile(), true);
}

bool ProjectSession::openPrepared(const QString &path, const QString &requestingModule,
    const QString &conversionModule, const QString &source, bool restoring)
{
    if (!ready() || path.isEmpty()) return false;
    emit aboutToChange();
    cancel_ = false;
    changingProject_ = true;
    editor_->setBusy(true); roughCut_->setBusy(true);
    setBusy(true, QStringLiteral("正在准备统一工程…"));
    const QPointer<ProjectSession> self(this);
    auto *cancel = &cancel_;
    const auto recovery = std::exchange(recoveryOverlay_, {});
    const auto format = isUnifiedUrl(QUrl::fromLocalFile(path)) || !conversionModule.isEmpty() || restoring
        ? ProjectStorageAdapter::Format::UnifiedSqlite
        : (requestingModule == QStringLiteral("roughcut") || QFileInfo(path).suffix() == QStringLiteral("subcue-roughcut")
            ? ProjectStorageAdapter::Format::RoughCutSqlite : ProjectStorageAdapter::Format::SubtitleJson);
    task_ = context_->submitProjectStorage([self, cancel, path, requestingModule, conversionModule, source, restoring, format, recovery] {
        if (!self) return;
        QString error;
        std::optional<ProjectStorageAdapter::Snapshot> snapshot;
        // 转换／恢复先使用独立管理器验证，成功后再让当前管理器取得会话。
        if (!conversionModule.isEmpty() || restoring) {
            ProjectManager preparation;
            snapshot = restoring ? preparation.restore(source, path, &error, ProjectStore::BackupMode::ExternalMediaReferences, cancel)
                : preparation.convert(source, conversionModule == QStringLiteral("subtitle")
                    ? ProjectStorageAdapter::Format::SubtitleJson : ProjectStorageAdapter::Format::RoughCutSqlite, path, &error, cancel);
            preparation.close();
        } else if (!cancel->load()) {
            snapshot = ProjectStorageAdapter(path, format).load(&error);
        }
        Project subtitle;
        RoughCutProject roughCut;
        MediaInfo roughInfo;
        bool subtitleReady = false, roughReady = false;
        int recoveredTasks = 0;
        if (snapshot && !prepareModules(path, *snapshot, subtitle, roughCut, &error)) snapshot.reset();
        if (snapshot && !recovery.isEmpty()) {
            if (snapshot->projectId != recovery.value(QStringLiteral("projectId")).toString()
                || (!recovery.value(QStringLiteral("namedPath")).toString().isEmpty()
                    && snapshot->storageVersion != QByteArray::fromHex(recovery.value(QStringLiteral("storageVersion")).toString().toLatin1()))) {
                snapshot.reset(); error = QStringLiteral("原工程已被外部修改或身份不匹配，未覆盖原工程；请打开原工程检查。");
            } else {
                QTemporaryDir staging;
                for (const auto &key : {QStringLiteral("subtitle"), QStringLiteral("roughcut")}) {
                    const auto decoded = QByteArray::fromBase64Encoding(recovery.value(key).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                    QFile file(staging.filePath(key));
                    if (!decoded || !staging.isValid() || !file.open(QIODevice::WriteOnly)
                        || file.write(decoded.decoded) != decoded.decoded.size()) { snapshot.reset(); break; }
                    file.close();
                    if (key == QStringLiteral("subtitle")) {
                        const auto restored = ProjectSerializer::load(file.fileName(), &error);
                        if (restored) subtitle = *restored; else snapshot.reset();
                    } else {
                        const auto restored = RoughCutProjectSerializer::load(file.fileName(), &error);
                        if (restored) roughCut = *restored; else snapshot.reset();
                    }
                }
                if (snapshot && !VoiceDocument::validate(recovery.value(QStringLiteral("voice")).toObject(), &error)) snapshot.reset();
                if (!snapshot && error.isEmpty()) error = QStringLiteral("恢复记录不完整或损坏，原工程未修改。");
            }
        }
        if (snapshot && !cancel->load()) {
            subtitleReady = subtitleAvailable(subtitle, cancel, &error);
            roughInfo = mediaInfoFromJson(roughCut.state.value(QStringLiteral("mediaInfo")).toObject());
            // 统一工程的模块快照只保存媒体资产引用；早期快照可能没有指纹，按实际文件回填，
            // 否则重开后素材被判离线，导出被禁用且“重新定位素材”也会因缺少指纹被拒绝。
            const QByteArray mediaHash = roughCut.mediaPath.isEmpty() || !QFileInfo::exists(roughCut.mediaPath)
                ? QByteArray() : RoughCutProjectSerializer::mediaSha256(roughCut.mediaPath, &error, cancel);
            if (mediaHash.size() == 32 && (roughCut.mediaSha256.isEmpty() || mediaHash == roughCut.mediaSha256)) {
                roughCut.mediaSha256 = mediaHash;
                const auto probed = MediaProbe::probe(roughCut.mediaPath);
                if (const auto info = std::get_if<MediaInfo>(&probed)) {
                    const bool verified = roughInfo.cfrVerified;
                    roughInfo = *info; roughInfo.cfrVerified = verified;
                    roughReady = roughInfo.audioStreamIndex >= 0;
                }
            }
            if (!cancel->load()) snapshot = self->manager_.open(path, format, &error, snapshot->storageVersion, true, &recoveredTasks);
            else { snapshot.reset(); error = QStringLiteral("工程打开已取消。"); }
        } else if (cancel->load()) { snapshot.reset(); error = QStringLiteral("工程打开已取消。"); }
        // 旧工程未存 UUID 时，准备读取与正式接管会各自产生临时身份，统一使用已接管会话的身份。
        if (snapshot) setProjectIdentity(subtitle, roughCut, snapshot->projectId);
        QMetaObject::invokeMethod(self.data(), [self, path, requestingModule, snapshot = std::move(snapshot),
            subtitle = std::move(subtitle), roughCut = std::move(roughCut), roughInfo, subtitleReady, roughReady, error, recoveredTasks, recovery] {
            if (!self || self->shuttingDown_) return;
            const bool success = snapshot.has_value();
            if (success) {
                self->recoveryTask_.waitForFinished();
                self->recoveryIdleTimer_.stop(); self->recoveryDeadlineTimer_.stop();
                if (recovery.isEmpty() || self->projectId() != snapshot->projectId)
                    QFile::remove(QDir(self->recoveryRoot()).filePath(self->projectId() + QStringLiteral(".json")));
                releaseRecoveryWorkspace(self->recoveredStoragePath_, self->recoveryRoot());
                self->checkpoint_ = *snapshot; self->path_ = QFileInfo(path).absoluteFilePath(); self->active_ = true;
                self->draftLock_.reset();
                if (self->draftStorage_) self->draftStorage_->setAutoRemove(true);
                self->draftStorage_.reset();
                self->editor_->applyProject(subtitle, self->path_, subtitleReady);
                self->roughCut_->applyProject(roughCut, self->path_, roughInfo, roughReady);
                if (self->voice_) self->voice_->reset(self->voiceSnapshot());
                self->recoveredStoragePath_.clear(); self->recoveryDrafts_ = {};
                if (!recovery.isEmpty()) {
                    if (recovery.value(QStringLiteral("namedPath")).toString().isEmpty()) {
                        self->recoveredStoragePath_ = self->path_; self->path_.clear();
                        self->editor_->projectPath_.clear(); self->roughCut_->projectPath_.clear();
                    }
                    self->editor_->setModified(true); self->roughCut_->modified_ = true;
                    if (self->voice_) self->voice_->restoreRecoveredDocument(recovery.value(QStringLiteral("voice")).toObject());
                    self->recoveryDrafts_ = recovery.value(QStringLiteral("drafts")).toObject();
                    emit self->recoveryRestored();
                }
                emit self->changed(); emit self->modifiedChanged();
            }
            self->editor_->setBusy(false); self->roughCut_->setBusy(false);
            self->changingProject_ = false;
            self->setBusy(false, success ? (self->unified() ? (recoveredTasks > 0
                ? QStringLiteral("统一工程已打开，%1 个中断任务等待手动处理；未重新发起生成。").arg(recoveredTasks)
                : QStringLiteral("统一工程已打开；离线媒体可稍后重新定位。"))
                : QStringLiteral("旧格式工程已打开；跨工作区数据请转换为统一副本。")) : error);
            emit self->opened(success, path, self->status_);
            if (requestingModule == QStringLiteral("subtitle")) emit self->editor_->projectOpenFinished(success, path, self->status_);
            else if (requestingModule == QStringLiteral("roughcut")) emit self->roughCut_->projectOpenFinished(success, path, self->status_);
        }, Qt::QueuedConnection);
    });
    return true;
}

bool ProjectSession::saveCurrentProject()
{
    return !path_.isEmpty() && saveProject(QUrl::fromLocalFile(path_));
}

bool ProjectSession::saveProject(const QUrl &url, const QString &requestingModule)
{
    const QString path = url.toLocalFile();
    if (!active_ || busy_ || shuttingDown_ || path.isEmpty() || savePending()) return false;
    if (editor_->busy() || roughCut_->busy() || (voice_ && voice_->busy()) || (tts_ && tts_->busy())
        || (library_ && library_->busy()) || (audio_ && audio_->busy())) {
        pendingSave_ = url; pendingSaveModule_ = requestingModule;
        status_ = QStringLiteral("等待当前任务完成后保存；可继续编辑或取消等待。");
        emit statusChanged(); emit busyChanged(); saveWaitTimer_.start(); return true;
    }
    if (!unified()) return saveLegacyProject(path, requestingModule);
    cancel_ = false;
    emit preparingSave();
    if (cancel_.load()) {
        status_ = QStringLiteral("请先修正未应用字段，再保存工程。"); emit statusChanged(); emit saved(false, path, status_); return false;
    }
    const Project subtitle = editor_->projectSnapshot();
    const RoughCutProject roughCut = roughCut_->projectSnapshot();
    const QJsonObject voice = voice_ ? voice_->snapshot() : QJsonObject{};
    QString error;
    QStringList inputs{subtitle.mediaPath, roughCut.mediaPath, roughCut.scriptPath};
    for (const auto &value : subtitle.state.value(QStringLiteral("assets")).toArray()) inputs.append(value.toObject().value(QStringLiteral("path")).toString());
    if (!safeOutputPath(path, inputs, &error)) { status_ = error; emit statusChanged(); emit saved(false, path, error); return false; }
    if (path_.isEmpty() || !sameFilePath(path_, path)) emit aboutToChange();
    const auto checkpoint = checkpoint_;
    const QString original = storagePath();
    cancel_ = false; setBusy(true, QStringLiteral("正在保存统一工程…"));
    const QPointer<ProjectSession> self(this);
    auto *cancel = &cancel_;
    task_ = context_->submitProjectStorage([self, cancel, path, original, checkpoint, subtitle, roughCut, voice, requestingModule] {
        if (!self) return;
        QString error;
        auto saved = checkpoint;
        bool success = !cancel->load();
        const bool replacing = original.isEmpty() || !sameFilePath(original, path);
        QTemporaryDir staging(QFileInfo(path).absoluteDir().filePath(QStringLiteral(".subcue-save-project-XXXXXX")));
        QString working = original;
        if (success && replacing) {
            success = staging.isValid() && !QFileInfo::exists(path) && !QFileInfo::exists(path + QStringLiteral(".assets"));
            if (!success) error = QStringLiteral("另存目标已存在或临时目录无法创建，未覆盖目标。");
            working = staging.filePath(QStringLiteral("project.sqlite"));
            if (success) success = original.isEmpty() ? ProjectStore(working).create(checkpoint.projectId, &error)
                : ProjectStore(original).backup(working, &error, ProjectStore::BackupMode::ExternalMediaReferences, cancel);
            if (success) {
                const auto copied = ProjectStorageAdapter(working, ProjectStorageAdapter::Format::UnifiedSqlite).load(&error);
                success = copied && std::get<ProjectStore::Snapshot>(copied->data).revision == std::get<ProjectStore::Snapshot>(checkpoint.data).revision;
                if (success) { saved = *copied; saved.sessionGeneration = checkpoint.sessionGeneration; }
                else if (error.isEmpty()) error = QStringLiteral("原工程被外部修改，另存未发布。");
            }
        }
        if (success) {
            ProjectStore store(working);
            const auto subtitleModule = ProjectStorageAdapter::captureModule(store, subtitle, saved.projectId, &error, cancel);
            const auto roughModule = subtitleModule ? ProjectStorageAdapter::captureModule(store, roughCut, saved.projectId, &error, cancel) : std::nullopt;
            success = subtitleModule && roughModule;
            if (success) {
                auto &data = std::get<ProjectStore::Snapshot>(saved.data);
                if (!voice.isEmpty()) data.modules.insert(QStringLiteral("voiceStudio"), voice);
                data.modules.insert(QStringLiteral("subtitle"), *subtitleModule);
                data.modules.insert(QStringLiteral("roughcut"), *roughModule);
                ProjectStorageAdapter::SaveResult result;
                success = replacing ? ProjectStorageAdapter(working, ProjectStorageAdapter::Format::UnifiedSqlite).save(saved, &error, cancel, &result)
                    : self->manager_.save(saved, &error, cancel, &result);
                if (success) { data.revision = *result.revision; saved.storageVersion = result.storageVersion; }
            }
        }
        if (success && replacing) {
            success = ProjectStore(working).backup(path, &error, ProjectStore::BackupMode::ExternalMediaReferences, cancel);
            if (success) {
                const auto opened = self->manager_.open(path, ProjectStorageAdapter::Format::UnifiedSqlite, &error, saved.storageVersion);
                success = opened.has_value();
                if (success) saved = *opened;
            }
        }
        if (!success && error.isEmpty()) error = QStringLiteral("工程保存已取消。");
        Project resolvedSubtitle = subtitle;
        RoughCutProject resolvedRoughCut = roughCut;
        // 读取已提交的资产位置，不用整个快照替换仍在编辑的模型。
        QString bindingError;
        const bool rebound = success && prepareModules(path, saved, resolvedSubtitle, resolvedRoughCut, &bindingError);
        QMetaObject::invokeMethod(self.data(), [self, path, requestingModule, success, error, saved = std::move(saved),
            subtitle, roughCut, resolvedSubtitle = std::move(resolvedSubtitle), resolvedRoughCut = std::move(resolvedRoughCut), rebound, bindingError] {
            if (!self || self->shuttingDown_) return;
            if (success) {
                self->checkpoint_ = saved; self->path_ = QFileInfo(path).absoluteFilePath();
                if (self->voice_) self->voice_->markSaved(std::get<ProjectStore::Snapshot>(saved.data).modules.value(QStringLiteral("voiceStudio")).toObject());
                self->draftLock_.reset();
                if (self->draftStorage_) self->draftStorage_->setAutoRemove(true);
                self->draftStorage_.reset();
                releaseRecoveryWorkspace(self->recoveredStoragePath_, self->recoveryRoot()); self->recoveredStoragePath_.clear();
                QFile::remove(QDir(self->recoveryRoot()).filePath(self->projectId() + QStringLiteral(".json")));
                setProjectIdentity(self->editor_->projectData_, self->roughCut_->projectData_, saved.projectId);
                self->editor_->projectPath_ = self->path_; self->roughCut_->projectPath_ = self->path_;
                if (rebound) {
                    if (self->editor_->mediaPath_ == subtitle.mediaPath) self->editor_->mediaPath_ = resolvedSubtitle.mediaPath;
                    const auto originalAssets = subtitle.state.value(QStringLiteral("assets")).toArray();
                    const auto resolvedAssets = resolvedSubtitle.state.value(QStringLiteral("assets")).toArray();
                    for (auto &value : self->editor_->projectFiles_) {
                        auto asset = value.toMap();
                        for (qsizetype index = 0; index < originalAssets.size() && index < resolvedAssets.size(); ++index) {
                            const auto originalAsset = originalAssets.at(index).toObject();
                            if (asset.value(QStringLiteral("path")).toString() == originalAsset.value(QStringLiteral("path")).toString()
                                && asset.value(QStringLiteral("id")).toString() == originalAsset.value(QStringLiteral("id")).toString()) {
                                asset.insert(QStringLiteral("path"), resolvedAssets.at(index).toObject().value(QStringLiteral("path")).toString());
                                value = asset;
                                break;
                            }
                        }
                    }
                    if (self->roughCut_->mediaPath_ == roughCut.mediaPath) self->roughCut_->mediaPath_ = resolvedRoughCut.mediaPath;
                    if (self->roughCut_->scriptPath_ == roughCut.scriptPath) self->roughCut_->scriptPath_ = resolvedRoughCut.scriptPath;
                    emit self->editor_->mediaChanged(); emit self->editor_->projectFilesChanged();
                    emit self->roughCut_->mediaChanged(); emit self->roughCut_->scriptChanged();
                }
                self->editor_->savedFingerprint_ = ProjectSerializer::fingerprint(resolvedSubtitle);
                self->editor_->setModified(self->editor_->projectFingerprint() != self->editor_->savedFingerprint_);
                self->roughCut_->savedFingerprint_ = RoughCutProjectSerializer::fingerprint(resolvedRoughCut); self->roughCut_->refreshModified();
                emit self->editor_->projectChanged(); emit self->roughCut_->projectChanged(); emit self->changed();
            }
            self->setBusy(false, success ? (rebound ? QStringLiteral("统一工程已保存。")
                : QStringLiteral("工程已提交，但资产位置刷新失败：%1。请重新打开确认。").arg(bindingError)) : error);
            if (success && self->voice_) self->voice_->loadCandidates();
            emit self->modifiedChanged(); emit self->saved(success, path, self->status_);
            if (requestingModule == QStringLiteral("subtitle")) emit self->editor_->projectSaveFinished(success, path, self->status_);
            else if (requestingModule == QStringLiteral("roughcut")) emit self->roughCut_->projectSaveFinished(success, path, self->status_);
        }, Qt::QueuedConnection);
    });
    return true;
}

bool ProjectSession::saveLegacyProject(const QString &path, const QString &requestingModule)
{
    const bool subtitle = legacyModule() == QStringLiteral("subtitle");
    if ((!requestingModule.isEmpty() && requestingModule != legacyModule())
        || (subtitle ? roughCut_->modified() : editor_->modified())) {
        status_ = QStringLiteral("旧格式只能保存原工作区数据，请先转换为统一工程处理跨区编辑。");
        emit statusChanged(); emit saved(false, path, status_);
        return false;
    }
    auto snapshot = checkpoint_;
    snapshot.data = subtitle ? ProjectStorageAdapter::Data(editor_->projectSnapshot())
        : ProjectStorageAdapter::Data(roughCut_->projectSnapshot());
    const QStringList inputs = subtitle ? QStringList{std::get<Project>(snapshot.data).mediaPath}
        : QStringList{std::get<RoughCutProject>(snapshot.data).mediaPath, std::get<RoughCutProject>(snapshot.data).scriptPath};
    QString error;
    if (!safeOutputPath(path, inputs, &error)) { status_ = error; emit statusChanged(); emit saved(false, path, error); return false; }
    const QString original = path_;
    cancel_ = false; setBusy(true, QStringLiteral("正在保存旧格式工程…"));
    const QPointer<ProjectSession> self(this);
    auto *cancel = &cancel_;
    task_ = context_->submitProjectStorage([self, cancel, snapshot, original, path, subtitle, requestingModule]() mutable {
        QString error;
        ProjectStorageAdapter::SaveResult result;
        bool success = !cancel->load();
        const auto format = subtitle ? ProjectStorageAdapter::Format::SubtitleJson : ProjectStorageAdapter::Format::RoughCutSqlite;
        if (success && sameFilePath(original, path)) success = self->manager_.save(snapshot, &error, cancel, &result);
        else if (success) {
            QLockFile targetLock(path + QStringLiteral(".lock"));
            QTemporaryDir staging(QFileInfo(path).absoluteDir().filePath(QStringLiteral(".subcue-save-legacy-XXXXXX")));
            const QString temporary = staging.filePath(QStringLiteral("project"));
            success = targetLock.tryLock(0) && staging.isValid() && !QFileInfo::exists(path) && QFile::copy(original, temporary);
            if (success) success = ProjectStorageAdapter(temporary, format).save(snapshot, &error, cancel, &result);
            if (success && RoughCutProjectSerializer::mediaSha256(original, &error, cancel) != snapshot.storageVersion) {
                success = false; error = QStringLiteral("原工程在另存期间被修改，目标未发布。");
            }
            if (success && !cancel->load()) success = QFile::rename(temporary, path);
            else success = false;
            if (success) {
                targetLock.unlock();
                const auto opened = self->manager_.open(path, format, &error, result.storageVersion);
                success = opened.has_value();
                if (success) snapshot = *opened;
            }
        }
        if (success) snapshot.storageVersion = result.storageVersion;
        if (!success && error.isEmpty()) error = cancel->load() ? QStringLiteral("工程保存已取消。")
            : QStringLiteral("无法发布另存目标，请检查目标是否存在、空间和写入权限。");
        QMetaObject::invokeMethod(self.data(), [self, path, subtitle, requestingModule, success, error, snapshot = std::move(snapshot)] {
            if (!self || self->shuttingDown_) return;
            if (success) {
                self->checkpoint_ = snapshot; self->path_ = QFileInfo(path).absoluteFilePath();
                setProjectIdentity(self->editor_->projectData_, self->roughCut_->projectData_, snapshot.projectId);
                if (subtitle) {
                    self->editor_->projectPath_ = self->path_;
                    self->editor_->savedFingerprint_ = ProjectSerializer::fingerprint(std::get<Project>(snapshot.data));
                    self->editor_->setModified(self->editor_->projectFingerprint() != self->editor_->savedFingerprint_);
                    emit self->editor_->projectChanged();
                } else {
                    self->roughCut_->projectPath_ = self->path_;
                    self->roughCut_->savedFingerprint_ = RoughCutProjectSerializer::fingerprint(std::get<RoughCutProject>(snapshot.data));
                    self->roughCut_->refreshModified(); emit self->roughCut_->projectChanged();
                }
                emit self->changed(); emit self->modifiedChanged();
            }
            self->setBusy(false, success ? QStringLiteral("旧格式工程已保存。") : error);
            emit self->saved(success, path, self->status_);
            if (requestingModule == QStringLiteral("subtitle")) emit self->editor_->projectSaveFinished(success, path, self->status_);
            else if (requestingModule == QStringLiteral("roughcut")) emit self->roughCut_->projectSaveFinished(success, path, self->status_);
        }, Qt::QueuedConnection);
    });
    return true;
}

bool ProjectSession::backupProject(const QUrl &destination, bool portable)
{
    if (!unified() || !ready() || path_.isEmpty()) return false;
    const QString path = destination.toLocalFile(), source = path_;
    if (path.isEmpty()) return false;
    cancel_ = false; setBusy(true, QStringLiteral("正在备份已保存的统一工程…"));
    const QPointer<ProjectSession> self(this);
    auto *cancel = &cancel_;
    task_ = context_->submitProjectStorage([self, cancel, source, path, portable] {
        QString error;
        const bool success = ProjectStore(source).backup(path, &error, portable
            ? ProjectStore::BackupMode::Portable : ProjectStore::BackupMode::ExternalMediaReferences, cancel);
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, success, path, error] {
            if (!self || self->shuttingDown_) return;
            self->setBusy(false, success ? QStringLiteral("备份完成，未保存编辑不包含在备份中。") : error);
            emit self->backedUp(success, path, self->status_);
        }, Qt::QueuedConnection);
    });
    return true;
}

void ProjectSession::cancel() { cancelPendingSave(); cancel_ = true; }

void ProjectSession::shutdown(bool cleanExit)
{
    if (shuttingDown_) return;
    if (!cleanExit) persistRecovery();
    recoveryIdleTimer_.stop(); recoveryDeadlineTimer_.stop();
    shuttingDown_ = true; cancel_ = true;
    cancelPendingSave();
    emit aboutToChange();
    task_.waitForFinished();
    recoveryTask_.waitForFinished();
    manager_.close();
    draftLock_.reset();
    if (cleanExit) {
        // 正常退出代表用户已决定保存或放弃，不能在下次启动时恢复已放弃的修改。
        QFile::remove(QDir(recoveryRoot()).filePath(projectId() + QStringLiteral(".json")));
        releaseRecoveryWorkspace(recoveredStoragePath_, recoveryRoot());
        recoveredStoragePath_.clear();
    }
    if ((cleanExit || (!modified() && recoveryDrafts_.isEmpty())) && draftStorage_) draftStorage_->setAutoRemove(true);
    draftStorage_.reset();
}
} // namespace subcue
