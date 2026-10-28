#include "voice_library_controller.h"
#include "application_context.h"
#include "project_session.h"
#include "tts/voice_library.h"
#include "tts/cloud_tts_provider.h"
#include "project/project_store.h"
#include "tts/audio_quality.h"
#include "tts/reference_samples.h"
#include "asr/asr_provider_factory.h"
#include "asr/asr_provider_catalog.h"
#include "inference/inference_manager.h"
#include "roughcut/rough_cut_project.h"
#include <QtCore/QUuid>
#include <QtCore/QSet>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtCore/QCryptographicHash>
#include <exception>
#include <stdexcept>
#include <QtCore/QDateTime>
#include <QtCore/QSaveFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QDate>
#include <QtCore/QLockFile>
#include <QtNetwork/QHostAddress>
#include <cmath>
#include <algorithm>

namespace subcue {
VoiceLibraryController::VoiceLibraryController(ApplicationContext *context, ProjectSession *session, QString path, QObject *parent)
    : QObject(parent), context_(context), session_(session), path_(path.isEmpty()
        ? QFileInfo(context->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voices.sqlite")) : std::move(path))
{
    requests_.setMaxThreadCount(1);
    reloadCloudClone();
    connect(session_, &ProjectSession::aboutToChange, this, &VoiceLibraryController::cancelAndWait);
}
VoiceLibraryController::~VoiceLibraryController() { cancelAndWait(); }
void VoiceLibraryController::cancel() { cancel_ = true; if (previewLoading_) previewCancel_ = true; }
void VoiceLibraryController::cancelAndWait()
{
    cancel(); previewCancel_ = true; requests_.waitForDone(); context_->clearCloudTask(this); task_.waitForFinished(); audioTask_.waitForFinished(); ++epoch_; cloudPreview_.clear(); cloudPreviewDirectory_.reset();
    if (cloudVoiceMutation_) { context_->endCloudMutation(); cloudVoiceMutation_ = false; }
    previewLoading_ = false;
    if (creating_) {
        creating_ = false; committingCreation_ = false; samples_.clear(); sampleDirectory_.reset(); derivedDirectory_.reset();
    }
    if (catalog_.value(QStringLiteral("state")) == QStringLiteral("loading")) { catalog_.insert(QStringLiteral("state"), QStringLiteral("cancelled")); catalog_.insert(QStringLiteral("message"), QStringLiteral("查询已取消，当前声音绑定保留。")); }
    if (busy_) { busy_ = false; status_ = QStringLiteral("操作已停止；已提交的声音版本保留。"); }
    emit changed();
}
void VoiceLibraryController::submit(std::function<bool(VoiceLibrary &, QString &, QString &)> operation, const QString &successText,
    int profileOffset, int versionOffset, int includeArchived)
{
    if (busy_) return;
    busy_ = true; cancel_ = false; status_ = QStringLiteral("正在处理声音库…"); emit changed();
    const QPointer<VoiceLibraryController> self(this);
    const auto epoch = ++epoch_;
    const auto path = path_; const auto selected = selected_; auto *cancel = &cancel_;
    profileOffset = profileOffset < 0 ? profileOffset_ : std::max(0, profileOffset);
    versionOffset = std::max(0, versionOffset);
    includeArchived = includeArchived < 0 ? (includeArchived_ ? 1 : 0) : includeArchived;
    task_ = context_->submitProjectStorage([self, epoch, path, selected, cancel, profileOffset, versionOffset, includeArchived, operation = std::move(operation), successText] {
        QString error; QString profileId = selected;
        QVariantList profiles, versions;
        bool success = false;
        try {
            VoiceLibrary library(path);
            success = !cancel->load() && library.initialize(&error) && operation(library, profileId, error);
            if (error.isEmpty() && !cancel->load()) {
                profiles = library.profiles(includeArchived != 0, profileOffset, 50, &error).toVariantList();
                if (!profileId.isEmpty()) {
                    const QString defaultVersion = library.defaultVersion(profileId, &error);
                    for (const auto &value : library.versions(profileId, versionOffset, 50, &error)) {
                        auto version = library.version(value.toObject().value(QStringLiteral("id")).toString(), &error);
                        if (!version) break;
                        version->insert(QStringLiteral("isDefault"), version->value(QStringLiteral("id")).toString() == defaultVersion);
                        versions.append(version->toVariantMap());
                    }
                }
            }
        } catch (const std::exception &) { error = QStringLiteral("声音库处理发生异常，请检查磁盘和权限。"); }
        success = success && error.isEmpty() && !cancel->load();
        if (cancel->load()) error = QStringLiteral("操作已取消；已提交的声音版本保留。");
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, profileId, profiles, versions, profileOffset, versionOffset, includeArchived, success, error, successText] {
            if (!self || self->epoch_ != epoch) return;
            if (error.isEmpty()) {
                self->selected_ = profileId; self->profiles_ = profiles; self->versions_ = versions;
                self->profileOffset_ = profileOffset; self->versionOffset_ = versionOffset; self->includeArchived_ = includeArchived != 0;
            }
            self->busy_ = false; self->status_ = error.isEmpty() ? successText : error;
            if (self->committingCreation_) {
                self->committingCreation_ = false;
                if (error.isEmpty()) { self->creating_ = false; self->samples_.clear(); self->sampleDirectory_.reset(); }
            }
            emit self->changed(); emit self->operationFinished(success);
        }, Qt::QueuedConnection);
    });
}
void VoiceLibraryController::refresh(const QString &profileId, bool includeArchived, int profileOffset, int versionOffset)
{
    submit([profileId](VoiceLibrary &, QString &selected, QString &) { selected = profileId; return true; }, QStringLiteral("声音库已读取。"), std::max(0, profileOffset), std::max(0, versionOffset), includeArchived ? 1 : 0);
}
QVariantMap VoiceLibraryController::creationCapabilities() const
{
    const auto provider = AsrProviderFactory::providerIdFromSettings(context_->settings);
    const bool local = provider == QLatin1String("qwen3") || provider == QLatin1String("funasr");
    const auto python = context_->settings.value(QStringLiteral("localAsrPython")).toString();
    const bool ready = local && QFileInfo(python.isEmpty() || python == QLatin1String("python") ? QString::fromUtf8(SUBCUE_PROJECT_INFERENCE_PYTHON) : python).isFile()
        && ModelLocator::modelReady(ModelLocator::directoryFor(AsrProviderCatalog::byId(provider).modelKind, context_->settings))
        && (provider != QLatin1String("qwen3") || ModelLocator::modelReady(ModelLocator::directoryFor(ModelKind::Qwen3ForcedAligner, context_->settings)));
    return {{QStringLiteral("localAsrReady"), ready}, {QStringLiteral("reference"), true}, {QStringLiteral("training"), false},
        {QStringLiteral("modelImport"), false}, {QStringLiteral("cloud"), true}};
}
void VoiceLibraryController::beginCreation()
{
    if (busy_) return;
    sampleDirectory_.reset(); samples_.clear(); creating_ = true; committingCreation_ = false;
    status_ = QStringLiteral("添加参考音频，检查并确认后创建音色。"); emit changed();
}
void VoiceLibraryController::cancelCreation()
{
    if (!creating_) return;
    cancelAndWait(); samples_.clear(); sampleDirectory_.reset(); derivedDirectory_.reset();
    creating_ = false; committingCreation_ = false; status_ = QStringLiteral("添加已取消，已有音色保留。"); emit changed();
}
void VoiceLibraryController::confirmCreationSample(int index, bool confirmed, const QString &language)
{
    if (!creating_ || busy_ || index < 0 || index >= samples_.size()
        || !QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}.contains(language)) return;
    auto sample = samples_[index].toMap();
    if (sample.contains(QStringLiteral("error"))) return;
    sample.insert(QStringLiteral("confirmed"), confirmed); sample.insert(QStringLiteral("referenceLanguage"), language);
    samples_[index] = sample; emit changed();
}
void VoiceLibraryController::cropCreationSample(int index, double startSeconds, double endSeconds)
{
    if (!creating_ || busy_ || index < 0 || index >= samples_.size()) return;
    const auto sample = samples_[index].toMap();
    const double duration = sample.value(QStringLiteral("endSeconds")).toDouble() - sample.value(QStringLiteral("startSeconds")).toDouble();
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) || startSeconds < 0 || endSeconds <= startSeconds || endSeconds > duration + 0.001) {
        status_ = QStringLiteral("裁切范围需在当前样本内，结束时间须大于开始时间。"); emit changed(); return;
    }
    const auto directory = sampleDirectory_; if (!directory) return;
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; auto *cancel = &cancel_; const QPointer<VoiceLibraryController> self(this);
    audioTask_ = context_->submitAudioProcessing([self, epoch, sample, index, startSeconds, endSeconds, directory, cancel] {
        QString error; const auto path = directory->filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".wav"));
        std::optional<AudioQuality> quality;
        try {
            if (ReferenceSamples::derive(sample.value(QStringLiteral("referencePath")).toString(), path, startSeconds, endSeconds, false, false, &error, cancel)) quality = AudioQuality::inspect(path, &error, cancel);
        } catch (...) { error = QStringLiteral("裁切失败，原样本保留。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, index, sample, path, startSeconds, endSeconds, quality, error, directory, cancel] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false;
            if (quality && error.isEmpty() && !cancel->load()) {
                auto updated = sample; updated.insert(QStringLiteral("referencePath"), path);
                updated.insert(QStringLiteral("referenceSha256"), QString::fromLatin1(quality->sha256.toHex())); updated.insert(QStringLiteral("quality"), quality->referenceReview().toVariantMap());
                updated.insert(QStringLiteral("startSeconds"), sample.value(QStringLiteral("startSeconds")).toDouble() + startSeconds);
                updated.insert(QStringLiteral("endSeconds"), sample.value(QStringLiteral("startSeconds")).toDouble() + endSeconds);
                updated.insert(QStringLiteral("transcript"), QString()); updated.insert(QStringLiteral("confirmed"), false); self->samples_[index] = updated;
                QFile::remove(sample.value(QStringLiteral("referencePath")).toString());
                self->status_ = QStringLiteral("裁切已完成，请重新核对转写并确认。");
            } else { QFile::remove(path); self->status_ = cancel->load() ? QStringLiteral("裁切已取消，原样本保留。") : error; }
            emit self->changed(); emit self->operationFinished(quality.has_value() && error.isEmpty() && !cancel->load());
        }, Qt::QueuedConnection);
    });
    emit changed();
}
void VoiceLibraryController::createFromSamples(const QString &name, bool transcriptRequired)
{
    if (!creating_ || busy_) return;
    QJsonArray entries;
    for (const auto &value : samples_) {
        const auto sample = value.toMap();
        if (!sample.value(QStringLiteral("selected")).toBool()) continue;
        if (!sample.value(QStringLiteral("confirmed")).toBool() || sample.contains(QStringLiteral("error"))
            || (transcriptRequired && sample.value(QStringLiteral("transcript")).toString().trimmed().isEmpty())) {
            status_ = QStringLiteral("请逐项试听并确认勾选的素材及转写；未确认的素材不能创建音色。"); emit changed(); return;
        }
        entries.append(QJsonObject{{QStringLiteral("referencePath"), sample.value(QStringLiteral("referencePath")).toString()},
            {QStringLiteral("metadata"), QJsonObject{{QStringLiteral("referenceText"), sample.value(QStringLiteral("transcript")).toString().trimmed()},
                {QStringLiteral("referenceLanguage"), sample.value(QStringLiteral("referenceLanguage"), QStringLiteral("Chinese")).toString()},
                {QStringLiteral("qualityState"), QStringLiteral("approved")}, {QStringLiteral("qualityWarningsAccepted"), true},
                {QStringLiteral("quality"), QJsonObject::fromVariantMap(sample.value(QStringLiteral("quality")).toMap())}}}});
    }
    if (entries.isEmpty() || name.trimmed().isEmpty()) { status_ = QStringLiteral("请填写音色名称，并至少确认一项素材。"); emit changed(); return; }
    committingCreation_ = true; const auto directory = sampleDirectory_; auto *cancel = &cancel_;
    submit([name, entries, directory, cancel](VoiceLibrary &library, QString &selected, QString &error) {
        const auto id = library.createWithVersions(name, entries, &error, cancel);
        if (!id) return false; selected = *id; return true;
    }, QStringLiteral("音色已创建，可选择已确认的参考版本用于配音。"));
}
void VoiceLibraryController::createProfile(const QString &name)
{
    submit([name](VoiceLibrary &library, QString &selected, QString &error) {
        const auto id = library.createProfile(name, &error); if (!id) return false; selected = *id; return true;
    }, QStringLiteral("音色已建立，请添加参考或云音色版本。"));
}
void VoiceLibraryController::renameProfile(const QString &name)
{
    submit([name](VoiceLibrary &library, QString &selected, QString &error) { return library.renameProfile(selected, name, &error); }, QStringLiteral("档案已重命名，已有版本与项目不变。"));
}
void VoiceLibraryController::archiveProfile()
{
    submit([](VoiceLibrary &library, QString &selected, QString &error) {
        if (!library.setProfileArchived(selected, true, &error)) return false;
        selected.clear(); return true;
    }, QStringLiteral("档案已停用，项目副本保留。"));
}
void VoiceLibraryController::restoreProfile()
{
    submit([](VoiceLibrary &library, QString &selected, QString &error) {
        return library.setProfileArchived(selected, false, &error);
    }, QStringLiteral("音色已恢复，旧版本保留。"));
}
void VoiceLibraryController::addLocalVersion(const QUrl &reference, const QString &transcript, bool transcriptRequired)
{
    if (busy_) return;
    if (!reference.isLocalFile() || (transcriptRequired && transcript.trimmed().isEmpty())) { status_ = QStringLiteral("请选择本地参考文件并填写转写。"); emit changed(); return; }
    busy_ = true; cancel_ = false; status_ = QStringLiteral("正在检查参考原件，审核后才可用于克隆。"); emit changed();
    auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceLibraryController> self(this);
    audioTask_ = context_->submitAudioProcessing([self, epoch, reference, transcript, cancel] {
        QString error; std::optional<AudioQuality> quality;
        try { quality = AudioQuality::inspect(reference.toLocalFile(), &error, cancel); }
        catch (...) { error = QStringLiteral("参考原件检查失败，请检查文件和可用内存。"); }
        if (quality && quality->peak == 0) error = QStringLiteral("参考原件全为静音，不能用于克隆。");
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, reference, transcript, cancel, quality, error] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false;
            if (cancel->load() || !quality || !error.isEmpty()) {
                self->status_ = cancel->load() ? QStringLiteral("导入已取消。") : error; emit self->changed(); emit self->operationFinished(false); return;
            }
            self->submit([reference, transcript, quality, cancel](VoiceLibrary &library, QString &selected, QString &error) {
                return library.addVersion(selected, {{QStringLiteral("referenceText"), transcript.trimmed()},
                    {QStringLiteral("qualityState"), QStringLiteral("pending")}, {QStringLiteral("quality"), quality->toJson()}}, reference.toLocalFile(), &error, cancel).has_value();
            }, QStringLiteral("参考原件已保存，仍需试听审核转写。"));
        }, Qt::QueuedConnection);
    });
}
QVariantList VoiceLibraryController::builtinVoices() const
{
    // 官方目录 v2，核对日期 2026-10-17；仅列出当前适配器支持的北京中文音色，不代表账号权限已验证。
    // 来源：https://help.aliyun.com/zh/model-studio/qwen-audio-tts-voice-list
    return {
        QVariantMap{{QStringLiteral("name"), QStringLiteral("于小云")}, {QStringLiteral("id"), QStringLiteral("yuxiaoyun_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 元气、亲切、自然")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cfd.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("乔小娇")}, {QStringLiteral("id"), QStringLiteral("qiaoxiaojiao_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 俏丽、可爱")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cfc.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("夏小晨")}, {QStringLiteral("id"), QStringLiteral("xiaxiaochen_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 元气、明亮")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cf2.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("安明远")}, {QStringLiteral("id"), QStringLiteral("anmingyuan_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 清亮、自然")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cf1.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("温怀清")}, {QStringLiteral("id"), QStringLiteral("wenhuaiqing_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 清亮、柔和")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cf0.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("安小岚")}, {QStringLiteral("id"), QStringLiteral("anxiaolan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 清甜、纯净")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cf7.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("谢舒柔")}, {QStringLiteral("id"), QStringLiteral("xieshurou_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 柔和、自然、知性")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9cf5.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("白清岚")}, {QStringLiteral("id"), QStringLiteral("baiqinglan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 明亮、清纯")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0b.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("许玉远")}, {QStringLiteral("id"), QStringLiteral("xuyuyuan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 知性、成熟、质感")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0a.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("安若柔")}, {QStringLiteral("id"), QStringLiteral("anruorou_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 气声、知性")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c09.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("闻怀之")}, {QStringLiteral("id"), QStringLiteral("wenhuaizhi_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 稳重、成熟")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c08.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("萧行之")}, {QStringLiteral("id"), QStringLiteral("xiaoxingzhi_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 端庄、贵气")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0f.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("顾云舒")}, {QStringLiteral("id"), QStringLiteral("guyunshu_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 成熟、稳重")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0e.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("霍拙石")}, {QStringLiteral("id"), QStringLiteral("huozhuoshi_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 清亮")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0d.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("叶清禾")}, {QStringLiteral("id"), QStringLiteral("yeqinghe_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 亲切、温柔")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c0c.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("云欢欢")}, {QStringLiteral("id"), QStringLiteral("yunhuanhuan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 高亢、热情")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c03.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("徐小俏")}, {QStringLiteral("id"), QStringLiteral("xuxiaoqiao_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 自然、俏皮")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c02.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("白安然")}, {QStringLiteral("id"), QStringLiteral("baianran_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 低沉、浑厚、气声")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c01.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("许言初")}, {QStringLiteral("id"), QStringLiteral("xuyanchu_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 沉稳、磁性")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c00.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("叶知晴")}, {QStringLiteral("id"), QStringLiteral("yezhiqing_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 轻快、自然")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c05.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("安迪")}, {QStringLiteral("id"), QStringLiteral("andi_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · ABC口音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c1b.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("安语晴")}, {QStringLiteral("id"), QStringLiteral("anyuqing_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 甜妹")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c1a.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安元妃")}, {QStringLiteral("id"), QStringLiteral("longanyuanfei_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 高傲妃子音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c2a.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙杰力豆")}, {QStringLiteral("id"), QStringLiteral("longjielidou_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 天真男童音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c29.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安灵希")}, {QStringLiteral("id"), QStringLiteral("longanlingxi_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 可爱甜美音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c2f.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙火火")}, {QStringLiteral("id"), QStringLiteral("longhuohuo_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 顽皮少年音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c28.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙应桃")}, {QStringLiteral("id"), QStringLiteral("longyingtao_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 温柔淡定女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c2e.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安雅")}, {QStringLiteral("id"), QStringLiteral("longanya_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 高雅气质女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c2d.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙婉")}, {QStringLiteral("id"), QStringLiteral("longwan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 细腻柔声女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c2c.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙星")}, {QStringLiteral("id"), QStringLiteral("longxing_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 温婉邻家女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c23.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙华")}, {QStringLiteral("id"), QStringLiteral("longhua_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 元气甜美女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c22.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙寒")}, {QStringLiteral("id"), QStringLiteral("longhan_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 温暖痴情男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c21.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安智")}, {QStringLiteral("id"), QStringLiteral("longanzhi_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 睿智轻熟男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c20.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙哲")}, {QStringLiteral("id"), QStringLiteral("longzhe_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 呆板大暖男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c27.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安洋")}, {QStringLiteral("id"), QStringLiteral("longanyang_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 阳光大男孩")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c26.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("李白")}, {QStringLiteral("id"), QStringLiteral("libai_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 古代诗仙男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c25.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙铃")}, {QStringLiteral("id"), QStringLiteral("longling_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 稚气呆板女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c24.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙牛牛")}, {QStringLiteral("id"), QStringLiteral("longniuniu_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 阳光男童声")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3b.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙闪闪")}, {QStringLiteral("id"), QStringLiteral("longshanshan_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 戏剧化童声")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3a.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙泡泡")}, {QStringLiteral("id"), QStringLiteral("longpaopao_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 飞天泡泡音")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c39.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("loongstella")}, {QStringLiteral("id"), QStringLiteral("loongstella_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 飒爽利落女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c38.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙媛")}, {QStringLiteral("id"), QStringLiteral("longyuan_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 温暖治愈女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3f.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙妙")}, {QStringLiteral("id"), QStringLiteral("longmiao_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 抑扬顿挫女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3e.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙三叔")}, {QStringLiteral("id"), QStringLiteral("longsanshu_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 沉稳质感男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3d.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安莉")}, {QStringLiteral("id"), QStringLiteral("longanli_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 利落从容女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c3c.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安温")}, {QStringLiteral("id"), QStringLiteral("longanwen_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 优雅知性女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c33.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安朗")}, {QStringLiteral("id"), QStringLiteral("longanlang_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 清爽利落男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c32.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙小夏")}, {QStringLiteral("id"), QStringLiteral("longxiaoxia_v3.1")}, {QStringLiteral("description"), QStringLiteral("女 · 沉稳权威女")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c31.wav")}},
        QVariantMap{{QStringLiteral("name"), QStringLiteral("龙安冲")}, {QStringLiteral("id"), QStringLiteral("longanchong_v3.1")}, {QStringLiteral("description"), QStringLiteral("男 · 激情推销男")}, {QStringLiteral("language"), QStringLiteral("Chinese")}, {QStringLiteral("previewUrl"), QStringLiteral("https://g-adoc.alcasset.com/media/maas_docs/sfm-cn/common/audios/6a4b3c2d1e0f9c30.wav")}}
    };
}
void VoiceLibraryController::useBuiltinVoice(const QString &voiceId, bool currentProfile)
{
    if (busy_) return;
    QString name;
    for (const auto &value : builtinVoices()) if (value.toMap().value(QStringLiteral("id")).toString() == voiceId) name = value.toMap().value(QStringLiteral("name")).toString();
    if (name.isEmpty()) { status_ = QStringLiteral("该音色不在当前型号的官方目录中。"); emit changed(); return; }
    QString key, error;
    auto metadata = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (metadata.isEmpty()) { status_ = error; catalog_ = {{QStringLiteral("state"), QStringLiteral("unavailable")}, {QStringLiteral("message"), error}}; emit changed(); return; }
    metadata.remove(QStringLiteral("cloudModelId"));
    metadata.insert(QStringLiteral("provider"), QStringLiteral("qwen-cloud"));
    metadata.insert(QStringLiteral("providerVoiceId"), voiceId);
    metadata.insert(QStringLiteral("preset"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("language"), QStringLiteral("Chinese")}});
    metadata.insert(QStringLiteral("catalogOrigin"), QStringLiteral("https://help.aliyun.com/zh/model-studio/qwen-audio-tts-voice-list"));
    metadata.insert(QStringLiteral("catalogVersion"), QStringLiteral("2/2026-10-17"));
    storeCloudVoice(metadata, name, currentProfile);
}
void VoiceLibraryController::storeCloudVoice(const QJsonObject &binding, const QString &name, bool currentProfile)
{
    auto metadata = binding; QString error;
    const auto revision = CloudTtsProvider::voiceRevision(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-revisions")), metadata, &error);
    if (!revision || binding.value(QStringLiteral("cloudVoiceRevision")).toString() != *revision) { status_ = revision ? QStringLiteral("云端音色已更新，请刷新并查询后重新选用；旧音频保留。") : error; emit changed(); return; }
    if (!revision->isEmpty()) metadata.insert(QStringLiteral("cloudVoiceRevision"), *revision);
    const QPointer<VoiceLibraryController> self(this); auto *cancel = &cancel_; const auto epoch = epoch_ + 1;
    submit([self, metadata, name, cancel, epoch, currentProfile](VoiceLibrary &library, QString &selected, QString &error) {
        // 相同账号、地域、Workspace、型号和官方音色重复选用时复用不可变版本。
        for (int offset = 0; !cancel->load(); offset += 50) {
            const auto profiles = library.profiles(false, offset, 50, &error);
            for (const auto &profile : profiles) {
                if (currentProfile && profile.toObject().value(QStringLiteral("id")).toString() != selected) continue;
                for (int page = 0; !cancel->load(); page += 50) {
                    const auto versions = library.versions(profile.toObject().value(QStringLiteral("id")).toString(), page, 50, &error);
                    for (const auto &row : versions) {
                        const auto found = library.version(row.toObject().value(QStringLiteral("id")).toString(), &error);
                        bool sameBinding = found.has_value();
                        const auto binding = found ? found->value(QStringLiteral("metadata")).toObject() : QJsonObject{};
                        for (const auto &field : {QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("cloudAccountId"), QStringLiteral("region"), QStringLiteral("businessSpace"), QStringLiteral("providerVoiceId"), QStringLiteral("cloudVoiceRevision")})
                            if (binding.value(field) != metadata.value(field)) sameBinding = false;
                        if (sameBinding) {
                            selected = profile.toObject().value(QStringLiteral("id")).toString();
                            const auto id = found->value(QStringLiteral("id")).toString();
                            if (self) QMetaObject::invokeMethod(self.data(), [self, id, name, epoch] { if (self && self->epoch_ == epoch && !self->cancel_.load()) emit self->voiceSelected(id, name); }, Qt::QueuedConnection);
                            return true;
                        }
                    }
                    if (!error.isEmpty() || versions.size() < 50) break;
                }
            }
            if (!error.isEmpty() || profiles.size() < 50) break;
        }
        if (cancel->load() || !error.isEmpty()) return false;
        std::optional<QString> version;
        if (!currentProfile || selected.isEmpty()) {
            const auto profile = library.createWithVersions(name, {QJsonObject{{QStringLiteral("metadata"), metadata}}}, &error, cancel);
            if (!profile) return false;
            selected = *profile; version = library.defaultVersion(selected, &error);
        } else version = library.addVersion(selected, metadata, {}, &error, cancel);
        if (!version || version->isEmpty()) return false;
        if (self) QMetaObject::invokeMethod(self.data(), [self, version, name, epoch] { if (self && self->epoch_ == epoch && !self->cancel_.load()) emit self->voiceSelected(*version, name); }, Qt::QueuedConnection);
        return true;
    }, QStringLiteral("官方系统音色已选用；未调用生成，权限在实际生成时验证。"), 0);
}
void VoiceLibraryController::addCloudVersion(const QString &voiceId, const QString &businessSpace, const QString &credentialReference)
{
    Q_UNUSED(businessSpace); Q_UNUSED(credentialReference);
    // 手动关联同样必须确认型号和审核状态，不能仅凭 ID 把未知声音标为可用。
    for (const auto &value : builtinVoices()) if (value.toMap().value(QStringLiteral("id")).toString() == voiceId) { useBuiltinVoice(voiceId, true); return; }
    discoverVoices(true, voiceId);
}

void VoiceLibraryController::previewCloudClone(const QString &url, const QString &prefix, double priceCny, const QString &priceSource, const QString &priceDate)
{
    if (busy_) return;
    // 每次预览都重读记录，避免另一个实例或取消后的结果被旧内存覆盖。
    QLockFile reviewLock(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json.lock")));
    reviewLock.setStaleLockTime(0);
    if (!reviewLock.tryLock(0)) { status_ = QStringLiteral("其它实例正在创建音色，请稍后重读记录。"); emit changed(); return; }
    reloadCloudClone();
    if (cloudClone_.value(QStringLiteral("state")) == QStringLiteral("unknown")) {
        status_ = QStringLiteral("上次云端音色操作结果不明；请先到服务商控制台核对，不会自动重发。"); emit changed(); return;
    }
    const QUrl reference(url, QUrl::StrictMode), source(priceSource, QUrl::StrictMode);
    if (!reference.isValid() || reference.scheme() != QStringLiteral("https") || reference.host().isEmpty()
        || !reference.userInfo().isEmpty() || reference.hasFragment() || reference.port(443) != 443 || url.size() > 4096
        || reference.host() == QStringLiteral("localhost") || reference.host().endsWith(QStringLiteral(".localhost"))
        || reference.host().endsWith(QStringLiteral(".local")) || !QHostAddress(reference.host()).isNull()) {
        status_ = QStringLiteral("请提供已获授权、公网可访问的 HTTPS 音频链接；不接受本地路径、IP 地址或登录凭据。"); emit changed(); return;
    }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9]{1,10}$")).match(prefix).hasMatch()
        || !std::isfinite(priceCny) || priceCny < 0 || priceCny > 10000
        || !source.isValid() || source.scheme() != QStringLiteral("https") || source.host().isEmpty() || !source.userInfo().isEmpty()
        || source.hasQuery() || source.hasFragment() || priceSource.size() > 2048
        || !QDate::fromString(priceDate, Qt::ISODate).isValid() || QDate::fromString(priceDate, Qt::ISODate) > QDate::currentDate()) {
        status_ = QStringLiteral("前缀需为 1–10 位英文字母或数字；请填写有效单次价格、HTTPS 价格来源及不晚于今天的核对日期。"); emit changed(); return;
    }
    QString key, error; const auto binding = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (binding.isEmpty()) { status_ = error; emit changed(); return; }
    QFile previous(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json")));
    QByteArray previousBytes;
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 65536) { status_ = QStringLiteral("无法读取音色创建记录，未准备新请求。"); emit changed(); return; }
        previousBytes = previous.readAll();
        const auto previousId = cloudClone_.value(QStringLiteral("id")).toString();
        if (QUuid(previousId).isNull() || QUuid(previousId).toString(QUuid::WithoutBraces) != previousId) { status_ = QStringLiteral("创建记录 ID 损坏，请先核对服务商记录。"); emit changed(); return; }
        const auto history = QFileInfo(previous).absoluteDir().filePath(QStringLiteral("voice-enrollment-history"));
        QSaveFile archive(QDir(history).filePath(previousId + QStringLiteral(".json")));
        if (!QDir().mkpath(history) || !archive.open(QIODevice::WriteOnly) || archive.write(previousBytes) != previousBytes.size() || !archive.commit()) { status_ = QStringLiteral("历史创建记录无法归档，未准备新请求。"); emit changed(); return; }
    }
    cloudClone_ = {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("state"), QStringLiteral("prepared")}, {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("previousRecordSha256"), QString::fromLatin1(QCryptographicHash::hash(previousBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("binding"), binding}, {QStringLiteral("configRevision"), context_->cloudConfig->load().value(QStringLiteral("revision"))},
        {QStringLiteral("referenceUrl"), reference.toString(QUrl::FullyEncoded)}, {QStringLiteral("referenceHost"), reference.host()},
        {QStringLiteral("referenceUrlSha256"), QString::fromLatin1(QCryptographicHash::hash(reference.toEncoded(), QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("prefix"), prefix}, {QStringLiteral("priceCny"), priceCny}, {QStringLiteral("priceSource"), priceSource}, {QStringLiteral("priceDate"), priceDate}};
    for (const auto &value : context_->cloudConfig->load().value(QStringLiteral("accounts")).toArray())
        if (value.toObject().value(QStringLiteral("id")) == binding.value(QStringLiteral("cloudAccountId"))) cloudClone_.insert(QStringLiteral("accountName"), value.toObject().value(QStringLiteral("name")));
    status_ = QStringLiteral("预览就绪：创建 1 个北京 Flash 音色，价格为人工快照；尚未提交或上传。"); emit changed();
}

void VoiceLibraryController::discardCloudClone()
{
    if (busy_ || !QStringList{QStringLiteral("prepared"), QStringLiteral("delete_prepared"), QStringLiteral("update_prepared")}.contains(cloudClone_.value(QStringLiteral("state")).toString())) return;
    cloudClone_ = {}; status_ = QStringLiteral("音色操作预览已取消，未发送请求。"); emit changed();
}

void VoiceLibraryController::reloadCloudClone()
{
    if (busy_) return;
    QFile file(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json")));
    cloudClone_ = {};
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) cloudClone_ = QJsonDocument::fromJson(file.readAll()).object();
        if (cloudClone_.value(QStringLiteral("schemaVersion")).toInt() != 1 || !QStringList{QStringLiteral("created"), QStringLiteral("unknown"), QStringLiteral("acknowledged"), QStringLiteral("deleted"), QStringLiteral("delete_blocked"), QStringLiteral("updated"), QStringLiteral("update_blocked")}.contains(cloudClone_.value(QStringLiteral("state")).toString()))
            cloudClone_ = {{QStringLiteral("state"), QStringLiteral("unknown")}, {QStringLiteral("message"), QStringLiteral("音色操作记录无法读取；请先在服务商控制台核对。")}};
    }
    emit changed();
}

void VoiceLibraryController::acknowledgeCloudClone(bool checked)
{
    if (busy_ || !checked || cloudClone_.value(QStringLiteral("state")) != QStringLiteral("unknown")) return;
    const auto path = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json"));
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { status_ = QStringLiteral("其它实例正在创建音色，请稍后重读记录。"); emit changed(); return; }
    const auto id = cloudClone_.value(QStringLiteral("id")).toString(); reloadCloudClone();
    if (id.isEmpty() || cloudClone_.value(QStringLiteral("id")).toString() != id || cloudClone_.value(QStringLiteral("state")) != QStringLiteral("unknown")) return;
    auto record = cloudClone_; record.insert(QStringLiteral("state"), QStringLiteral("acknowledged"));
    record.insert(QStringLiteral("manuallyCheckedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    record.insert(QStringLiteral("message"), QStringLiteral("已记录人工核对，不代表远端操作失败、费用撤销或素材删除；再次操作仍需重新确认。"));
    if (record.value(QStringLiteral("operation")) == QStringLiteral("update")) {
        auto binding = record.value(QStringLiteral("binding")).toObject(); binding.insert(QStringLiteral("providerVoiceId"), record.value(QStringLiteral("voiceId")));
        if (!CloudTtsProvider::saveVoiceRevision(QFileInfo(path).absoluteDir().filePath(QStringLiteral("voice-revisions")), binding, id, false, &status_)) { emit changed(); return; }
    }
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { status_ = QStringLiteral("人工核对记录保存失败，仍禁止重发。"); emit changed(); return; }
    cloudClone_ = record; status_ = record.value(QStringLiteral("message")).toString(); emit changed();
}

void VoiceLibraryController::useCreatedCloudClone()
{
    if (busy_ || !QStringList{QStringLiteral("created"), QStringLiteral("updated")}.contains(cloudClone_.value(QStringLiteral("state")).toString())) return;
    QString key, error;
    if (context_->cloudConfig->resolveSynthesis(&key, &error) != cloudClone_.value(QStringLiteral("binding")).toObject()) { status_ = QStringLiteral("当前账号／型号与创建记录不同，请切回原配置后查询。"); emit changed(); return; }
    discoverVoices(true, cloudClone_.value(QStringLiteral("voiceId")).toString());
}

void VoiceLibraryController::confirmCloudClone(bool rightsAccepted, bool privacyAccepted, bool costAccepted)
{
    if (busy_ || cloudClone_.value(QStringLiteral("state")) != QStringLiteral("prepared")) return;
    if (!rightsAccepted || !privacyAccepted || !costAccepted) { status_ = QStringLiteral("请明确确认声音使用权、服务商读取与留存规则，以及本次创建费用。"); emit changed(); return; }
    QString key, error; const auto binding = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (binding.isEmpty() || binding != cloudClone_.value(QStringLiteral("binding")).toObject()
        || context_->cloudConfig->load().value(QStringLiteral("revision")) != cloudClone_.value(QStringLiteral("configRevision"))) {
        cloudClone_ = {}; status_ = QStringLiteral("账号、凭据或配置已改变，请重新预览并确认。"); emit changed(); return;
    }
    const auto reference = cloudClone_.value(QStringLiteral("referenceUrl")).toString();
    auto record = cloudClone_; record.remove(QStringLiteral("referenceUrl")); record.insert(QStringLiteral("state"), QStringLiteral("unknown"));
    record.insert(QStringLiteral("submittedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    record.insert(QStringLiteral("rightsAccepted"), true); record.insert(QStringLiteral("privacyAccepted"), true); record.insert(QStringLiteral("costAccepted"), true);
    record.insert(QStringLiteral("consentVersion"), 1);
    record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("unknown"));
    record.insert(QStringLiteral("message"), QStringLiteral("请求可能已发送。若未取得 Voice ID，请在服务商控制台核对；不会自动重发。"));
    const auto path = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json"));
    auto lock = std::make_shared<QLockFile>(path + QStringLiteral(".lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) { status_ = QStringLiteral("其它实例正在创建音色，请稍后重读记录。"); emit changed(); return; }
    QFile previous(path); QByteArray previousBytes;
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 65536) { status_ = QStringLiteral("创建记录无法读取，未发送请求。"); emit changed(); return; }
        previousBytes = previous.readAll();
    }
    if (QString::fromLatin1(QCryptographicHash::hash(previousBytes, QCryptographicHash::Sha256).toHex()) != cloudClone_.value(QStringLiteral("previousRecordSha256")).toString()) {
        reloadCloudClone(); status_ = QStringLiteral("创建记录已改变，请重读并重新预览；未发送请求。"); emit changed(); return;
    }
    previous.close();
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { status_ = QStringLiteral("无法持久化创建记录，已阻止发送，请检查磁盘权限。"); emit changed(); return; }
    const QPointer<VoiceLibraryController> self(this);
    if (!context_->trackCloudTask(this, binding.value(QStringLiteral("cloudAccountId")).toString(), [self] { if (self) self->cancelAndWait(); })) {
        cloudClone_ = record; status_ = QStringLiteral("创建记录已保留，请核对账号状态。"); emit changed(); return;
    }
    cloudClone_ = record; busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; auto *http = context_->aiHttp;
    status_ = QStringLiteral("正在提交一次音色创建；取消不能保证远端停止，请保留返回的 Voice ID。"); emit changed();
    requests_.start([self, epoch, cancel, http, binding, key, reference, record, path, lock]() mutable {
        try {
            QtNetworkHttpClient native; HttpRequest request;
            request.url = QUrl(QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/customization").arg(binding.value(QStringLiteral("businessSpace")).toString()));
            request.method = QByteArrayLiteral("POST"); request.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key.toUtf8()}, {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
            request.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), QStringLiteral("voice-enrollment")}, {QStringLiteral("input"), QJsonObject{
                {QStringLiteral("action"), QStringLiteral("create_voice")}, {QStringLiteral("target_model"), binding.value(QStringLiteral("model"))},
                {QStringLiteral("prefix"), record.value(QStringLiteral("prefix"))}, {QStringLiteral("url"), reference}, {QStringLiteral("language_hints"), QJsonArray{QStringLiteral("zh")}},
                {QStringLiteral("max_prompt_audio_length"), 10.0}, {QStringLiteral("enable_preprocess"), false}, {QStringLiteral("enable_volume_normalization"), QStringLiteral("false")}}}}).toJson(QJsonDocument::Compact);
            request.followRedirects = false; request.maxResponseBytes = 65536; request.transferTimeoutMs = 30000;
            const auto response = (http ? http : &native)->send(request, cancel);
            if (const auto *result = std::get_if<HttpResponse>(&response)) {
                record.insert(QStringLiteral("httpStatus"), result->status);
                const auto body = QJsonDocument::fromJson(result->body).object();
                if (body.value(QStringLiteral("usage")).toObject().value(QStringLiteral("count")) == QJsonValue(1)) record.insert(QStringLiteral("reportedVoiceCount"), 1);
                const auto id = body.value(QStringLiteral("output")).toObject().value(QStringLiteral("voice_id")).toString();
                if (result->status == 200 && !id.contains(key) && QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,256}$")).match(id).hasMatch()
                    && id.startsWith(binding.value(QStringLiteral("model")).toString() + QLatin1Char('-') + record.value(QStringLiteral("prefix")).toString() + QLatin1Char('-'))) {
                    record.insert(QStringLiteral("voiceId"), id); record.insert(QStringLiteral("state"), QStringLiteral("created"));
                    record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("voice_id_returned"));
                    record.insert(QStringLiteral("message"), QStringLiteral("已返回 Voice ID，尚未核验审核状态；请查询后关联。未自动生成或修改文稿音色。"));
                }
                if (record.value(QStringLiteral("state")) == QStringLiteral("unknown")) record.insert(QStringLiteral("message"), QStringLiteral("接口返回 HTTP %1，但未取得可信 Voice ID。请在服务商控制台核对，不会自动重发。").arg(result->status));
                const auto requestId = body.value(QStringLiteral("request_id")).toString();
                if (!requestId.contains(key) && QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(requestId).hasMatch()) record.insert(QStringLiteral("requestId"), requestId);
            }
        } catch (...) { record.insert(QStringLiteral("message"), QStringLiteral("创建请求发生异常，远端结果不明；请在服务商控制台核对，不会自动重发。")); }
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) record.insert(QStringLiteral("message"), record.value(QStringLiteral("message")).toString() + QStringLiteral(" 结果落盘失败，请立即记录 Voice ID 并到控制台核对。"));
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, record] {
            if (!self || self->epoch_ != epoch) return;
            self->context_->clearCloudTask(self.data()); self->busy_ = false; self->cloudClone_ = record;
            self->status_ = record.value(QStringLiteral("message")).toString(); emit self->changed(); emit self->operationFinished(record.value(QStringLiteral("state")) == QStringLiteral("created"));
        }, Qt::QueuedConnection);
    });
}
void VoiceLibraryController::previewCloudDeletion(const QString &voiceId)
{
    if (busy_) return;
    QLockFile lock(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { status_ = QStringLiteral("其它实例正在操作云端音色，请稍后重读记录。"); emit changed(); return; }
    reloadCloudClone();
    if (cloudClone_.value(QStringLiteral("state")) == QStringLiteral("unknown")) { status_ = QStringLiteral("上次云端音色操作结果不明，请先到控制台核对，不会自动重发。"); emit changed(); return; }
    QString key, error; const auto binding = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (binding.isEmpty()) { status_ = error; emit changed(); return; }
    if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,256}$")).match(voiceId).hasMatch()
        || !voiceId.startsWith(binding.value(QStringLiteral("model")).toString() + QLatin1Char('-'))
        || voiceId.size() <= binding.value(QStringLiteral("model")).toString().size() + 1 || voiceId.contains(key)) {
        status_ = QStringLiteral("仅支持当前北京 Flash 型号的自定义 Voice ID，系统音色不能删除。"); emit changed(); return;
    }
    QFile previous(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json")));
    QByteArray previousBytes;
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 65536) { status_ = QStringLiteral("音色操作记录无法读取，未准备删除。"); emit changed(); return; }
        previousBytes = previous.readAll(); const auto id = cloudClone_.value(QStringLiteral("id")).toString();
        if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id) { status_ = QStringLiteral("音色操作记录 ID 损坏，请先核对控制台记录。"); emit changed(); return; }
        const auto history = QFileInfo(previous).absoluteDir().filePath(QStringLiteral("voice-enrollment-history"));
        QSaveFile archive(QDir(history).filePath(id + QStringLiteral(".json")));
        if (!QDir().mkpath(history) || !archive.open(QIODevice::WriteOnly) || archive.write(previousBytes) != previousBytes.size() || !archive.commit()) { status_ = QStringLiteral("历史记录无法归档，未准备删除。"); emit changed(); return; }
    }
    cloudClone_ = {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("state"), QStringLiteral("delete_prepared")}, {QStringLiteral("operation"), QStringLiteral("delete")},
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("voiceId"), voiceId}, {QStringLiteral("binding"), binding},
        {QStringLiteral("configRevision"), context_->cloudConfig->load().value(QStringLiteral("revision"))},
        {QStringLiteral("previousRecordSha256"), QString::fromLatin1(QCryptographicHash::hash(previousBytes, QCryptographicHash::Sha256).toHex())}};
    for (const auto &value : context_->cloudConfig->load().value(QStringLiteral("accounts")).toArray())
        if (value.toObject().value(QStringLiteral("id")) == binding.value(QStringLiteral("cloudAccountId"))) cloudClone_.insert(QStringLiteral("accountName"), value.toObject().value(QStringLiteral("name")));
    status_ = QStringLiteral("删除预览就绪，尚未访问网络。确认后先查询型号，再发送一次删除；已有音频与项目声音快照保留。"); emit changed();
}

