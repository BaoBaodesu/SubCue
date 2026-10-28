#include "voice_audio_controller.h"
#include "voice_studio_controller.h"
#include "audio_preview_controller.h"
#include "project_session.h"
#include "application_context.h"
#include "tts/voice_audio.h"
#include "tts/audio_quality.h"
#include "common/file_path_guard.h"
#include "project/project_store.h"
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QElapsedTimer>
#include <cmath>
namespace subcue {
namespace {
constexpr qint64 diskLimit = 1024LL * 1024 * 1024;
QByteArray editingKey(const QJsonObject &document, const QString &segment)
{
    QJsonArray ordered;
    for (const auto &value : document.value(QStringLiteral("segments")).toArray()) {
        const auto item = value.toObject(); if (!segment.isEmpty() && item.value(QStringLiteral("id")).toString() != segment) continue;
        auto take = item.value(QStringLiteral("selectedTake")).toObject(); take.remove(QStringLiteral("inputVersion")); take.remove(QStringLiteral("inputKey"));
        ordered.append(QJsonObject{{QStringLiteral("id"), item.value(QStringLiteral("id"))}, {QStringLiteral("take"), take}, {QStringLiteral("gain"), item.value(QStringLiteral("gain"))}, {QStringLiteral("pause"), segment.isEmpty() ? item.value(QStringLiteral("pauseMs")) : QJsonValue(0)}});
    }
    return QCryptographicHash::hash(QJsonDocument(ordered).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
qint64 cacheSize(const QDir &directory)
{
    qint64 size = 0; for (const auto &file : directory.entryInfoList({QStringLiteral("*.wav"), QStringLiteral("*.json")}, QDir::Files)) size += file.size(); return size;
}
bool makeRoom(const QDir &directory, const QString &pinned, qint64 needed, QString *error)
{
    if (needed > diskLimit) { *error = QStringLiteral("连续试听超过 1GiB 缓存上限，请试听单句或分批处理。"); return false; }
    auto size = cacheSize(directory);
    for (const auto &file : directory.entryInfoList({QStringLiteral("*.wav")}, QDir::Files, QDir::Time | QDir::Reversed)) {
        if (size + needed <= diskLimit) break;
        // 仅回收本缓存目录内由内容键命名的派生文件；活动试听被固定保护。
        if (file.isSymLink() || file.completeBaseName().size() != 64 || QByteArray::fromHex(file.completeBaseName().toLatin1()).size() != 32 || sameFilePath(file.absoluteFilePath(), pinned)) continue;
        QFile metadata(directory.filePath(file.fileName() + QStringLiteral(".json"))); const auto metadataSize = metadata.size();
        if (QFile::remove(file.absoluteFilePath())) { size -= file.size(); if (metadata.remove()) size -= metadataSize; }
    }
    if (size + needed > diskLimit) { *error = QStringLiteral("活动试听占用缓存，无法为新编排腾出空间，请先停止试听。"); return false; }
    return true;
}
}
VoiceAudioController::VoiceAudioController(ApplicationContext *context, ProjectSession *session, VoiceStudioController *document, AudioPreviewController *preview, QObject *parent)
    : QObject(parent), context_(context), session_(session), document_(document), preview_(preview)
{
    connect(session, &ProjectSession::aboutToChange, this, [this] { cancelAndWait(); stop(); rows_.clear(); waveforms_.clear(); preparedPath_.clear(); editKey_.clear(); emit changed(); });
    connect(document, &VoiceStudioController::changed, this, [this] {
        if (!editKey_.isEmpty() && editingKey(document_->snapshot(), segment_) != editKey_) { stop(); rows_.clear(); waveforms_.clear(); preparedPath_.clear(); editKey_.clear(); status_ = QStringLiteral("编排已变化，请重新准备试听；未应用候选不影响缓存。"); emit changed(); }
    });
}
VoiceAudioController::~VoiceAudioController() { cancelAndWait(); stop(); }
void VoiceAudioController::cancelAndWait() { cancel_ = true; if (preview_) preview_->stop(); task_.waitForFinished(); ++epoch_; busy_ = false; }
void VoiceAudioController::stop() { if (busy_ && preparing_) { cancelAndWait(); emit changed(); } if (preview_) preview_->stop(); }
bool VoiceAudioController::ownsPlayback() const { return preview_ && !preparedPath_.isEmpty() && preview_->sourcePath() == preparedPath_; }
qint64 VoiceAudioController::waveformBytes() const { qint64 size = 0; for (const auto &peaks : waveforms_) size += peaks.capacity() * qint64(sizeof(WaveformPeak)); return size; }
QVariantList VoiceAudioController::waveform(const QString &id) const
{
    QVariantList result; for (const auto &peak : waveforms_.value(id)) result.append(QVariant::fromValue(QVariantList{peak.min, peak.max})); return result;
}
void VoiceAudioController::prepare(bool single) { if (!busy_) { stop(); run({}, false, false, true, single); } }
void VoiceAudioController::exportWav(const QUrl &file, bool normalize, bool sentences, bool allowStale)
{
    if (!file.isLocalFile()) return;
    if (sentences && QFileInfo(file.toLocalFile()).isDir()) {
        const auto directory = QDir(file.toLocalFile()).filePath(QStringLiteral("SubCue-%1-%2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")), QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
        run(QUrl::fromLocalFile(directory), normalize, true, allowStale, false);
    } else run(file, normalize, sentences, allowStale, false);
}
void VoiceAudioController::seek(const QString &segmentId, double fraction)
{
    if (busy_ || !document_ || !preview_ || preparedPath_.isEmpty() || !std::isfinite(fraction)) return;
    for (const auto &value : rows_) {
        const auto row = value.toMap(); if (row.value(QStringLiteral("id")).toString() != segmentId) continue;
        document_->select(segmentId);
        const auto frames = row.value(QStringLiteral("position")).toString().toLongLong() + qRound64((row.value(QStringLiteral("end")).toString().toLongLong() - row.value(QStringLiteral("start")).toString().toLongLong()) * std::clamp(fraction, 0.0, 1.0));
        if (preview_->sourcePath() != preparedPath_) preview_->play(preparedPath_, preparedHash_, frames * 1000000 / 48000);
        else preview_->seek(frames * 1000000 / 48000);
        return;
    }
}
void VoiceAudioController::run(const QUrl &file, bool normalize, bool sentences, bool allowStale, bool single)
{
    if (busy_) return;
    if (session_->assetMutationBlocked()) { status_ = QStringLiteral("工程正在保存或迁移，请稍候再试听或导出。"); emit changed(); return; }
    if (!document_ || !session_->unified() || session_->storagePath().isEmpty()) { status_ = QStringLiteral("当前配音工作区不可用，请新建工程后导入文稿。"); emit changed(); return; }
    if (sentences && normalize) { status_ = QStringLiteral("句级导出不逐句拉齐电平；整轨 RMS 处理只用于整篇导出。"); emit changed(); return; }
    const auto frozen = document_->snapshot(); const auto key = editingKey(frozen, single ? document_->selectedId() : QString());
    const auto segment = single ? document_->selectedId() : QString(), project = session_->storagePath(), pinned = preview_ && (preview_->playing() || preview_->loading()) ? preparedPath_ : QString();
    const auto cacheRoot = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-preview-cache"));
    auto protectedPaths = session_->inputPaths(); protectedPaths.append(context_->settingsManager.path()); protectedPaths.append(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voices.sqlite")));
    if (!file.isEmpty()) { QString error; if (!safeOutputPath(file.toLocalFile(), protectedPaths, &error)) { status_ = error; emit changed(); return; } }
    preparing_ = file.isEmpty();
    busy_ = true; progress_ = 0; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceAudioController> self(this);
    status_ = file.isEmpty() ? QStringLiteral("后台准备按编排试听…") : QStringLiteral("正在导出冻结的 Take 与编排；后续编辑不改变本次输出。"); emit changed();
    task_ = context_->submitAudioProcessing([self, epoch, frozen, key, segment, project, cacheRoot, pinned, protectedPaths, file, normalize, sentences, allowStale, cancel] {
        QString error, problem, outputHash, outputPath; QJsonArray rows; qint64 bytes = -1; bool stale = false, reused = false, published = false;
        try {
            const auto clips = VoiceAudio::plan(project, frozen, segment, allowStale, &error, &problem); stale = error.contains(QStringLiteral("过期"));
            const auto progress = [self, epoch, cancel, last = std::make_shared<QElapsedTimer>()](int done, int total) {
                if (cancel->load() || (last->isValid() && last->elapsed() < 30 && done != total)) return; last->start();
                if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, done, total] { if (self && self->epoch_ == epoch) { self->progress_ = total ? done * 100 / total : 0; emit self->changed(); } }, Qt::QueuedConnection);
            };
            if (error.isEmpty() && file.isEmpty()) {
                QDir directory(cacheRoot); if (!directory.mkpath(QStringLiteral("."))) error = QStringLiteral("无法创建试听缓存目录。");
                if (QFileInfo(cacheRoot).canonicalFilePath().compare(QDir(QFileInfo(cacheRoot).absoluteDir().canonicalPath()).filePath(QStringLiteral("voice-preview-cache")), Qt::CaseInsensitive) != 0) error = QStringLiteral("试听缓存目录指向其它位置，拒绝写入或回收。");
                outputPath = directory.filePath(QString::fromLatin1(VoiceAudio::key(clips)) + QStringLiteral(".wav")); QFile metadata(outputPath + QStringLiteral(".json"));
                if (QFileInfo(outputPath).isSymLink() || QFileInfo(metadata.fileName()).isSymLink()) error = QStringLiteral("试听缓存文件是链接，拒绝覆盖。");
                if (error.isEmpty() && metadata.open(QIODevice::ReadOnly) && metadata.size() <= 16LL * 1024 * 1024) {
                    const auto record = QJsonDocument::fromJson(metadata.readAll()).object();
                    const auto quality = AudioQuality::inspect(outputPath, nullptr, cancel);
                    bool validRows = clips.size() <= 1000 && record.value(QStringLiteral("rows")).toArray().size() == clips.size();
                    const auto cached = record.value(QStringLiteral("rows")).toArray();
                    for (int i = 0; validRows && i < clips.size(); ++i) {
                        auto expected = clips[i].toObject(); expected.remove(QStringLiteral("path")); const auto row = cached[i].toObject();
                        for (auto field = expected.begin(); field != expected.end(); ++field) if (row.value(field.key()) != field.value()) validRows = false;
                        const auto peaks = row.value(QStringLiteral("waveform")).toArray(); if (peaks.size() != 256) validRows = false;
                        for (const auto &peak : peaks) { const auto pair = peak.toArray(); if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble() || !std::isfinite(pair[0].toDouble()) || !std::isfinite(pair[1].toDouble())) validRows = false; }
                    }
                    if (quality && validRows && record.value(QStringLiteral("key")).toString().toLatin1() == VoiceAudio::key(clips) && quality->sha256.toHex() == record.value(QStringLiteral("sha256")).toString().toLatin1()) {
                        // 缓存有效不代表源 Take 仍完整；试听与导出采用相同的资产校验规则。
                        for (const auto &value : clips) {
                            const auto clip = value.toObject(); const auto sourceQuality = AudioQuality::inspect(clip.value(QStringLiteral("path")).toString(), &error, cancel);
                            if (!sourceQuality || sourceQuality->sha256.toHex() != clip.value(QStringLiteral("sha256")).toString().toLatin1()
                                || sourceQuality->sampleRate != clip.value(QStringLiteral("sourceRate")).toInt() || sourceQuality->sampleCount != clip.value(QStringLiteral("sourceCount")).toString().toLongLong()) {
                                if (sourceQuality) error = QStringLiteral("Take 内容或规格发生变化，请恢复资产。"); problem = clip.value(QStringLiteral("id")).toString(); break;
                            }
                        }
                        if (error.isEmpty()) { rows = cached; outputHash = record.value(QStringLiteral("sha256")).toString(); reused = true; }
                    }
                }
                metadata.close();
                const auto last = clips.last().toObject(); const auto needed = (last.value(QStringLiteral("position")).toString().toLongLong() + last.value(QStringLiteral("end")).toString().toLongLong() - last.value(QStringLiteral("start")).toString().toLongLong() + last.value(QStringLiteral("pause")).toString().toLongLong()) * 3 + 44;
                if (error.isEmpty() && !reused && makeRoom(directory, pinned, needed + 16 * 1024 * 1024, &error)) {
                    rows = VoiceAudio::render(clips, outputPath, false, &error, cancel, progress);
                    const auto quality = error.isEmpty() ? AudioQuality::inspect(outputPath, &error, cancel) : std::optional<AudioQuality>{};
                    if (quality) {
                        outputHash = QString::fromLatin1(quality->sha256.toHex()); QSaveFile record(outputPath + QStringLiteral(".json"));
                        const auto data = QJsonDocument(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(VoiceAudio::key(clips))}, {QStringLiteral("sha256"), outputHash}, {QStringLiteral("rows"), rows}}).toJson(QJsonDocument::Compact);
                        if (data.size() > 16LL * 1024 * 1024 || !record.open(QIODevice::WriteOnly) || record.write(data) != data.size() || !record.commit()) error = QStringLiteral("试听缓存元数据发布失败。");
                    }
                }
                bytes = cacheSize(directory);
            } else if (error.isEmpty()) {
                auto inputs = protectedPaths; for (const auto &asset : ProjectStore(project).assets(&error)) inputs.append(ProjectStore(project).resolveAudioPath(asset.toObject().value(QStringLiteral("path")).toString()));
                if (error.isEmpty() && safeOutputPath(file.toLocalFile(), inputs, &error)) {
                    if (!sentences) { rows = VoiceAudio::render(clips, file.toLocalFile(), normalize, &error, cancel, progress, false); published = error.isEmpty() && !rows.isEmpty(); }
                    else if (QFileInfo::exists(file.toLocalFile())) error = QStringLiteral("句级导出需要一个尚不存在的新目录，避免覆盖原文件。");
                    else {
                        QTemporaryDir staging(QFileInfo(file.toLocalFile()).absoluteDir().filePath(QStringLiteral(".voice-export-XXXXXX")));
                        if (!staging.isValid()) error = QStringLiteral("无法创建句级导出临时目录。");
                        for (int i = 0; error.isEmpty() && i < clips.size(); ++i) { auto clip = clips[i].toObject(); clip.insert(QStringLiteral("position"), QStringLiteral("0")); clip.insert(QStringLiteral("pause"), QStringLiteral("0"));
                            (void)VoiceAudio::render({clip}, staging.filePath(QStringLiteral("%1-%2.wav").arg(i + 1, 4, 10, QLatin1Char('0')).arg(clip.value(QStringLiteral("id")).toString())), normalize, &error, cancel, {}, false); progress(i + 1, clips.size()); }
                        if (error.isEmpty() && !cancel->load()) { published = QDir().rename(staging.path(), file.toLocalFile()); if (!published) error = QStringLiteral("句级文件发布失败，原目标保持不变。"); }
                    }
                }
            }
        } catch (...) { error = QStringLiteral("音频处理发生异常，请检查资产、磁盘和内存。"); }
        if (cancel->load() && !published) error = QStringLiteral("音频处理已取消，已发布的完整缓存可在下次复用。");
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, frozen, key, segment, file, normalize, sentences, rows, outputHash, outputPath, bytes, error, problem, stale, reused] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false; if (bytes >= 0) self->cacheBytes_ = bytes;
            bool clipping = false; for (const auto &row : rows) clipping |= row.toObject().value(QStringLiteral("clipping")).toBool();
            if (!error.isEmpty()) {
                if (file.isEmpty()) { self->stop(); self->rows_.clear(); self->waveforms_.clear(); self->preparedPath_.clear(); self->preparedHash_.clear(); self->editKey_.clear(); }
                self->status_ = error; if (!problem.isEmpty() && self->document_) self->document_->select(problem); if (stale && !cancel->load()) emit self->staleExportRequested(file, normalize, sentences);
            }
            else if (!file.isEmpty()) self->status_ = QStringLiteral("WAV 导出完成：48kHz／24-bit PCM／单声道。%1").arg(normalize ? QStringLiteral("整轨 RMS 平均电平处理已开启（非 LUFS）。") : clipping ? QStringLiteral("编排增益导致削波，请试听并降低增益。") : QStringLiteral("未进行响度处理。"));
            else if (self->document_ && editingKey(self->document_->snapshot(), segment) == key) {
                self->rows_.clear(); self->waveforms_.clear();
                for (const auto &value : rows) { auto row = value.toObject(); QVector<WaveformPeak> peaks; peaks.reserve(256);
                    for (const auto &peak : row.value(QStringLiteral("waveform")).toArray()) { const auto pair = peak.toArray(); peaks.append({static_cast<float>(pair[0].toDouble()), static_cast<float>(pair[1].toDouble())}); }
                    self->waveforms_.insert(row.value(QStringLiteral("id")).toString(), std::move(peaks)); row.remove(QStringLiteral("waveform")); self->rows_.append(row.toVariantMap()); }
                self->segment_ = segment; self->editKey_ = key; self->preparedPath_ = outputPath; self->preparedHash_ = outputHash;
                self->status_ = reused ? QStringLiteral("已复用编排缓存。") : QStringLiteral("编排缓存已准备完成。");
                if (clipping) self->status_ += QStringLiteral("编排增益导致削波，请降低增益。");
                if (self->active_ && self->preview_) self->preview_->play(outputPath, outputHash);
            } else self->status_ = QStringLiteral("准备期间编排已改变，旧结果仅保留缓存，未自动播放。");
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}
}