void VoiceLibraryController::confirmCloudDeletion(const QString &voiceId, bool accepted)
{
    confirmCloudVoiceMutation(voiceId, accepted, false);
}
void VoiceLibraryController::previewCloudUpdate(const QString &voiceId, const QString &url)
{
    if (busy_) return;
    const QUrl reference(url, QUrl::StrictMode);
    if (!reference.isValid() || reference.scheme() != QStringLiteral("https") || reference.host().isEmpty()
        || !reference.userInfo().isEmpty() || reference.hasFragment() || reference.port(443) != 443 || url.size() > 4096
        || reference.host() == QStringLiteral("localhost") || reference.host().endsWith(QStringLiteral(".localhost"))
        || reference.host().endsWith(QStringLiteral(".local")) || !QHostAddress(reference.host()).isNull()) {
        status_ = QStringLiteral("请提供已获授权、公网可访问的 HTTPS 参考链接；不接受本地路径、IP 地址或登录凭据。"); emit changed(); return;
    }
    discardCloudClone(); previewCloudDeletion(voiceId);
    if (cloudClone_.value(QStringLiteral("state")) != QStringLiteral("delete_prepared")) return;
    cloudClone_.insert(QStringLiteral("state"), QStringLiteral("update_prepared")); cloudClone_.insert(QStringLiteral("operation"), QStringLiteral("update"));
    cloudClone_.insert(QStringLiteral("referenceUrl"), reference.toString(QUrl::FullyEncoded)); cloudClone_.insert(QStringLiteral("referenceHost"), reference.host());
    cloudClone_.insert(QStringLiteral("referenceUrlSha256"), QString::fromLatin1(QCryptographicHash::hash(reference.toEncoded(), QCryptographicHash::Sha256).toHex()));
    status_ = QStringLiteral("更新预览就绪，尚未联网。确认后停止本账号云任务，查询型号并更新一次；新声音须重新查询并选用，旧音频保留。"); emit changed();
}
void VoiceLibraryController::confirmCloudUpdate(const QString &voiceId, bool rightsAccepted, bool privacyAccepted, bool costAccepted)
{
    confirmCloudVoiceMutation(voiceId, rightsAccepted && privacyAccepted && costAccepted, true);
}
void VoiceLibraryController::confirmCloudVoiceMutation(const QString &voiceId, bool accepted, bool update)
{
    if (busy_ || cloudClone_.value(QStringLiteral("state")) != (update ? QStringLiteral("update_prepared") : QStringLiteral("delete_prepared"))) return;
    if (!accepted || voiceId != cloudClone_.value(QStringLiteral("voiceId")).toString()) { status_ = QStringLiteral("请完整输入目标 Voice ID，并完成本次操作的全部确认。"); emit changed(); return; }
    QString key, error; const auto binding = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (binding.isEmpty() || binding != cloudClone_.value(QStringLiteral("binding")).toObject()
        || context_->cloudConfig->load().value(QStringLiteral("revision")) != cloudClone_.value(QStringLiteral("configRevision"))) {
        cloudClone_ = {}; status_ = QStringLiteral("账号、凭据或配置已改变，请重新预览操作。"); emit changed(); return;
    }
    const auto path = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-enrollment.json"));
    auto lock = std::make_shared<QLockFile>(path + QStringLiteral(".lock")); lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) { status_ = QStringLiteral("其它实例正在操作云端音色，请稍后重读记录。"); emit changed(); return; }
    QFile previous(path); QByteArray previousBytes;
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 65536) { status_ = QStringLiteral("音色操作记录无法读取，未发送请求。"); emit changed(); return; }
        previousBytes = previous.readAll();
    }
    if (QString::fromLatin1(QCryptographicHash::hash(previousBytes, QCryptographicHash::Sha256).toHex()) != cloudClone_.value(QStringLiteral("previousRecordSha256")).toString()) {
        reloadCloudClone(); status_ = QStringLiteral("操作记录已改变，请重读并重新预览；未发送请求。"); emit changed(); return;
    }
    previous.close();
    const auto reference = cloudClone_.value(QStringLiteral("referenceUrl")).toString();
    auto record = cloudClone_; record.remove(QStringLiteral("referenceUrl")); record.insert(QStringLiteral("state"), QStringLiteral("unknown")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("unknown"));
    record.insert(QStringLiteral("submittedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)); record.insert(update ? QStringLiteral("updateAccepted") : QStringLiteral("deletionAccepted"), true); record.insert(QStringLiteral("consentVersion"), 1);
    if (update) { record.insert(QStringLiteral("rightsAccepted"), true); record.insert(QStringLiteral("privacyAccepted"), true); record.insert(QStringLiteral("costAccepted"), true); }
    record.insert(QStringLiteral("message"), QStringLiteral("云端音色操作可能已发送；取消不能保证远端停止，请到控制台核对，不会自动重发。"));
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile journal(path);
    if (!journal.open(QIODevice::WriteOnly) || journal.write(bytes) != bytes.size() || !journal.commit()) { status_ = QStringLiteral("音色操作记录无法持久化，已阻止发送。"); emit changed(); return; }
    const QPointer<VoiceLibraryController> self(this);
    if (update ? !context_->beginCloudMutation({binding.value(QStringLiteral("cloudAccountId")).toString()})
               : !context_->trackCloudTask(this, binding.value(QStringLiteral("cloudAccountId")).toString(), [self] { if (self) self->cancelAndWait(); })) {
        if (update) {
            record.insert(QStringLiteral("state"), QStringLiteral("update_blocked")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("update_not_sent"));
            record.insert(QStringLiteral("message"), QStringLiteral("无法独占云端变更，未发送更新。请关闭其它 SubCue／旧版实例并等待配置变更结束，再重新预览。"));
            const auto blockedBytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile blocked(path);
            if (!blocked.open(QIODevice::WriteOnly) || blocked.write(blockedBytes) != blockedBytes.size() || !blocked.commit()) { record.insert(QStringLiteral("state"), QStringLiteral("unknown")); record.insert(QStringLiteral("message"), QStringLiteral("未发送更新，但操作状态落盘失败；请保存记录并检查磁盘权限。")); }
        }
        cloudClone_ = record; status_ = update ? record.value(QStringLiteral("message")).toString() : QStringLiteral("操作记录已保留，请核对账号状态。"); emit changed(); return;
    }
    cloudVoiceMutation_ = update;
    cloudClone_ = record; busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; auto *http = context_->aiHttp;
    status_ = QStringLiteral("正在查询目标音色型号；通过后仅发送一次操作。"); emit changed();
    requests_.start([self, epoch, cancel, http, binding, key, record, path, lock, update, reference]() mutable {
        try {
            QtNetworkHttpClient native; HttpRequest request;
            request.url = QUrl(QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/customization").arg(binding.value(QStringLiteral("businessSpace")).toString()));
            request.method = QByteArrayLiteral("POST"); request.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key.toUtf8()}, {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
            request.followRedirects = false; request.maxResponseBytes = 65536; request.transferTimeoutMs = 15000;
            request.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), QStringLiteral("voice-enrollment")}, {QStringLiteral("input"), QJsonObject{
                {QStringLiteral("action"), QStringLiteral("query_voice")}, {QStringLiteral("voice_id"), record.value(QStringLiteral("voiceId"))}}}}).toJson(QJsonDocument::Compact);
            const auto query = (http ? http : &native)->send(request, cancel); const auto *detail = std::get_if<HttpResponse>(&query);
            if (detail) record.insert(QStringLiteral("queryHttpStatus"), detail->status);
            const auto output = detail && detail->status == 200 ? QJsonDocument::fromJson(detail->body).object().value(QStringLiteral("output")).toObject() : QJsonObject{};
            if (cancel->load() || output.value(QStringLiteral("target_model")) != binding.value(QStringLiteral("model"))
                || !QStringList{QStringLiteral("OK"), QStringLiteral("DEPLOYING"), QStringLiteral("UNDEPLOYED")}.contains(output.value(QStringLiteral("status")).toString())) {
                record.insert(QStringLiteral("state"), QStringLiteral("delete_blocked")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("delete_not_sent"));
                record.insert(QStringLiteral("message"), QStringLiteral("删除前查询未通过或已取消，未发送删除请求；请核对账号、型号与远端状态后重新预览。"));
                if (update) { record.insert(QStringLiteral("state"), QStringLiteral("update_blocked")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("update_not_sent")); record.insert(QStringLiteral("message"), QStringLiteral("更新前查询未通过或已取消，未发送更新；请核对账号、型号与远端状态后重新预览。")); }
            } else {
                auto input = QJsonObject{{QStringLiteral("action"), update ? QStringLiteral("update_voice") : QStringLiteral("delete_voice")}, {QStringLiteral("voice_id"), record.value(QStringLiteral("voiceId"))}};
                if (update) {
                    auto revisionBinding = binding; revisionBinding.insert(QStringLiteral("providerVoiceId"), record.value(QStringLiteral("voiceId")));
                    if (!CloudTtsProvider::saveVoiceRevision(QFileInfo(path).absoluteDir().filePath(QStringLiteral("voice-revisions")), revisionBinding, record.value(QStringLiteral("id")).toString(), true)) throw std::runtime_error("voice revision storage failed");
                    input.insert(QStringLiteral("url"), reference);
                }
                request.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), QStringLiteral("voice-enrollment")}, {QStringLiteral("input"), input}}).toJson(QJsonDocument::Compact);
                const auto response = (http ? http : &native)->send(request, cancel);
                if (const auto *result = std::get_if<HttpResponse>(&response)) {
                    record.insert(QStringLiteral("httpStatus"), result->status); const auto body = QJsonDocument::fromJson(result->body).object();
                    const auto requestId = body.value(QStringLiteral("request_id")).toString();
                    const bool validId = !requestId.contains(key) && QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(requestId).hasMatch();
                    if (validId) record.insert(QStringLiteral("requestId"), requestId);
                    if (body.value(QStringLiteral("usage")).toObject().value(QStringLiteral("count")) == QJsonValue(1)) record.insert(QStringLiteral("reportedVoiceCount"), 1);
                    if (result->status == 200 && validId && !body.contains(QStringLiteral("code")) && body.value(QStringLiteral("output")).isObject()
                        && body.value(QStringLiteral("output")).toObject().isEmpty() && record.value(QStringLiteral("reportedVoiceCount")) == QJsonValue(1)) {
                        record.insert(QStringLiteral("state"), QStringLiteral("deleted")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("delete_acknowledged"));
                        record.insert(QStringLiteral("message"), QStringLiteral("服务商已确认删除音色；旧 Voice ID 的后续合成可能失败。已有音频、项目快照及托管源文件保留，不代表服务商其它素材副本已删除。"));
                        if (update) {
                            auto revisionBinding = binding; revisionBinding.insert(QStringLiteral("providerVoiceId"), record.value(QStringLiteral("voiceId")));
                            record.insert(QStringLiteral("state"), QStringLiteral("updated")); record.insert(QStringLiteral("remoteOutcome"), QStringLiteral("update_acknowledged"));
                            record.insert(QStringLiteral("message"), QStringLiteral("服务商已接受音色更新，尚未确认审核完成；请查询并选用新声音版本。旧声音版本不能再次生成，已有音频保留。"));
                            if (!CloudTtsProvider::saveVoiceRevision(QFileInfo(path).absoluteDir().filePath(QStringLiteral("voice-revisions")), revisionBinding, record.value(QStringLiteral("id")).toString(), false)) { record.insert(QStringLiteral("state"), QStringLiteral("unknown")); record.insert(QStringLiteral("message"), QStringLiteral("更新已获接口确认，但修订记录保存失败；请到控制台核对后记录人工核对。")); }
                        }
                    }
                    if (record.value(QStringLiteral("state")) == QStringLiteral("unknown") && record.value(QStringLiteral("remoteOutcome")) == QStringLiteral("unknown")) record.insert(QStringLiteral("message"), QStringLiteral("音色操作接口返回 HTTP %1，但未取得完整确认；请到控制台核对，不会自动重发。").arg(result->status));
                }
            }
        } catch (...) { record.insert(QStringLiteral("message"), QStringLiteral("云端音色操作异常，远端结果不明，请到控制台核对，不会自动重发。")); }
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact); QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) record.insert(QStringLiteral("message"), record.value(QStringLiteral("message")).toString() + QStringLiteral(" 结果落盘失败，请立即保存请求 ID 并核对控制台。"));
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, record] {
            if (!self || self->epoch_ != epoch) return;
            self->context_->clearCloudTask(self.data()); self->busy_ = false; self->cloudClone_ = record;
            if (self->cloudVoiceMutation_) { self->context_->endCloudMutation(); self->cloudVoiceMutation_ = false; }
            self->catalog_ = {{QStringLiteral("state"), QStringLiteral("unloaded")}, {QStringLiteral("message"), QStringLiteral("音色操作后请刷新账号目录，旧列表不能继续选用。")}};
            self->status_ = record.value(QStringLiteral("message")).toString(); emit self->changed(); emit self->operationFinished(QStringList{QStringLiteral("deleted"), QStringLiteral("updated")}.contains(record.value(QStringLiteral("state")).toString()));
        }, Qt::QueuedConnection);
    });
}

void VoiceLibraryController::discoverVoices(bool refresh, const QString &manualId)
{
    if (busy_) return;
    QString key, error; const auto metadata = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (metadata.isEmpty()) { catalog_ = {{QStringLiteral("state"), error.contains(QStringLiteral("Workspace")) ? QStringLiteral("workspace_required") : QStringLiteral("unauthorized")}, {QStringLiteral("message"), error}}; status_ = error; emit changed(); return; }
    if (!manualId.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,256}$")).match(manualId).hasMatch()) { status_ = QStringLiteral("音色 ID 格式无效。"); emit changed(); return; }
    const auto identity = QCryptographicHash::hash(QJsonDocument(QJsonObject{{QStringLiteral("binding"), metadata}, {QStringLiteral("accounts"), context_->cloudConfig->load().value(QStringLiteral("accounts"))}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    const auto cache = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-catalog.json"));
    const auto revisionDirectory = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-revisions"));
    catalogIdentity_ = identity;
    QFile file(cache); QJsonObject cached;
    if (file.open(QIODevice::ReadOnly) && file.size() <= 2 * 1024 * 1024) cached = QJsonDocument::fromJson(file.readAll()).object();
    if (cached.value(QStringLiteral("identity")).toString().toLatin1() != identity) cached = {};
    if (!refresh) {
        catalog_ = cached.isEmpty() ? QJsonObject{{QStringLiteral("state"), QStringLiteral("unloaded")}, {QStringLiteral("message"), QStringLiteral("点击刷新以查询当前账号的自定义音色。")}} : cached;
        if (!cached.isEmpty()) { catalog_.insert(QStringLiteral("state"), QStringLiteral("stale_cache")); catalog_.insert(QStringLiteral("message"), QStringLiteral("已读取账号缓存，非实时状态；可刷新确认权限与审核结果。")); }
        emit changed(); return;
    }
    const auto account = metadata.value(QStringLiteral("cloudAccountId")).toString();
    const QPointer<VoiceLibraryController> self(this);
    if (!context_->trackCloudTask(this, account, [self] { if (self) self->cancelAndWait(); })) return;
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; auto *cancel = &cancel_; auto *http = context_->aiHttp;
    catalog_ = {{QStringLiteral("state"), QStringLiteral("loading")}, {QStringLiteral("message"), QStringLiteral("正在读取账号音色；仅查询元数据，不发起合成。")}}; emit changed();
    requests_.start([self, epoch, cancel, http, metadata, key, identity, cache, cached, manualId, revisionDirectory] {
        QtNetworkHttpClient native; QString state = QStringLiteral("success"), message; QJsonArray rows; QSet<QString> seen;
        const auto query = [&](const QJsonObject &input) {
            HttpRequest request; request.url = QUrl(QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/customization").arg(metadata.value(QStringLiteral("businessSpace")).toString()));
            request.method = QByteArrayLiteral("POST"); request.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key.toUtf8()}, {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
            request.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), QStringLiteral("voice-enrollment")}, {QStringLiteral("input"), input}}).toJson(QJsonDocument::Compact);
            request.followRedirects = false; request.maxResponseBytes = 2 * 1024 * 1024; request.transferTimeoutMs = 15000;
            const auto response = (http ? http : &native)->send(request, cancel);
            if (!std::holds_alternative<HttpResponse>(response)) { state = QStringLiteral("offline"); message = QStringLiteral("网络不可用或请求取消，请刷新重试。"); return QJsonObject{}; }
            const auto result = std::get<HttpResponse>(response);
            if (result.status != 200) { state = result.status == 401 || result.status == 403 ? QStringLiteral("unauthorized") : result.status == 429 ? QStringLiteral("throttled") : QStringLiteral("unsupported"); message = QStringLiteral("音色查询失败（HTTP %1），检查账号权限、地域或业务空间。").arg(result.status); return QJsonObject{}; }
            const auto output = QJsonDocument::fromJson(result.body).object().value(QStringLiteral("output"));
            if (!output.isObject()) { state = QStringLiteral("unsupported"); message = QStringLiteral("音色接口返回结构无效，未冒充空列表。"); }
            return output.toObject();
        };
        for (int page = 0; page < 100 && !cancel->load(); ++page) {
            const auto output = manualId.isEmpty() ? query({{QStringLiteral("action"), QStringLiteral("list_voice")}, {QStringLiteral("page_index"), page}, {QStringLiteral("page_size"), 100}}) : QJsonObject{};
            if (state != QStringLiteral("success")) break;
            if (manualId.isEmpty() && !output.value(QStringLiteral("voice_list")).isArray()) { state = QStringLiteral("unsupported"); message = QStringLiteral("音色列表格式无效。"); break; }
            const auto list = manualId.isEmpty() ? output.value(QStringLiteral("voice_list")).toArray() : QJsonArray{QJsonObject{{QStringLiteral("voice_id"), manualId}}};
            for (const auto &value : list) {
                if (cancel->load()) break;
                const auto id = value.toObject().value(QStringLiteral("voice_id")).toString();
                if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,256}$")).match(id).hasMatch() || id.contains(key) || seen.contains(id)) continue;
                seen.insert(id);
                const auto detail = query({{QStringLiteral("action"), QStringLiteral("query_voice")}, {QStringLiteral("voice_id"), id}});
                if (state != QStringLiteral("success")) break;
                const auto target = detail.value(QStringLiteral("target_model")).toString(), status = detail.value(QStringLiteral("status")).toString();
                auto revisionBinding = metadata; revisionBinding.insert(QStringLiteral("providerVoiceId"), id); QString revisionError;
                const auto revision = CloudTtsProvider::voiceRevision(revisionDirectory, revisionBinding, &revisionError);
                const bool compatible = revision && target == metadata.value(QStringLiteral("model")).toString() && status == QStringLiteral("OK");
                rows.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), QStringLiteral("账号音色 · %1").arg(id.right(12))}, {QStringLiteral("status"), status}, {QStringLiteral("targetModel"), target}, {QStringLiteral("compatible"), compatible}, {QStringLiteral("cloudVoiceRevision"), revision ? *revision : QString()}, {QStringLiteral("source"), manualId.isEmpty() ? QStringLiteral("账号自定义") : QStringLiteral("人工关联，已查询校验")}, {QStringLiteral("reason"), !revision ? revisionError : compatible ? QString() : target != metadata.value(QStringLiteral("model")).toString() ? QStringLiteral("目标型号不匹配") : status == QStringLiteral("DEPLOYING") ? QStringLiteral("审核处理中") : QStringLiteral("审核未通过或状态未知")}, {QStringLiteral("hasPreview"), !detail.value(QStringLiteral("resource_link")).toString().isEmpty()}});
            }
            if (state != QStringLiteral("success") || !manualId.isEmpty() || list.size() < 100) break;
            if (page == 99) { state = QStringLiteral("unsupported"); message = QStringLiteral("目录超过本次安全查询上限，请缩小账号音色范围。"); }
        }
        QJsonObject result{{QStringLiteral("identity"), QString::fromLatin1(identity)}, {QStringLiteral("state"), state}, {QStringLiteral("rows"), rows}, {QStringLiteral("refreshedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        if (state == QStringLiteral("success")) {
            if (rows.isEmpty()) result.insert(QStringLiteral("state"), QStringLiteral("empty"));
            result.insert(QStringLiteral("message"), rows.isEmpty() ? QStringLiteral("当前账号没有自定义音色；可选择系统音色。") : QStringLiteral("账号目录已刷新；只有型号匹配且审核通过的声音可以选用。"));
            if (manualId.isEmpty() && !cancel->load()) { QSaveFile output(cache); const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact); if (bytes.size() > 2 * 1024 * 1024 || !output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) result.insert(QStringLiteral("message"), QStringLiteral("目录已读取，但缓存写入失败；请检查磁盘权限。")); }
        } else { result.insert(QStringLiteral("message"), message); if (!cached.isEmpty()) { result.insert(QStringLiteral("rows"), cached.value(QStringLiteral("rows"))); result.insert(QStringLiteral("state"), QStringLiteral("stale_cache")); result.insert(QStringLiteral("message"), message + QStringLiteral(" 当前显示旧缓存，请刷新后再选用。")); } }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, result, manualId] {
            if (!self || self->epoch_ != epoch) return;
            self->context_->clearCloudTask(self.data()); self->busy_ = false;
            if (cancel->load()) { self->catalog_ = {{QStringLiteral("state"), QStringLiteral("cancelled")}, {QStringLiteral("message"), QStringLiteral("查询已取消，当前声音绑定保留。")}}; }
            else self->catalog_ = result;
            emit self->changed();
            if (!manualId.isEmpty() && !cancel->load() && result.value(QStringLiteral("state")) == QStringLiteral("success")) self->useDiscoveredVoice(manualId);
        }, Qt::QueuedConnection);
    });
}
void VoiceLibraryController::useDiscoveredVoice(const QString &id)
{
    if (busy_ || catalog_.value(QStringLiteral("state")) != QStringLiteral("success")) { status_ = QStringLiteral("请先刷新当前账号目录并确认审核状态。"); emit changed(); return; }
    QString key, error; auto metadata = context_->cloudConfig->resolveSynthesis(&key, &error);
    const auto identity = QCryptographicHash::hash(QJsonDocument(QJsonObject{{QStringLiteral("binding"), metadata}, {QStringLiteral("accounts"), context_->cloudConfig->load().value(QStringLiteral("accounts"))}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    if (metadata.isEmpty() || identity != catalogIdentity_) { status_ = QStringLiteral("账号或型号已改变，请刷新目录；原绑定保留。"); emit changed(); return; }
    for (const auto &value : catalog_.value(QStringLiteral("rows")).toArray()) {
        const auto row = value.toObject(); if (row.value(QStringLiteral("id")).toString() != id || !row.value(QStringLiteral("compatible")).toBool()) continue;
        metadata.remove(QStringLiteral("cloudModelId")); metadata.insert(QStringLiteral("provider"), QStringLiteral("qwen-cloud")); metadata.insert(QStringLiteral("providerVoiceId"), id);
        if (!row.value(QStringLiteral("cloudVoiceRevision")).toString().isEmpty()) metadata.insert(QStringLiteral("cloudVoiceRevision"), row.value(QStringLiteral("cloudVoiceRevision")));
        metadata.insert(QStringLiteral("preset"), QJsonObject{{QStringLiteral("name"), row.value(QStringLiteral("name"))}, {QStringLiteral("language"), QStringLiteral("Chinese")}});
        metadata.insert(QStringLiteral("source"), row.value(QStringLiteral("source"))); storeCloudVoice(metadata, row.value(QStringLiteral("name")).toString()); return;
    }
    status_ = QStringLiteral("该音色未通过当前型号与审核校验。"); emit changed();
}
qint64 VoiceLibraryController::previewCacheBytes() const
{
    return cloudPreviewDirectory_ ? QFileInfo(cloudPreview_.value(QStringLiteral("path")).toString()).size() : 0;
}
void VoiceLibraryController::cancelPreview()
{
    if (!previewLoading_) return;
    previewCancel_ = true; requests_.waitForDone(); ++epoch_; previewLoading_ = false; busy_ = false;
    context_->clearCloudTask(this); cloudPreview_.clear(); cloudPreviewDirectory_.reset();
    status_ = QStringLiteral("试听准备已停止。"); emit changed();
}
void VoiceLibraryController::previewCloudVoice(const QString &id)
{
    if (busy_) return;
    QString officialUrl;
    for (const auto &value : builtinVoices()) if (value.toMap().value(QStringLiteral("id")).toString() == id) officialUrl = value.toMap().value(QStringLiteral("previewUrl")).toString();
    QString key, error; const auto metadata = officialUrl.isEmpty() ? context_->cloudConfig->resolveSynthesis(&key, &error)
        : QJsonObject{{QStringLiteral("provider"), QStringLiteral("official-catalog")}, {QStringLiteral("model"), QStringLiteral("qwen-audio-3.1-tts-flash")}, {QStringLiteral("catalogVersion"), QStringLiteral("2/2026-10-17")}};
    if (metadata.isEmpty()) { status_ = error; emit changed(); return; }
    const auto identity = QCryptographicHash::hash(QJsonDocument(metadata).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    if (cloudPreview_.value(QStringLiteral("id")).toString() == id && cloudPreview_.value(QStringLiteral("identity")).toByteArray() == identity && cloudPreviewDirectory_) { emit auditionRequested(cloudPreview_.value(QStringLiteral("path")).toString(), cloudPreview_.value(QStringLiteral("sha256")).toString()); return; }
    bool available = !officialUrl.isEmpty();
    for (const auto &value : catalog_.value(QStringLiteral("rows")).toArray()) if (value.toObject().value(QStringLiteral("id")).toString() == id && value.toObject().value(QStringLiteral("hasPreview")).toBool()) available = true;
    if (!available) { status_ = QStringLiteral("暂无参考试听资源；可选用后确认费用生成试听。"); emit changed(); return; }
    const QPointer<VoiceLibraryController> self(this);
    if (officialUrl.isEmpty() && !context_->trackCloudTask(this, metadata.value(QStringLiteral("cloudAccountId")).toString(), [self] { if (self) self->cancelAndWait(); })) return;
    busy_ = true; previewLoading_ = true; previewCancel_ = false; const auto epoch = ++epoch_; auto *cancel = &previewCancel_; auto *http = context_->aiHttp; emit changed();
    requests_.start([self, epoch, cancel, http, metadata, key, identity, id, officialUrl] {
        QtNetworkHttpClient native; QString error, hash; auto directory = std::make_shared<QTemporaryDir>();
        const auto path = directory->filePath(QStringLiteral("reference-preview.wav"));
        QUrl url(officialUrl);
        if (officialUrl.isEmpty()) {
            HttpRequest query; query.url = QUrl(QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/services/audio/tts/customization").arg(metadata.value(QStringLiteral("businessSpace")).toString()));
            query.method = QByteArrayLiteral("POST"); query.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key.toUtf8()}, {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
            query.body = QJsonDocument(QJsonObject{{QStringLiteral("model"), QStringLiteral("voice-enrollment")}, {QStringLiteral("input"), QJsonObject{{QStringLiteral("action"), QStringLiteral("query_voice")}, {QStringLiteral("voice_id"), id}}}}).toJson(QJsonDocument::Compact);
            query.maxResponseBytes = 2 * 1024 * 1024; query.followRedirects = false; query.transferTimeoutMs = 15000;
            const auto response = (http ? http : &native)->send(query, cancel);
            QJsonObject detail;
            if (std::holds_alternative<HttpResponse>(response) && std::get<HttpResponse>(response).status == 200) detail = QJsonDocument::fromJson(std::get<HttpResponse>(response).body).object().value(QStringLiteral("output")).toObject();
            url = QUrl(detail.value(QStringLiteral("resource_link")).toString());
            if (detail.value(QStringLiteral("target_model")) != metadata.value(QStringLiteral("model")) || detail.value(QStringLiteral("status")) != QStringLiteral("OK")) error = QStringLiteral("参考试听不可用、审核状态或型号不匹配；请刷新目录。");
        }
        if (url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty()) error = QStringLiteral("试听资源地址无效，请刷新目录。");
        if (error.isEmpty()) {
            HttpRequest download; download.url = url; download.maxDownloadBytes = 32 * 1024 * 1024; download.followRedirects = false; download.transferTimeoutMs = 30000;
            QFile file(path);
            if (!directory->isValid() || !file.open(QIODevice::WriteOnly) || !std::holds_alternative<qint64>((http ? http : &native)->download(download, &file, 0, cancel))) error = QStringLiteral("参考试听下载失败，请重试。");
            file.close();
            if (error.isEmpty()) { const auto quality = AudioQuality::inspect(path, &error, cancel); if (quality) hash = QString::fromLatin1(quality->sha256.toHex()); }
        }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, directory, path, hash, error, identity, id, officialUrl] {
            if (!self || self->epoch_ != epoch) return;
            self->context_->clearCloudTask(self.data()); self->busy_ = false; self->previewLoading_ = false;
            if (cancel->load() || !error.isEmpty() || hash.isEmpty()) { self->status_ = cancel->load() ? QStringLiteral("试听已取消。") : error; emit self->changed(); return; }
            // 先切换播放器，再释放旧参考文件；只保留一个有 32MiB 下载上限的试听缓存。
            emit self->auditionRequested(path, hash); self->cloudPreviewDirectory_ = directory;
            self->cloudPreview_ = {{QStringLiteral("id"), id}, {QStringLiteral("identity"), identity}, {QStringLiteral("path"), path}, {QStringLiteral("sha256"), hash}};
            self->status_ = officialUrl.isEmpty() ? QStringLiteral("正在播放参考样本，非 TTS 合成效果；再次播放复用缓存。") : QStringLiteral("正在播放官方现成试听样本，未调用 TTS；再次播放复用缓存。"); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void VoiceLibraryController::savePreset(const QString &versionId, const QString &name, const QString &usageScope)
{
    if (name.trimmed().isEmpty() || name.size() > 128 || usageScope.size() > 4096) {
        status_ = QStringLiteral("请填写预设名称（最多 128 字），使用范围最多 4096 字。"); emit changed(); return;
    }
    auto *cancel = &cancel_;
    submit([versionId, name, usageScope, cancel](VoiceLibrary &library, QString &selected, QString &error) {
        const auto version = library.version(versionId, &error);
        if (!version || version->value(QStringLiteral("voiceId")).toString() != selected) { error = QStringLiteral("请选择当前档案的声音版本。"); return false; }
        auto metadata = version->value(QStringLiteral("metadata")).toObject();
        metadata.insert(QStringLiteral("preset"), QJsonObject{{QStringLiteral("name"), name.trimmed()}, {QStringLiteral("language"), QStringLiteral("Chinese")}});
        metadata.insert(QStringLiteral("usageScope"), usageScope.trimmed()); metadata.insert(QStringLiteral("sourceVersionId"), versionId);
        return library.addVersion(selected, metadata, version->value(QStringLiteral("referencePath")).toString(), &error, cancel).has_value();
    }, QStringLiteral("中文声音预设已保存为新版本；已用项目和默认版本不变。"));
}
void VoiceLibraryController::copyToProject(const QString &versionId)
{
    if (!session_->unified() || session_->storagePath().isEmpty() || session_->assetMutationBlocked()) {
        status_ = QStringLiteral("请等待当前存储操作完成；旧格式工程需先转换为统一副本。"); emit changed(); return;
    }
    const QString path = session_->storagePath(); auto *cancel = &cancel_;
    submit([path, versionId, cancel](VoiceLibrary &library, QString &, QString &error) {
        ProjectStore project(path); return library.copyVersionToProject(versionId, project, &error, cancel);
    }, QStringLiteral("声音版本已复制到项目；文稿使用的声音未自动改变。"));
}
void VoiceLibraryController::setDefaultVersion(const QString &versionId)
{
    submit([versionId](VoiceLibrary &library, QString &selected, QString &error) {
        return library.setDefaultVersion(selected, versionId, &error);
    }, QStringLiteral("默认版本已更新，已有项目声音保持不变。"));
}
void VoiceLibraryController::inspectVersion(const QString &versionId)
{
    if (busy_) return;
    busy_ = true; cancel_ = false; review_.clear(); derived_.clear(); status_ = QStringLiteral("正在检查参考样本…"); emit changed();
    const QPointer<VoiceLibraryController> self(this); const auto epoch = ++epoch_;
    const QString path = path_; auto *cancel = &cancel_;
    task_ = context_->submitProjectStorage([self, epoch, path, cancel, versionId] {
        QString error;
        std::optional<QJsonObject> version;
        try { if (!cancel->load()) version = VoiceLibrary(path).version(versionId, &error); }
        catch (...) { error = QStringLiteral("声音版本读取失败，请检查磁盘和权限。"); }
        if (error.isEmpty() && (!version || version->value(QStringLiteral("referencePath")).toString().isEmpty())) error = QStringLiteral("该版本没有可检查的本地参考。");
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, version, error] {
            if (!self || self->epoch_ != epoch) return;
            if (cancel->load()) { self->busy_ = false; self->status_ = QStringLiteral("参考检查已取消。"); emit self->changed(); emit self->operationFinished(false); return; }
            if (!error.isEmpty()) { self->busy_ = false; self->status_ = error; emit self->changed(); emit self->operationFinished(false); return; }
            self->status_ = QStringLiteral("正在解码参考音频…"); emit self->changed();
            self->audioTask_ = self->context_->submitAudioProcessing([self, epoch, cancel, version] {
                QString error;
                std::optional<AudioQuality> quality;
                try { quality = AudioQuality::inspect(version->value(QStringLiteral("referencePath")).toString(), &error, cancel); }
                catch (...) { error = QStringLiteral("参考检查发生异常，请检查音频和可用内存。"); }
                if (quality && QString::fromLatin1(quality->sha256.toHex()) != version->value(QStringLiteral("referenceSha256")).toString()) error = QStringLiteral("参考文件与声音版本指纹不符，请恢复原件。");
                if (quality && quality->peak == 0) error = QStringLiteral("参考样本全为静音，不能用于克隆。");
                QJsonObject review;
                if (quality && error.isEmpty()) { review = *version; review.insert(QStringLiteral("quality"), quality->referenceReview()); }
                if (!self) return;
                QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, review, error] {
                    if (!self || self->epoch_ != epoch) return;
                    self->busy_ = false; self->review_ = cancel->load() ? QVariantMap{} : review.toVariantMap();
                    self->status_ = cancel->load() ? QStringLiteral("参考检查已取消。")
                        : (error.isEmpty() ? QStringLiteral("检查完成，请试听并审核转写；确认后创建新的可用版本。") : error);
                    emit self->changed(); emit self->operationFinished(error.isEmpty() && !cancel->load());
                }, Qt::QueuedConnection);
            });
        }, Qt::QueuedConnection);
    });
}
void VoiceLibraryController::discardEmotionPrediction()
{
    if (busy_ || review_.isEmpty()) return;
    auto metadata = review_.value(QStringLiteral("metadata")).toMap();
    metadata.remove(QStringLiteral("emotionPrediction")); review_.insert(QStringLiteral("metadata"), metadata);
    status_ = QStringLiteral("当前审核已忽略模型建议，人工标签和历史版本保留。"); emit changed();
}

void VoiceLibraryController::recognizeEmotion(const QString &python, const QString &modelDirectory, bool sampleBatch)
{
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->commit();
    if (busy_ || (sampleBatch ? samples_.isEmpty() : review_.isEmpty())) return;
    if (sampleBatch && samplesProfile_ != selected_) {
        status_ = QStringLiteral("请重新选择整理时的音色，避免样本串用。"); emit changed(); return;
    }
    if (!QFileInfo(python).isFile() || !QFileInfo(modelDirectory).isDir()) {
        status_ = QStringLiteral("请提供独立情绪识别 Python 和本地模型目录；不会自动安装或下载。"); emit changed(); return;
    }
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("inference/audio_emotion_worker.py"));
    const auto worker = QFileInfo::exists(bundled) ? bundled : QDir(QString::fromUtf8(SUBCUE_PROJECT_ROOT)).filePath(QStringLiteral("tools/audio_emotion_worker.py"));
    QJsonArray jobs;
    for (const auto &value : sampleBatch ? samples_ : QVariantList{review_}) {
        const auto sample = value.toMap();
        if (sampleBatch && (!sample.value(QStringLiteral("selected")).toBool() || sample.contains(QStringLiteral("error")) || sample.contains(QStringLiteral("savedVersionId")))) continue;
        jobs.append(QJsonObject{{QStringLiteral("id"), sample.value(sampleBatch ? QStringLiteral("sampleId") : QStringLiteral("id")).toString()},
            {QStringLiteral("path"), sample.value(QStringLiteral("referencePath")).toString()}, {QStringLiteral("sha256"), sample.value(QStringLiteral("referenceSha256")).toString()}});
    }
    if (jobs.isEmpty() || jobs.size() > 100) {
        status_ = QStringLiteral("请勾选 1–100 个尚未保存的有效样本；更多样本请分批识别。"); emit changed(); return;
    }
    const auto input = QJsonDocument(QJsonObject{{QStringLiteral("modelDirectory"), modelDirectory}, {QStringLiteral("jobs"), jobs}}).toJson(QJsonDocument::Compact) + '\n';
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; auto *cancel = &cancel_;
    const QPointer<VoiceLibraryController> self(this);
    status_ = QStringLiteral("情绪识别已排队，共用 GPU；预测不会覆盖人工标签。"); emit changed();
    const auto directory = sampleDirectory_;
    audioTask_ = context_->submitAudioProcessing([self, epoch, cancel, python, worker, input, jobs, sampleBatch, directory] {
        QString error; QJsonArray predictions;
        try {
            const auto process = InferenceManager::instance().run(python, {QStringLiteral("-B"), worker}, input, cancel, 180000, {}, QStringLiteral("L3 本地音频情绪识别"));
            const auto report = QJsonDocument::fromJson(process.standardOutput).object();
            predictions = report.value(QStringLiteral("results")).toArray();
            if (!process.started || process.exitCode != 0 || process.cancelled || process.timedOut || process.outputTruncated || predictions.size() != jobs.size())
                error = QStringLiteral("音频情绪识别失败或结果无效：%1").arg(QString::fromUtf8(process.standardError).right(12000));
            // 整批验证后才回写，不能将错序或其它资产的预测绑定到当前样本。
            for (int i = 0; error.isEmpty() && i < jobs.size(); ++i) {
                const auto prediction = predictions[i].toObject();
                if (!VoiceLibrary::validEmotionPrediction(prediction) || prediction.value(QStringLiteral("reviewed")).toBool()
                    || prediction.value(QStringLiteral("id")) != jobs[i].toObject().value(QStringLiteral("id"))
                    || prediction.value(QStringLiteral("assetSha256")) != jobs[i].toObject().value(QStringLiteral("sha256")))
                    error = QStringLiteral("情绪预测与本次样本不匹配，请重新识别。");
            }
        } catch (...) { error = QStringLiteral("音频情绪识别异常，请检查独立环境、模型和可用内存。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, predictions, error, sampleBatch] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false;
            if (!cancel->load() && error.isEmpty()) {
                for (const auto &value : predictions) {
                    const auto prediction = value.toObject();
                    if (sampleBatch) {
                        for (int i = 0; i < self->samples_.size(); ++i) {
                            auto sample = self->samples_[i].toMap();
                            if (sample.value(QStringLiteral("sampleId")).toString() != prediction.value(QStringLiteral("id")).toString()) continue;
                            sample.insert(QStringLiteral("emotionPrediction"), prediction.toVariantMap()); self->samples_[i] = sample; break;
                        }
                    } else {
                        auto metadata = self->review_.value(QStringLiteral("metadata")).toMap();
                        metadata.insert(QStringLiteral("emotionPrediction"), prediction.toVariantMap()); self->review_.insert(QStringLiteral("metadata"), metadata);
                    }
                }
            }
            self->status_ = cancel->load() ? QStringLiteral("情绪识别已取消，人工标签保留。")
                : error.isEmpty() ? QStringLiteral("识别完成，分数未经准确率校准；请核对建议。保存审核版本后落盘，人工标签未改变。") : error;
            emit self->changed(); emit self->operationFinished(!cancel->load() && error.isEmpty());
        }, Qt::QueuedConnection);
    });
}

void VoiceLibraryController::approveReview(const QString &transcript, bool acceptWarnings, const QString &referenceLanguage, const QString &manualEmotion, bool emotionReviewed, bool transcriptRequired)
{
    if (review_.isEmpty() || (transcriptRequired && transcript.trimmed().isEmpty()) || review_.value(QStringLiteral("voiceId")).toString() != selected_) {
        status_ = QStringLiteral("请先选择该音色、完成参考检查并填写审核转写。"); emit changed(); return;
    }
    const auto quality = review_.value(QStringLiteral("quality")).toMap();
    if (!quality.value(QStringLiteral("warnings")).toList().isEmpty() && !acceptWarnings) {
        status_ = QStringLiteral("该样本存在质量疑点，请试听并明确确认后使用。"); emit changed(); return;
    }
    auto metadata = QJsonObject::fromVariantMap(review_.value(QStringLiteral("metadata")).toMap());
    if (metadata.contains(QStringLiteral("emotionPrediction"))) {
        auto prediction = metadata.value(QStringLiteral("emotionPrediction")).toObject();
        if (!VoiceLibrary::validEmotionPrediction(prediction)) { status_ = QStringLiteral("情绪预测无效，请重新识别。"); emit changed(); return; }
        if (!emotionReviewed) {
            status_ = prediction.value(QStringLiteral("requiresReview")).toBool()
                ? QStringLiteral("情绪分数低、类别不明确或接近，请试听并确认情绪审核；人工标签不会自动改变。")
                : QStringLiteral("模型分数尚未经准确率校准，请试听并确认情绪审核，或忽略当前建议继续人工建档。"); emit changed(); return;
        }
        prediction.insert(QStringLiteral("reviewed"), emotionReviewed); metadata.insert(QStringLiteral("emotionPrediction"), prediction);
    }
    if (!manualEmotion.isEmpty()) {
        if (!VoiceLibrary::validEmotionLabel(manualEmotion)) { status_ = QStringLiteral("请选择有效的人工情绪标签。"); emit changed(); return; }
        metadata.insert(QStringLiteral("manualEmotion"), manualEmotion);
    }
    const auto language = referenceLanguage.isEmpty() ? metadata.value(QStringLiteral("referenceLanguage")).toString(QStringLiteral("Chinese")) : referenceLanguage;
    if (!QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}.contains(language)) { status_ = QStringLiteral("参考语言需明确选择中文、英语或日语。"); emit changed(); return; }
    metadata.insert(QStringLiteral("referenceLanguage"), language);
    metadata.insert(QStringLiteral("referenceText"), transcript.trimmed());
    metadata.insert(QStringLiteral("qualityState"), QStringLiteral("approved"));
    metadata.insert(QStringLiteral("quality"), QJsonObject::fromVariantMap(quality));
    metadata.insert(QStringLiteral("sourceVersionId"), review_.value(QStringLiteral("id")).toString());
    metadata.insert(QStringLiteral("qualityWarningsAccepted"), acceptWarnings);
    const QString reference = review_.value(QStringLiteral("referencePath")).toString(); auto *cancel = &cancel_;
    submit([metadata, reference, cancel](VoiceLibrary &library, QString &selected, QString &error) {
        return library.addVersion(selected, metadata, reference, &error, cancel).has_value();
    }, QStringLiteral("审核后的可用版本已保存，旧版本保留。"));
}

void VoiceLibraryController::prepareSamples(const QList<QUrl> &files, bool split)
{
    if (busy_ || (!creating_ && selected_.isEmpty())) return;
    QStringList paths;
    for (const auto &file : files) {
        if (!file.isLocalFile()) { status_ = QStringLiteral("仅支持用户选择的本地素材。"); emit changed(); return; }
        paths.append(file.toLocalFile());
    }
    const auto previous = creating_ ? samples_ : QVariantList{};
    auto directory = creating_ && sampleDirectory_ ? sampleDirectory_ : std::make_shared<QTemporaryDir>();
    if (!directory->isValid()) { status_ = QStringLiteral("无法创建样本整理目录。"); emit changed(); return; }
    sampleDirectory_ = directory; samplesProfile_ = selected_; if (!creating_) samples_.clear(); busy_ = true; cancel_ = false;
    status_ = QStringLiteral("正在切分同一说话人的素材；不会识别或分离说话人。"); emit changed();
    const QPointer<VoiceLibraryController> self(this); const auto epoch = ++epoch_; auto *cancel = &cancel_;
    audioTask_ = context_->submitAudioProcessing([self, epoch, paths, directory, cancel, split, previous] {
        QString error; QJsonArray samples;
        try { samples = ReferenceSamples::prepare(paths, directory->path(), &error, cancel, [self, epoch](int current, int total) {
            if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, current, total] {
                if (!self || self->epoch_ != epoch) return;
                self->status_ = QStringLiteral("样本整理 %1／%2").arg(current).arg(total); emit self->changed();
            }, Qt::QueuedConnection);
        }, split); } catch (...) { error = QStringLiteral("样本整理发生异常，请检查文件和可用内存。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, samples, error, cancel, directory, previous] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false;
            self->samples_ = previous;
            if (!cancel->load() && error.isEmpty()) {
                for (const auto &value : samples.toVariantList()) {
                    const auto row = value.toMap(); bool duplicate = false;
                    for (const auto &existing : self->samples_) if (!row.value(QStringLiteral("referenceSha256")).toString().isEmpty()
                        && existing.toMap().value(QStringLiteral("referenceSha256")) == row.value(QStringLiteral("referenceSha256"))) { duplicate = true; break; }
                    if (!duplicate) self->samples_.append(value);
                }
            }
            QSet<QString> retained;
            qint64 bytes = 0;
            for (const auto &value : self->samples_) { const auto path = value.toMap().value(QStringLiteral("referencePath")).toString(); retained.insert(path); bytes += QFileInfo(path).size(); }
            const bool overflow = bytes > 512LL * 1024 * 1024 || self->samples_.size() > 2000;
            if (overflow) {
                self->samples_ = previous; retained.clear();
                for (const auto &value : previous) retained.insert(value.toMap().value(QStringLiteral("referencePath")).toString());
                self->status_ = QStringLiteral("参考草稿超过 512MiB 或 2000 段，请减少素材后重试。");
            }
            // 目录只包含本次草稿派生文件，保留正在使用的参考；不触碰用户原件。
            for (const auto &file : QDir(directory->path()).entryInfoList(QDir::Files)) if (!retained.contains(file.absoluteFilePath())) QFile::remove(file.absoluteFilePath());
            if (self->samples_.isEmpty() && (cancel->load() || !error.isEmpty())) self->sampleDirectory_.reset();
            self->status_ = overflow ? QStringLiteral("参考草稿超过 512MiB 或 2000 段，请减少素材后重试。") : cancel->load() ? QStringLiteral("样本整理已取消。") : error.isEmpty()
                ? (self->creating_ ? QStringLiteral("素材已添加；逐项试听、核对转写并确认后创建音色。重复参考已跳过。") : QStringLiteral("整理完成；填写或识别转写，保存后逐个审核。重复内容已跳过，短片段和质量疑点需人工复核。")) : error;
            emit self->changed(); emit self->operationFinished(error.isEmpty() && !cancel->load() && !overflow);
        }, Qt::QueuedConnection);
    });
}

void VoiceLibraryController::editSample(int index, const QString &transcript, bool selected)
{
    if (busy_ || index < 0 || index >= samples_.size() || transcript.size() > 65536) return;
    auto sample = samples_[index].toMap(); if (sample.value(QStringLiteral("transcript")).toString() != transcript) sample.insert(QStringLiteral("confirmed"), false);
    sample.insert(QStringLiteral("transcript"), transcript); sample.insert(QStringLiteral("selected"), selected);
    samples_[index] = sample; // 不重置列表和输入法草稿；提交或开始识别前由界面显式提交。
}

void VoiceLibraryController::transcribeSamples()
{
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->commit();
    if (busy_ || samples_.isEmpty()) return;
    const auto settings = context_->settings;
    const auto provider = AsrProviderFactory::providerIdFromSettings(settings);
    if (provider != QLatin1String("qwen3") && provider != QLatin1String("funasr")) {
        status_ = QStringLiteral("请在现有 ASR 设置中选择可用的本地 Qwen／FunASR；样本不会发送云端，可手工填写转写。"); emit changed(); return;
    }
    busy_ = true; cancel_ = false; status_ = QStringLiteral("等待 GPU 批次，正在进行本地中文转写…"); emit changed();
    const QPointer<VoiceLibraryController> self(this); const auto epoch = ++epoch_; auto *cancel = &cancel_;
    const auto samples = samples_; const auto directory = sampleDirectory_;
    audioTask_ = context_->submitAudioProcessing([self, epoch, cancel, settings, samples, directory] {
        QVariantList updated = samples; QString error; QSet<QString> texts, processed;
        try {
            auto lease = InferenceManager::instance().acquire(cancel, -1, QStringLiteral("声音样本中文转写批次"));
            if (lease) {
                auto service = AsrProviderFactory(nullptr).create(settings, {});
                for (const auto &value : samples) {
                    if (cancel->load()) break;
                    const auto sample = value.toMap(); const auto source = sample.value(QStringLiteral("sourcePath")).toString();
                    if (!sample.value(QStringLiteral("selected")).toBool() || sample.contains(QStringLiteral("error"))
                        || processed.contains(source) || !sample.value(QStringLiteral("transcript")).toString().trimmed().isEmpty()) continue;
                    processed.insert(source);
                    if (QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(source, &error, cancel).toHex()) != sample.value(QStringLiteral("sourceSha256")).toString()) {
                        error = QStringLiteral("原件在整理后发生变化，请重新整理。"); break;
                    }
                    const auto result = service->transcribe({source, cancel, {}, true});
                    if (const auto *failure = std::get_if<AppError>(&result)) { error = failure->userMessage(); break; }
                    if (QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(source, &error, cancel).toHex()) != sample.value(QStringLiteral("sourceSha256")).toString()) {
                        error = QStringLiteral("原件在转写期间发生变化，请重新整理。"); break;
                    }
                    for (int i = 0; i < updated.size(); ++i) {
                        auto item = updated[i].toMap();
                        if (item.value(QStringLiteral("sourcePath")).toString() != source || !item.value(QStringLiteral("selected")).toBool()
                            || !item.value(QStringLiteral("transcript")).toString().trimmed().isEmpty()) continue;
                        QString text;
                        const auto start = qRound64(item.value(QStringLiteral("startSeconds")).toDouble() * 1000);
                        const auto end = qRound64(item.value(QStringLiteral("endSeconds")).toDouble() * 1000);
                        bool boundaryCrossed = false;
                        for (const auto &word : std::get<Transcript>(result).words) {
                            if (word.endMs <= start || word.startMs >= end) continue;
                            if (!word.preciseTiming || word.startMs < start || word.endMs > end) boundaryCrossed = true;
                            else text += word.text;
                        }
                        if (boundaryCrossed) {
                            // 粗时间范围或横跨切口时重新识别该短音频，不能把整句猜配给某一段。
                            const auto clipped = service->transcribe({item.value(QStringLiteral("referencePath")).toString(), cancel, {}, true});
                            if (const auto *failure = std::get_if<AppError>(&clipped)) { error = failure->userMessage(); break; }
                            text.clear(); for (const auto &word : std::get<Transcript>(clipped).words) text += word.text;
                        }
                        item.insert(QStringLiteral("confirmed"), false); item.insert(QStringLiteral("transcript"), text.trimmed()); item.insert(QStringLiteral("transcriptOrigin"), QStringLiteral("local-asr")); updated[i] = item;
                    }
                    if (!error.isEmpty()) break;
                }
                for (int i = 0; i < updated.size(); ++i) {
                    auto item = updated[i].toMap(); const auto text = item.value(QStringLiteral("transcript")).toString().simplified();
                    item.insert(QStringLiteral("duplicateText"), !text.isEmpty() && texts.contains(text));
                    if (!text.isEmpty()) texts.insert(text); updated[i] = item;
                }
            }
        } catch (...) { error = QStringLiteral("本地转写失败；可保留已完成结果并手工填写。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, updated, error, cancel] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false; self->samples_ = updated;
            self->status_ = cancel->load() ? QStringLiteral("转写已取消，已完成转写保留。") : error.isEmpty()
                ? QStringLiteral("转写完成；重复文本仅提示，请试听判断，不自动删除不同表演。") : error;
            emit self->changed(); emit self->operationFinished(error.isEmpty() && !cancel->load());
        }, Qt::QueuedConnection);
    });
}

void VoiceLibraryController::saveSamples(bool transcriptRequired)
{
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->commit();
    if (busy_ || samples_.isEmpty()) return;
    if (samplesProfile_ != selected_) { status_ = QStringLiteral("请重新选择整理时的音色，避免样本串用。"); emit changed(); return; }
    const auto samples = samples_; const auto directory = sampleDirectory_; auto *cancel = &cancel_;
    for (const auto &value : samples) {
        const auto sample = value.toMap();
        if (sample.value(QStringLiteral("selected")).toBool() && !sample.contains(QStringLiteral("error"))
            && transcriptRequired && sample.value(QStringLiteral("transcript")).toString().trimmed().isEmpty()) {
            status_ = QStringLiteral("请填写所有勾选样本的转写，或取消勾选。保存后仍需逐个试听审核。"); emit changed(); return;
        }
    }
    const QPointer<VoiceLibraryController> self(this); const auto epoch = epoch_ + 1;
    submit([samples, directory, cancel, self, epoch](VoiceLibrary &library, QString &selected, QString &error) {
        QHash<QString, QString> originals; QSet<QString> savedSamples;
        for (int offset = 0;; offset += 50) {
            const auto versions = library.versions(selected, offset, 50, &error);
            if (!error.isEmpty()) return false;
            for (const auto &value : versions) {
                const auto version = library.version(value.toObject().value(QStringLiteral("id")).toString(), &error);
                if (!version) return false;
                const auto metadata = version->value(QStringLiteral("metadata")).toObject();
                if (metadata.contains(QStringLiteral("sampleId"))) savedSamples.insert(metadata.value(QStringLiteral("sampleId")).toString());
                if (metadata.value(QStringLiteral("sampleKind")).toString() == QLatin1String("original"))
                    originals.insert(version->value(QStringLiteral("referenceSha256")).toString(), version->value(QStringLiteral("id")).toString());
            }
            if (versions.size() < 50) break;
        }
        for (int i = 0; i < samples.size(); ++i) {
            const auto &value = samples[i];
            const auto sample = value.toMap();
            if (!sample.value(QStringLiteral("selected")).toBool() || sample.contains(QStringLiteral("error")) || sample.contains(QStringLiteral("savedVersionId"))) continue;
            if (savedSamples.contains(sample.value(QStringLiteral("sampleId")).toString())) continue;
            if (cancel->load()) return false;
            const auto hash = sample.value(QStringLiteral("sourceSha256")).toString();
            if (!originals.contains(hash)) {
                if (QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(sample.value(QStringLiteral("sourcePath")).toString(), &error, cancel).toHex()) != hash) {
                    error = QStringLiteral("原件在整理后发生变化，请重新整理。"); return false;
                }
                const auto original = library.addVersion(selected, {{QStringLiteral("sampleKind"), QStringLiteral("original")}, {QStringLiteral("originalSha256"), hash},
                    {QStringLiteral("sourceName"), QFileInfo(sample.value(QStringLiteral("sourcePath")).toString()).fileName()}, {QStringLiteral("qualityState"), QStringLiteral("pending")}},
                    sample.value(QStringLiteral("sourcePath")).toString(), &error, cancel);
                if (!original) return false; originals.insert(hash, *original);
            }
            QJsonObject metadata{{QStringLiteral("sampleKind"), QStringLiteral("segment")},
                {QStringLiteral("sampleId"), sample.value(QStringLiteral("sampleId")).toString()},
                {QStringLiteral("sourceVersionId"), originals.value(hash)}, {QStringLiteral("sourceSha256"), hash},
                {QStringLiteral("startSeconds"), sample.value(QStringLiteral("startSeconds")).toDouble()},
                {QStringLiteral("endSeconds"), sample.value(QStringLiteral("endSeconds")).toDouble()},
                {QStringLiteral("sourceSampleRate"), sample.value(QStringLiteral("sourceSampleRate")).toInt()},
                {QStringLiteral("sourceStartSample"), sample.value(QStringLiteral("sourceStartSample")).toString()},
                {QStringLiteral("sourceEndSample"), sample.value(QStringLiteral("sourceEndSample")).toString()},
                {QStringLiteral("referenceText"), sample.value(QStringLiteral("transcript")).toString().trimmed()},
                {QStringLiteral("qualityState"), QStringLiteral("pending")}, {QStringLiteral("quality"), QJsonObject::fromVariantMap(sample.value(QStringLiteral("quality")).toMap())}};
            if (sample.contains(QStringLiteral("emotionPrediction"))) metadata.insert(QStringLiteral("emotionPrediction"), QJsonObject::fromVariantMap(sample.value(QStringLiteral("emotionPrediction")).toMap()));
            const auto version = library.addVersion(selected, metadata, sample.value(QStringLiteral("referencePath")).toString(), &error, cancel);
            if (!version) return false;
            if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, i, id = *version] {
                if (!self || self->epoch_ != epoch || i >= self->samples_.size()) return;
                auto saved = self->samples_[i].toMap(); saved.insert(QStringLiteral("savedVersionId"), id); self->samples_[i] = saved; emit self->changed();
            }, Qt::QueuedConnection);
        }
        return true;
    }, QStringLiteral("选中样本和原件已保存为独立版本；请逐个检查审核后设为默认。"));
}

void VoiceLibraryController::deriveReview(double startSeconds, double endSeconds, bool removeDc, bool reduceNoise)
{
    if (busy_ || review_.isEmpty()) return;
    const auto quality = review_.value(QStringLiteral("quality")).toMap();
    const double duration = quality.value(QStringLiteral("sampleCount")).toString().toDouble() / quality.value(QStringLiteral("sampleRate")).toInt();
    if (endSeconds > duration + 0.00001) { status_ = QStringLiteral("裁切结束时间超出原样本，未静默截断。"); emit changed(); return; }
    auto directory = std::make_shared<QTemporaryDir>();
    if (!directory->isValid()) { status_ = QStringLiteral("无法创建派生音频目录。"); emit changed(); return; }
    busy_ = true; cancel_ = false; derived_.clear(); status_ = QStringLiteral("正在准备派生样本，请 A/B 试听并重新审核转写。"); emit changed();
    const auto review = review_; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceLibraryController> self(this);
    audioTask_ = context_->submitAudioProcessing([self, epoch, cancel, review, directory, startSeconds, endSeconds, removeDc, reduceNoise] {
        QString error; QVariantMap derived;
        const auto path = directory->filePath(QStringLiteral("derived.wav"));
        try {
            const auto source = review.value(QStringLiteral("referencePath")).toString();
            if (QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(source, &error, cancel).toHex()) != review.value(QStringLiteral("referenceSha256")).toString()) error = QStringLiteral("原样本指纹不符，请恢复原件。");
            if (error.isEmpty() && ReferenceSamples::derive(source, path, startSeconds, endSeconds, removeDc, reduceNoise, &error, cancel)) {
                if (QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(source, &error, cancel).toHex()) != review.value(QStringLiteral("referenceSha256")).toString()) error = QStringLiteral("原样本在派生期间发生变化，请重新检查。");
                const auto quality = AudioQuality::inspect(path, &error, cancel);
                if (quality && quality->peak != 0 && error.isEmpty()) derived = {{QStringLiteral("referencePath"), path}, {QStringLiteral("referenceSha256"), QString::fromLatin1(quality->sha256.toHex())},
                    {QStringLiteral("quality"), quality->referenceReview().toVariantMap()}, {QStringLiteral("startSeconds"), startSeconds}, {QStringLiteral("endSeconds"), endSeconds},
                    {QStringLiteral("removeDc"), removeDc}, {QStringLiteral("reduceNoise"), reduceNoise}, {QStringLiteral("sourceVersionId"), review.value(QStringLiteral("id"))}};
                else if (error.isEmpty()) error = QStringLiteral("派生样本全为静音，不能使用。");
            }
        } catch (...) { error = QStringLiteral("派生样本处理失败，请检查音频和磁盘。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, directory, derived, error] {
            if (!self || self->epoch_ != epoch) return;
            self->busy_ = false;
            if (!cancel->load() && error.isEmpty()) { self->derived_ = derived; self->derivedDirectory_ = directory; }
            self->status_ = cancel->load() ? QStringLiteral("派生处理已取消。") : error.isEmpty() ? QStringLiteral("派生样本已准备，请 A/B 试听；裁切后的转写必须重新复核。") : error;
            emit self->changed(); emit self->operationFinished(error.isEmpty() && !cancel->load());
        }, Qt::QueuedConnection);
    });
}

void VoiceLibraryController::approveDerived(const QString &transcript, bool acceptWarnings, bool transcriptReviewed, const QString &referenceLanguage, const QString &manualEmotion, bool transcriptRequired)
{
    if (!transcriptReviewed) { status_ = QStringLiteral("请确认已 A/B 试听并重新核对裁切后的转写。"); emit changed(); return; }
    if (derived_.isEmpty() || (transcriptRequired && transcript.trimmed().isEmpty()) || review_.value(QStringLiteral("voiceId")).toString() != selected_) return;
    const auto quality = derived_.value(QStringLiteral("quality")).toMap();
    if (!quality.value(QStringLiteral("warnings")).toList().isEmpty() && !acceptWarnings) {
        status_ = QStringLiteral("请试听并确认派生样本质量疑点。裁切后需重新审核转写。"); emit changed(); return;
    }
    auto metadata = QJsonObject::fromVariantMap(review_.value(QStringLiteral("metadata")).toMap());
    // 派生音频须重新标注，不能继承原音频的预测或人工结论。
    metadata.remove(QStringLiteral("emotionPrediction"));
    metadata.remove(QStringLiteral("manualEmotion"));
    if (!manualEmotion.isEmpty()) {
        if (!VoiceLibrary::validEmotionLabel(manualEmotion)) { status_ = QStringLiteral("请选择有效的人工情绪标签。"); emit changed(); return; }
        metadata.insert(QStringLiteral("manualEmotion"), manualEmotion);
    }
    const auto language = referenceLanguage.isEmpty() ? metadata.value(QStringLiteral("referenceLanguage")).toString(QStringLiteral("Chinese")) : referenceLanguage;
    if (!QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}.contains(language)) { status_ = QStringLiteral("参考语言需明确选择中文、英语或日语。"); emit changed(); return; }
    metadata.insert(QStringLiteral("referenceLanguage"), language);
    metadata.insert(QStringLiteral("referenceText"), transcript.trimmed()); metadata.insert(QStringLiteral("qualityState"), QStringLiteral("approved"));
    metadata.insert(QStringLiteral("quality"), QJsonObject::fromVariantMap(quality)); metadata.insert(QStringLiteral("qualityWarningsAccepted"), acceptWarnings);
    auto process = QJsonObject::fromVariantMap(derived_); process.remove(QStringLiteral("referencePath")); process.remove(QStringLiteral("quality"));
    metadata.insert(QStringLiteral("processing"), process); metadata.insert(QStringLiteral("sourceVersionId"), derived_.value(QStringLiteral("sourceVersionId")).toString());
    const auto path = derived_.value(QStringLiteral("referencePath")).toString(); const auto directory = derivedDirectory_; auto *cancel = &cancel_;
    submit([metadata, path, directory, cancel](VoiceLibrary &library, QString &selected, QString &error) {
        return library.addVersion(selected, metadata, path, &error, cancel).has_value();
    }, QStringLiteral("派生审核版本已保存，原件和旧版本保持不变。"));
}
}
