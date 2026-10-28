#include "voice_studio_controller.h"
#include "application_context.h"
#include "project_session.h"
#include "tts/voice_document.h"
#include "tts/voice_library.h"
#include "tts/audio_quality.h"
#include "project/project_store.h"
#include "tts_task_controller.h"
#include "common/file_path_guard.h"
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QUuid>
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethod>
#include <QtGui/QUndoCommand>

namespace subcue {
namespace {
class ManuscriptFilter final : public QSortFilterProxyModel {
public:
    explicit ManuscriptFilter(QObject *parent) : QSortFilterProxyModel(parent) { setFilterCaseSensitivity(Qt::CaseInsensitive); setFilterRole(VoiceStudioController::Body); }
    void configure(const QString &text, const QString &state, int paragraph) {
        beginFilterChange(); state_ = state; paragraph_ = paragraph;
        setFilterFixedString(text); endFilterChange();
    }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
        if (!QSortFilterProxyModel::filterAcceptsRow(row, parent)) return false;
        const auto item = sourceModel()->index(row, 0, parent);
        if (paragraph_ > 0 && item.data(VoiceStudioController::ParagraphNumber).toInt() != paragraph_) return false;
        const bool applied = !item.data(VoiceStudioController::SelectedTake).toMap().isEmpty();
        return state_ == QStringLiteral("all") || (state_ == QStringLiteral("missing") && !applied)
            || (state_ == QStringLiteral("applied") && applied) || (state_ == QStringLiteral("stale") && item.data(VoiceStudioController::Stale).toBool());
    }
private:
    QString state_ = QStringLiteral("all");
    int paragraph_ = 0;
};
class EditCommand final : public QUndoCommand {
public:
    EditCommand(std::function<void(const QJsonObject &)> apply, QJsonObject before, QJsonObject after, const QString &text)
        : QUndoCommand(text), apply_(std::move(apply)), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { apply_(before_); }
    void redo() override { apply_(after_); }
private:
    std::function<void(const QJsonObject &)> apply_;
    QJsonObject before_, after_;
};
QJsonObject itemFor(const QJsonObject &document, const QString &id)
{
    for (const auto &value : document.value(QStringLiteral("segments")).toArray()) if (value.toObject().value(QStringLiteral("id")).toString() == id) return value.toObject();
    return {};
}
QJsonObject candidateFor(const QJsonObject &row)
{
    const auto input = row.value(QStringLiteral("input")).toObject(); const auto result = row.value(QStringLiteral("result")).toObject();
    const auto quality = result.value(QStringLiteral("quality")).toObject();
    QJsonObject frozen{{QStringLiteral("spoken"), input.value(QStringLiteral("spokenText"))}, {QStringLiteral("manualSpoken"), true}};
    for (const auto &key : {QStringLiteral("voiceVersionId"), QStringLiteral("parameters"), QStringLiteral("provider"), QStringLiteral("model"), QStringLiteral("modelVersion"), QStringLiteral("language")}) frozen.insert(key, input.value(key));
    return {{QStringLiteral("id"), row.value(QStringLiteral("id"))}, {QStringLiteral("segmentId"), input.value(QStringLiteral("segmentId"))},
        {QStringLiteral("path"), row.value(QStringLiteral("audioPath"))}, {QStringLiteral("sampleCount"), quality.value(QStringLiteral("sampleCount"))},
        {QStringLiteral("sampleRate"), quality.value(QStringLiteral("sampleRate"))}, {QStringLiteral("sha256"), quality.value(QStringLiteral("sha256"))},
        {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(VoiceDocument::empty(), frozen).toHex())},
        {QStringLiteral("inputVersion"), input.value(QStringLiteral("inputVersion"))}, {QStringLiteral("mock"), result.value(QStringLiteral("mock"))},
        {QStringLiteral("warnings"), quality.value(QStringLiteral("warnings"))}, {QStringLiteral("diagnostics"), result.value(QStringLiteral("diagnostics"))}};
}
bool hasCurrentCandidate(ProjectStore &store, const QJsonObject &document, const QJsonObject &item,
                         QString *error, const std::atomic<bool> *cancel)
{
    const auto key = VoiceDocument::inputKey(document, item);
    const auto selected = item.value(QStringLiteral("selectedTake")).toObject();
    // 输入未变不代表文件仍然可用；已应用音频同样需要校验项目资产和实际内容。
    if (!selected.isEmpty() && QByteArray::fromHex(selected.value(QStringLiteral("inputKey")).toString().toLatin1()) == key) {
        for (const auto &value : store.assets(error)) {
            const auto asset = value.toObject();
            if (asset.value(QStringLiteral("id")) != selected.value(QStringLiteral("id"))) continue;
            if (asset.value(QStringLiteral("kind")).toString() == QStringLiteral("take") && asset.value(QStringLiteral("sha256")) == selected.value(QStringLiteral("sha256"))) {
                const auto quality = AudioQuality::inspect(store.resolveAudioPath(asset.value(QStringLiteral("path")).toString()), nullptr, cancel);
                if (quality && quality->sha256.toHex() == selected.value(QStringLiteral("sha256")).toString().toLatin1()
                    && quality->sampleRate == selected.value(QStringLiteral("sampleRate")).toInt()
                    && quality->sampleCount == selected.value(QStringLiteral("sampleCount")).toString().toLongLong()) return true;
            }
            break;
        }
        if (!error->isEmpty()) return false;
    }
    for (int offset = 0; !cancel->load(); offset += 50) {
        const auto rows = store.segmentTakes(item.value(QStringLiteral("id")).toString(), offset, 50, error);
        if (!error->isEmpty()) return false;
        for (const auto &row : rows) {
            const auto take = candidateFor(row.toObject());
            if (take.value(QStringLiteral("inputVersion")) == item.value(QStringLiteral("inputVersion"))
                && QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()) == key
                && QFileInfo::exists(take.value(QStringLiteral("path")).toString())) {
                const auto quality = AudioQuality::inspect(take.value(QStringLiteral("path")).toString(), nullptr, cancel);
                if (quality && quality->sha256 == QByteArray::fromHex(take.value(QStringLiteral("sha256")).toString().toLatin1())) return true;
            }
        }
        if (rows.size() < 50) return false;
    }
    return false;
}
}
VoiceStudioController::VoiceStudioController(ApplicationContext *context, ProjectSession *session, QObject *parent)
    : QAbstractListModel(parent), context_(context), session_(session), document_(VoiceDocument::empty()), saved_(document_)
{
    QFile viewState(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-editor-state.json")));
    if (viewState.open(QIODevice::ReadOnly) && viewState.size() <= 65536) viewState_ = QJsonDocument::fromJson(viewState.readAll()).object();
    filter_ = new ManuscriptFilter(this); filter_->setSourceModel(this);
    undo_.setUndoLimit(100);
    connect(&undo_, &QUndoStack::indexChanged, this, &VoiceStudioController::changed);
    connect(session, &ProjectSession::aboutToChange, this, &VoiceStudioController::cancelAndWait);
    connect(session, &ProjectSession::opened, this, [this](bool success) { if (success) loadCandidates(); });
    session->setVoiceController(this);
}
VoiceStudioController::~VoiceStudioController() { cancelAndWait(); }
void VoiceStudioController::setManuscriptFilter(const QString &text, const QString &state, int paragraph)
{
    static_cast<ManuscriptFilter *>(filter_)->configure(text, state, std::max(0, paragraph));
}
int VoiceStudioController::filteredIndex(const QString &id) const
{
    return filter_->mapFromSource(index(segmentIndex(id))).row();
}
QString VoiceStudioController::adjacentFilteredId(int direction, bool pending) const
{
    if (direction != -1 && direction != 1) return {};
    int row = filteredIndex(selectedId_);
    if (row < 0) row = direction > 0 ? -1 : filter_->rowCount();
    for (row += direction; row >= 0 && row < filter_->rowCount(); row += direction) {
        const auto item = filter_->index(row, 0);
        if (!pending || item.data(SelectedTake).toMap().isEmpty() || item.data(Stale).toBool()) return item.data(Id).toString();
    }
    return {};
}
int VoiceStudioController::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : document_.value(QStringLiteral("segments")).toArray().size(); }
QHash<int, QByteArray> VoiceStudioController::roleNames() const
{
    return {{Id, "segmentId"}, {ParagraphId, "paragraphId"}, {Body, "body"}, {Spoken, "spoken"}, {Manual, "manualSpoken"},
        {Notes, "notes"}, {VoiceVersion, "voiceVersionId"}, {Gain, "gain"}, {Pause, "pauseMs"}, {SelectedTake, "selectedTake"}, {Stale, "stale"}, {ParagraphNumber, "paragraphNumber"}, {VoiceName, "voiceName"}, {Locked, "locked"}, {GenerationState, "generationState"}};
}
QVariant VoiceStudioController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto item = document_.value(QStringLiteral("segments")).toArray()[index.row()].toObject();
    if (role == ParagraphNumber) {
        const auto paragraphs = document_.value(QStringLiteral("paragraphs")).toArray();
        for (int i = 0; i < paragraphs.size(); ++i) if (paragraphs[i].toObject().value(QStringLiteral("id")) == item.value(QStringLiteral("paragraphId"))) return i + 1;
        return {};
    }
    if (role == Locked) return item.value(QStringLiteral("locked")).toBool();
    if (role == GenerationState) {
        if (tasks_ && tasks_->segmentState(item.value(QStringLiteral("id")).toString()) == QStringLiteral("generating")) return QStringLiteral("generating");
        if (!item.value(QStringLiteral("selectedTake")).toObject().isEmpty()) return data(index, Stale).toBool() ? QStringLiteral("stale") : QStringLiteral("applied");
        if (VoiceDocument::effectiveVoiceVersion(document_, item).isEmpty()) return QStringLiteral("unbound");
        const auto state = generationStates_.value(item.value(QStringLiteral("id")).toString()).toObject();
        if (state.value(QStringLiteral("inputVersion")) == item.value(QStringLiteral("inputVersion")) && state.value(QStringLiteral("inputKey")).toString() == QString::fromLatin1(VoiceDocument::inputKey(document_, item).toHex())) return state.value(QStringLiteral("state")).toString();
        if (QStringList{QStringLiteral("candidate"), QStringLiteral("stale")}.contains(state.value(QStringLiteral("state")).toString())) return QStringLiteral("stale");
        return QStringLiteral("pending");
    }
    if (role == Spoken) return VoiceDocument::spokenText(document_, item);
    if (role == VoiceVersion) return VoiceDocument::effectiveVoiceVersion(document_, item);
    if (role == VoiceName) {
        const auto voice = VoiceDocument::effectiveVoiceVersion(document_, item);
        return voice.isEmpty() ? QStringLiteral("未选择音色") : document_.value(QStringLiteral("voiceNames")).toObject().value(voice).toString(QStringLiteral("项目音色"));
    }
    if (role == Stale) {
        const auto take = item.value(QStringLiteral("selectedTake")).toObject();
        return !take.isEmpty() && QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()) != VoiceDocument::inputKey(document_, item);
    }
    const QHash<int, QString> fields{{Id, QStringLiteral("id")}, {ParagraphId, QStringLiteral("paragraphId")}, {Body, QStringLiteral("text")},
        {Manual, QStringLiteral("manualSpoken")}, {Notes, QStringLiteral("notes")}, {VoiceVersion, QStringLiteral("voiceVersionId")},
        {Gain, QStringLiteral("gain")}, {Pause, QStringLiteral("pauseMs")}, {SelectedTake, QStringLiteral("selectedTake")}};
    return item.value(fields.value(role)).toVariant();
}
bool VoiceStudioController::modified() const { return document_ != saved_; }
QVariantMap VoiceStudioController::selected() const { auto item = itemFor(document_, selectedId_); item.insert(QStringLiteral("effectiveSpoken"), VoiceDocument::spokenText(document_, item)); item.insert(QStringLiteral("effectiveVoiceVersionId"), VoiceDocument::effectiveVoiceVersion(document_, item)); return item.toVariantMap(); }
void VoiceStudioController::reset(const QJsonObject &document)
{
    QString error;
    auto next = document.isEmpty() ? VoiceDocument::empty() : document;
    if (next.value(QStringLiteral("version")).toInt() == 1) { next.insert(QStringLiteral("version"), 2); next.insert(QStringLiteral("defaultVoiceVersionId"), QString()); }
    if (!VoiceDocument::validate(next, &error)) { status_ = error; emit changed(); return; }
    flashPreview_ = {}; flashJobs_.clear(); flashDocument_ = {};
    undo_.clear(); selectedIds_.clear(); selectedId_.clear(); candidates_.clear(); projectVoices_.clear(); generationStates_ = {}; candidateOffset_ = 0; emit candidatesChanged(); discardPreview(); replace(next); saved_ = next; emit documentReset(); emit changed();
}
void VoiceStudioController::persistSessionState(const QVariantMap &state)
{
    viewState_ = QJsonObject::fromVariantMap(state);
    QSaveFile file(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-editor-state.json")));
    const auto bytes = QJsonDocument(viewState_).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { status_ = QStringLiteral("布局保存失败，请检查应用数据目录权限。"); emit changed(); }
}
void VoiceStudioController::setSessionState(const QVariantMap &state)
{
    persistSessionState(state);
    if (session_->unified()) { document_.insert(QStringLiteral("session"), QJsonObject::fromVariantMap(state)); emit changed(); }
}
void VoiceStudioController::markSaved(const QJsonObject &document)
{
    saved_ = document;
    // 另存后旧候选的绝对路径已经失效，从当前存储重新解析，保留正文和撤销栈。
    candidates_.clear(); emit candidatesChanged(); emit changed();
}
void VoiceStudioController::select(const QString &id)
{
    if (itemFor(document_, id).isEmpty()) return;
    selectedIds_ = {id};
    if (selectedId_ != id) { selectedId_ = id; candidates_.clear(); emit candidatesChanged(); loadCandidates(); }
    emit selectionChanged();
}
void VoiceStudioController::selectMany(const QString &id, bool toggle, bool range)
{
    if (itemFor(document_, id).isEmpty()) return;
    auto ids = selectedIds_;
    if (range) {
        const int first = filteredIndex(selectedId_), last = filteredIndex(id);
        if (first < 0 || last < 0) return;
        for (int row = std::min(first, last); row <= std::max(first, last); ++row) {
            const auto value = filter_->index(row, 0).data(Id).toString(); if (!ids.contains(value)) ids.append(value);
        }
    } else if (toggle) { if (ids.contains(id)) ids.removeAll(id); else ids.append(id); }
    else ids = {id};
    select(id); selectedIds_ = ids.isEmpty() ? QStringList{id} : ids; emit selectionChanged();
}
QVariantMap VoiceStudioController::selectionProperties() const
{
    QVariantMap result; int count = 0;
    for (const auto &id : selectedIds_) {
        const auto item = itemFor(document_, id); if (item.isEmpty() || item.value(QStringLiteral("locked")).toBool()) continue;
        for (const auto &field : {QStringLiteral("language"), QStringLiteral("gain"), QStringLiteral("pauseMs"), QStringLiteral("parameters")}) {
            if (count == 0) result.insert(field, item.value(field).toVariant());
            else if (result.value(field) != item.value(field).toVariant()) result.insert(field, QVariant());
        }
        ++count;
    }
    result.insert(QStringLiteral("editable"), count); result.insert(QStringLiteral("locked"), selectedIds_.size() - count); return result;
}
void VoiceStudioController::setLocked(bool locked)
{
    if (busy_ || session_->assetMutationBlocked()) return;
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); if (!selectedIds_.contains(item.value(QStringLiteral("id")).toString())) continue;
        if (locked) item.insert(QStringLiteral("voiceVersionId"), VoiceDocument::effectiveVoiceVersion(document_, item));
        item.insert(QStringLiteral("locked"), locked); items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items); commit(next, locked ? QStringLiteral("选中句已锁定，批量操作会跳过。") : QStringLiteral("选中句已解锁。"));
}
void VoiceStudioController::editSelection(const QVariantMap &fields)
{
    if (busy_ || session_->assetMutationBlocked()) return;
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); const auto before = item;
        if (!selectedIds_.contains(item.value(QStringLiteral("id")).toString()) || item.value(QStringLiteral("locked")).toBool()) continue;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!QStringList{QStringLiteral("language"), QStringLiteral("gain"), QStringLiteral("pauseMs"), QStringLiteral("parameters")}.contains(it.key())) { status_ = QStringLiteral("不支持的批量编辑字段。"); emit changed(); return; }
            item.insert(it.key(), QJsonValue::fromVariant(it.value()));
        }
        const auto provider = item.value(QStringLiteral("provider")).toString();
        const auto capabilities = engineCapabilities(provider == QStringLiteral("qwen-cloud") ? QStringLiteral("cloud") : provider == QStringLiteral("index-local") ? QStringLiteral("index") : provider == QStringLiteral("gpt-local") ? QStringLiteral("gpt") : provider == QStringLiteral("mock") ? QStringLiteral("mock") : QStringLiteral("qwen"));
        if (!capabilities.value(QStringLiteral("languages")).toStringList().contains(item.value(QStringLiteral("language")).toString())) { status_ = QStringLiteral("选中句中存在不支持该语言的引擎，未应用任何修改。"); emit changed(); return; }
        const auto parameters = item.value(QStringLiteral("parameters")).toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it) if (!capabilities.value(QStringLiteral("parameters")).toStringList().contains(it.key())) { status_ = QStringLiteral("选中句中存在不支持该参数的引擎，未应用任何修改。"); emit changed(); return; }
        if (VoiceDocument::inputKey(document_, before) != VoiceDocument::inputKey(next, item)) item.insert(QStringLiteral("inputVersion"), QString::number(before.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1));
        items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items); commit(next, QStringLiteral("已批量应用共同属性；锁定句跳过，旧音频与裁切保留。"));
}
void VoiceStudioController::replace(const QJsonObject &document)
{
    const bool voiceNamesChanged = document_.value(QStringLiteral("voiceNames")) != document.value(QStringLiteral("voiceNames"));
    const auto before = document_.value(QStringLiteral("segments")).toArray(); const auto after = document.value(QStringLiteral("segments")).toArray();
    bool same = before.size() == after.size();
    for (int i = 0; same && i < before.size(); ++i) same = before[i].toObject().value(QStringLiteral("id")) == after[i].toObject().value(QStringLiteral("id"));
    if (!same) beginResetModel(); document_ = document;
    if (!same) endResetModel();
    else for (int i = 0; i < after.size(); ++i) if (before[i] != after[i]) emit dataChanged(index(i), index(i));
    if (same && voiceNamesChanged && !after.isEmpty()) emit dataChanged(index(0), index(after.size() - 1), {VoiceName});
    if (itemFor(document_, selectedId_).isEmpty()) selectedId_ = after.isEmpty() ? QString() : after.first().toObject().value(QStringLiteral("id")).toString();
    for (int i = selectedIds_.size(); i-- > 0;) if (itemFor(document_, selectedIds_[i]).isEmpty()) selectedIds_.removeAt(i);
    if (selectedIds_.isEmpty() && !selectedId_.isEmpty()) selectedIds_ = {selectedId_};
    emit changed(); emit selectionChanged();
}
bool VoiceStudioController::commit(const QJsonObject &document, const QString &description)
{
    QString error;
    if (!session_->unified()) { status_ = QStringLiteral("请先将旧工程转换为统一副本，配音编辑只保存到项目 SQLite。"); emit changed(); return false; }
    if (!VoiceDocument::validate(document, &error)) { status_ = error; emit changed(); return false; }
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray()) {
        auto before = value.toObject(); if (!before.value(QStringLiteral("locked")).toBool()) continue;
        auto after = itemFor(document, before.value(QStringLiteral("id")).toString());
        before.remove(QStringLiteral("locked")); after.remove(QStringLiteral("locked"));
        if (before != after || VoiceDocument::inputKey(document_, value.toObject()) != VoiceDocument::inputKey(document, itemFor(document, value.toObject().value(QStringLiteral("id")).toString()))) {
            status_ = QStringLiteral("操作影响已锁定句，请先解锁；文稿和音频保持不变。"); emit changed(); return false;
        }
    }
    if (document == document_) { status_ = description; emit changed(); return true; }
    const QPointer<VoiceStudioController> self(this);
    undo_.push(new EditCommand([self](const QJsonObject &state) { if (self) self->replace(state); }, document_, document, description));
    status_ = description; emit changed(); return true;
}
void VoiceStudioController::editSegment(const QString &id, const QString &text, const QString &spoken, bool manual, const QString &notes)
{
    commit(VoiceDocument::edit(document_, id, text, spoken, manual, notes), QStringLiteral("句子编辑已提交。"));
}
bool VoiceStudioController::commitDraft(const QVariantMap &draft)
{
    const auto typed = QJsonObject::fromVariantMap(draft);
    if (!typed.value(QStringLiteral("text")).isString() || !typed.value(QStringLiteral("spoken")).isString() || !typed.value(QStringLiteral("notes")).isString()
        || !typed.value(QStringLiteral("manualSpoken")).isBool() || !typed.value(QStringLiteral("gain")).isDouble() || !typed.value(QStringLiteral("pauseMs")).isDouble()) {
        status_ = QStringLiteral("草稿字段不完整或类型无效，未提交。"); emit changed(); return false;
    }
    const auto id = draft.value(QStringLiteral("id")).toString(); const auto before = itemFor(document_, id);
    if (before.isEmpty()) { status_ = QStringLiteral("当前句已不存在，草稿未应用。"); emit changed(); return false; }
    auto next = VoiceDocument::edit(document_, id, draft.value(QStringLiteral("text")).toString(), draft.value(QStringLiteral("spoken")).toString(), draft.value(QStringLiteral("manualSpoken")).toBool(), draft.value(QStringLiteral("notes")).toString());
    auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); if (item.value(QStringLiteral("id")).toString() != id) continue;
        item.insert(QStringLiteral("gain"), draft.value(QStringLiteral("gain")).toDouble()); item.insert(QStringLiteral("pauseMs"), draft.value(QStringLiteral("pauseMs")).toDouble());
        if (draft.contains(QStringLiteral("language"))) {
            if (!QStringList{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}.contains(draft.value(QStringLiteral("language")).toString())) { status_ = QStringLiteral("请选择中文、英语或日语；混语先拆为显式语言片段。"); emit changed(); return false; }
            item.insert(QStringLiteral("language"), typed.value(QStringLiteral("language")));
        }
        const auto seed = draft.value(QStringLiteral("seed")).toString().trimmed(); auto parameters = item.value(QStringLiteral("parameters")).toObject();
        if (seed.isEmpty()) parameters.remove(QStringLiteral("seed"));
        else { bool valid = false; const auto value = seed.toLongLong(&valid); if (!valid || value < 0 || value > 2147483647) { status_ = QStringLiteral("seed 必须是 0～2147483647 的整数，或留空。"); emit changed(); return false; } parameters.insert(QStringLiteral("seed"), value); }
        if (draft.value(QStringLiteral("indexParameters")).toBool()) {
            if (typed.value(QStringLiteral("durationFactor")).toDouble(-1) == 1) parameters.remove(QStringLiteral("durationFactor"));
            else parameters.insert(QStringLiteral("durationFactor"), typed.value(QStringLiteral("durationFactor")));
            if (draft.value(QStringLiteral("emotion")).toString().isEmpty()) { parameters.remove(QStringLiteral("emotion")); parameters.remove(QStringLiteral("emotionStrength")); }
            else { parameters.insert(QStringLiteral("emotion"), typed.value(QStringLiteral("emotion"))); parameters.insert(QStringLiteral("emotionStrength"), typed.value(QStringLiteral("emotionStrength"))); }
        }
        if (draft.value(QStringLiteral("gptParameters")).toBool()) {
            if (typed.value(QStringLiteral("speedFactor")).toDouble(-1) == 1) parameters.remove(QStringLiteral("speedFactor"));
            else parameters.insert(QStringLiteral("speedFactor"), typed.value(QStringLiteral("speedFactor")));
        }
        if (!validateTtsParameters(parameters, &status_)) { emit changed(); return false; }
        item.insert(QStringLiteral("parameters"), parameters); auto take = item.value(QStringLiteral("selectedTake")).toObject();
        if (!take.isEmpty()) {
            bool validStart = false, validEnd = false; const auto start = draft.value(QStringLiteral("trimStart")).toString().toLongLong(&validStart), end = draft.value(QStringLiteral("trimEnd")).toString().toLongLong(&validEnd);
            if (!validStart || !validEnd || start < 0 || end <= start || end > take.value(QStringLiteral("sampleCount")).toString().toLongLong()) { status_ = QStringLiteral("草稿裁切范围无效，正文和参数均未提交。"); emit changed(); return false; }
            take.insert(QStringLiteral("trimStart"), QString::number(start)); take.insert(QStringLiteral("trimEnd"), QString::number(end)); take.insert(QStringLiteral("manualCrop"), start != 0 || end != take.value(QStringLiteral("sampleCount")).toString().toLongLong()); item.insert(QStringLiteral("selectedTake"), take);
        }
        item.insert(QStringLiteral("inputVersion"), QString::number(before.value(QStringLiteral("inputVersion")).toString().toLongLong()
            + (VoiceDocument::inputKey(document_, before) != VoiceDocument::inputKey(next, item) ? 1 : 0))); items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items); return commit(next, QStringLiteral("检查区草稿已作为一个编辑命令提交。"));
}
void VoiceStudioController::propose(const QJsonObject &document, const QVariantList &differences, const QString &message)
{
    QString error;
    if (!VoiceDocument::validate(document, &error)) { status_ = error; emit changed(); return; }
    pending_ = document; previewBase_ = document_; preview_ = differences; status_ = message; emit previewChanged(); emit changed();
}
void VoiceStudioController::previewText(const QString &text, const QString &engine)
{
    auto next = VoiceDocument::appendText(document_, text); QVariantList differences;
    auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = rowCount(); i < items.size(); ++i) {
        auto item = items[i].toObject(); const auto body = item.value(QStringLiteral("text")).toString();
        if (!engine.isEmpty()) {
            const bool kana = QRegularExpression(QStringLiteral("[\\x{3040}-\\x{30ff}]")).match(body).hasMatch();
            const bool han = QRegularExpression(QStringLiteral("[\\x{3400}-\\x{9fff}]")).match(body).hasMatch();
            const bool latin = QRegularExpression(QStringLiteral("[A-Za-z]")).match(body).hasMatch();
            const auto language = kana && !latin ? QStringLiteral("Japanese") : han && !latin && !kana ? QStringLiteral("Chinese") : latin && !han && !kana ? QStringLiteral("English") : QString();
            if (!language.isEmpty() && engineCapabilities(engine).value(QStringLiteral("languages")).toStringList().contains(language)) item.insert(QStringLiteral("language"), language);
            differences.append(QVariantMap{{QStringLiteral("kind"), language.isEmpty() ? QStringLiteral("新增句子 · 混合语言或语言不明确，请确认目标语言") : QStringLiteral("新增句子 · 语言为自动建议，请确认")}, {QStringLiteral("after"), item.toVariantMap()}});
        } else differences.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("新增句子")}, {QStringLiteral("after"), item.toVariantMap()}});
        items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items);
    propose(next, differences, QStringLiteral("预览按标点分段，确认后固定 ID；现有段落保持不变。"));
}
void VoiceStudioController::previewSplit(int offset)
{
    QString error; const auto next = VoiceDocument::split(document_, selectedId_, offset, &error);
    if (!error.isEmpty()) { status_ = error; emit changed(); return; }
    propose(next, {QVariantMap{{QStringLiteral("kind"), QStringLiteral("拆分：旧手工朗读、裁切及选中 Take 保留在归档；新句需重新核对")}, {QStringLiteral("before"), selected()}}}, QStringLiteral("确认拆分后创建新 ID，历史不会删除。"));
}
void VoiceStudioController::previewMerge()
{
    QString error; const auto next = VoiceDocument::merge(document_, selectedId_, &error);
    if (!error.isEmpty()) { status_ = error; emit changed(); return; }
    propose(next, {QVariantMap{{QStringLiteral("kind"), QStringLiteral("合并相邻句：保留朗读修正，使用第一句声音／参数；旧裁切与 Take 归档")}, {QStringLiteral("before"), selected()}}}, QStringLiteral("请确认声音参数及断句，合并结果需新候选。"));
}
void VoiceStudioController::previewResegment(const QString &text)
{
    const auto next = VoiceDocument::resegment(document_, selected().value(QStringLiteral("paragraphId")).toString(), text);
    propose(next, {QVariantMap{{QStringLiteral("kind"), QStringLiteral("仅重分选中段落：原句与手工结果归档，新句需重新绑定声音")}, {QStringLiteral("text"), text}}}, QStringLiteral("其它段落及其选中 Take 不变。"));
}
QString VoiceStudioController::paragraphText() const
{
    QString text; const auto paragraph = itemFor(document_, selectedId_).value(QStringLiteral("paragraphId"));
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray()) if (value.toObject().value(QStringLiteral("paragraphId")) == paragraph) text += value.toObject().value(QStringLiteral("text")).toString();
    return text;
}
int VoiceStudioController::segmentIndex(const QString &id) const
{
    const auto items = document_.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) if (items[i].toObject().value(QStringLiteral("id")).toString() == id) return i;
    return -1;
}
void VoiceStudioController::previewDelete() { propose(VoiceDocument::remove(document_, selectedId_), {QVariantMap{{QStringLiteral("kind"), QStringLiteral("删除句子并归档，生成历史保留")}, {QStringLiteral("before"), selected()}}}, QStringLiteral("确认删除当前句子。")); }
void VoiceStudioController::applyPreview()
{
    if (QGuiApplication::inputMethod()) QGuiApplication::inputMethod()->commit();
    if (pending_.isEmpty()) return;
    if (document_ != previewBase_) { status_ = QStringLiteral("预览期间文稿有新修改，请重新预览。" ); emit changed(); return; }
    const auto next = pending_; discardPreview(); commit(next, QStringLiteral("预览变更已应用。"));
}
void VoiceStudioController::discardPreview() { pending_ = {}; previewBase_ = {}; preview_.clear(); emit previewChanged(); }
void VoiceStudioController::undo() { undo_.undo(); }
void VoiceStudioController::redo() { undo_.redo(); }
void VoiceStudioController::setArrangement(double gain, int pauseMs, const QString &startSample, const QString &endSample)
{
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); if (item.value(QStringLiteral("id")).toString() != selectedId_) continue;
        auto take = item.value(QStringLiteral("selectedTake")).toObject();
        if (!take.isEmpty()) {
            bool validStart = false, validEnd = false; const auto start = startSample.toLongLong(&validStart), end = endSample.toLongLong(&validEnd);
            if (!validStart || !validEnd || start < 0 || end <= start || end > take.value(QStringLiteral("sampleCount")).toString().toLongLong()) {
                status_ = QStringLiteral("裁切范围无效，未静默截断。"); emit changed(); return;
            }
            take.insert(QStringLiteral("trimStart"), QString::number(start)); take.insert(QStringLiteral("trimEnd"), QString::number(end));
            take.insert(QStringLiteral("manualCrop"), start != 0 || end != take.value(QStringLiteral("sampleCount")).toString().toLongLong());
            item.insert(QStringLiteral("selectedTake"), take);
        }
        item.insert(QStringLiteral("gain"), gain); item.insert(QStringLiteral("pauseMs"), pauseMs); items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items); commit(next, QStringLiteral("裁切、增益与停顿已编辑，生成输入不受影响。"));
}
void VoiceStudioController::cancelAndWait() { discardGeneration(); if (candidateCancel_) *candidateCancel_ = true; candidateTask_.waitForFinished(); ++candidateEpoch_; candidatesLoading_ = false; cancel_ = true; task_.waitForFinished(); ++epoch_; busy_ = false; if (flashConnection_.value(QStringLiteral("state")) == QStringLiteral("testing")) { flashConnection_.insert(QStringLiteral("state"), QStringLiteral("failed")); flashConnection_.insert(QStringLiteral("detail"), QStringLiteral("连接测试已取消。")); } }
void VoiceStudioController::setTasks(TtsTaskController *tasks)
{
    if (tasks_) disconnect(tasks_, nullptr, this, nullptr); tasks_ = tasks;
    const QPointer<VoiceStudioController> self(this);
    if (tasks_) { connect(tasks_, &TtsTaskController::statusChanged, this, [this] { if (rowCount() > 0) emit dataChanged(index(0), index(rowCount() - 1), {GenerationState}); emit changed(); });
        connect(tasks_, &TtsTaskController::candidateCommitted, this, [self] { if (self) self->loadCandidates(); });
        connect(tasks_, &TtsTaskController::finished, this, [self] { if (self) self->loadCandidates(); }); }
}
void VoiceStudioController::bindVoiceVersion(const QString &versionId, const QString &scope, const QString &displayName)
{
    if (busy_ || session_->assetMutationBlocked() || (selectedId_.isEmpty() && scope != QStringLiteral("default"))) return;
    if (scope != QStringLiteral("selected") && scope != QStringLiteral("paragraph") && scope != QStringLiteral("unbound") && scope != QStringLiteral("default") && scope != QStringLiteral("selection")) return;
    if (!session_->unified() || session_->storagePath().isEmpty()) { status_ = QStringLiteral("当前工作区暂不可用，请新建工程或将旧工程转换为统一副本。"); emit changed(); return; }
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); auto *cancel = &cancel_;
    QStringList ids;
    const auto selected = itemFor(document_, selectedId_);
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray()) {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("locked")).toBool()) continue;
        if ((scope == QStringLiteral("selection") && selectedIds_.contains(item.value(QStringLiteral("id")).toString()))
            || (scope == QStringLiteral("selected") && item.value(QStringLiteral("id")).toString() == selectedId_)
            || (scope == QStringLiteral("paragraph") && item.value(QStringLiteral("paragraphId")) == selected.value(QStringLiteral("paragraphId")))
            || (scope == QStringLiteral("unbound") && item.value(QStringLiteral("voiceVersionId")).toString().isEmpty())) ids.append(item.value(QStringLiteral("id")).toString());
    }
    const auto path = session_->storagePath();
    const auto libraryPath = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voices.sqlite")); emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, ids, scope, path, libraryPath, versionId, displayName] {
        QString error; ProjectStore store(path);
        bool success = false;
        try {
            auto version = store.voiceVersion(versionId, &error);
            if (!version && error.isEmpty()) success = VoiceLibrary(libraryPath).copyVersionToProject(versionId, store, &error, cancel);
            else success = version.has_value();
        } catch (...) { error = QStringLiteral("声音复制发生异常，请检查参考资产和磁盘。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, ids, scope, versionId, displayName, success, error] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (!success || cancel->load()) { self->status_ = cancel->load() ? QStringLiteral("声音绑定已取消。") : error; emit self->changed(); return; }
            auto next = self->document_; auto items = next.value(QStringLiteral("segments")).toArray();
            if (!displayName.trimmed().isEmpty()) {
                auto names = next.value(QStringLiteral("voiceNames")).toObject(); names.insert(versionId, displayName.trimmed()); next.insert(QStringLiteral("voiceNames"), names);
            }
            if (scope == QStringLiteral("default")) {
                self->commit(VoiceDocument::setDefaultVoice(next, versionId), QStringLiteral("默认旁白已更新；局部音色、已有音频及人工编排保留，继承句需重新生成。")); self->loadCandidates();
                return;
            }
            for (int i = 0; i < items.size(); ++i) {
                auto item = items[i].toObject(); if (!ids.contains(item.value(QStringLiteral("id")).toString())) continue;
                if (scope == QStringLiteral("unbound") && !item.value(QStringLiteral("voiceVersionId")).toString().isEmpty()) continue;
                if (item.value(QStringLiteral("voiceVersionId")).toString() != versionId) { item.insert(QStringLiteral("voiceVersionId"), versionId); item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1)); items[i] = item; }
            }
            next.insert(QStringLiteral("segments"), items); self->commit(next, QStringLiteral("项目声音版本已绑定，旧 Take 保留并标记失效。")); self->loadCandidates();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::restoreDefaultVoice()
{
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (item.value(QStringLiteral("id")).toString() != selectedId_ || item.value(QStringLiteral("voiceVersionId")).toString().isEmpty()) continue;
        item.insert(QStringLiteral("voiceVersionId"), QString());
        if (VoiceDocument::inputKey(document_, items[i].toObject()) != VoiceDocument::inputKey(document_, item))
            item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1));
        items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items);
    commit(next, QStringLiteral("当前句已恢复默认旁白；已有音频和编排保留。"));
}
int VoiceStudioController::inheritedVoiceCount() const
{
    int count = 0;
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray())
        if (value.toObject().value(QStringLiteral("voiceVersionId")).toString().isEmpty()) ++count;
    return count;
}
QVariantList VoiceStudioController::replacementRules() const { return document_.value(QStringLiteral("replacements")).toArray().toVariantList(); }
void VoiceStudioController::previewReplacements(const QVariantList &rules)
{
    auto next = document_; next.insert(QStringLiteral("replacements"), QJsonArray::fromVariantList(rules));
    auto items = next.value(QStringLiteral("segments")).toArray(); QVariantList differences;
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        const auto before = VoiceDocument::spokenText(document_, item), after = VoiceDocument::spokenText(next, item);
        if (before != after) { differences.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("朗读替换")}, {QStringLiteral("beforeText"), before}, {QStringLiteral("afterText"), after}});
            item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1)); items[i] = item; }
    }
    next.insert(QStringLiteral("segments"), items); propose(next, differences, QStringLiteral("替换仅影响未手工修正的朗读；逐条预览后应用。"));
}
void VoiceStudioController::previewNormalization()
{
    const auto item = itemFor(document_, selectedId_); if (item.isEmpty()) return;
    if (item.value(QStringLiteral("language")).toString() != QStringLiteral("Chinese")) { status_ = QStringLiteral("数字规范化建议目前仅支持中文；其它语言请填写手工朗读修正。"); emit changed(); return; }
    const auto before = VoiceDocument::spokenText(document_, item), after = VoiceDocument::normalizeChinese(before);
    propose(VoiceDocument::edit(document_, selectedId_, item.value(QStringLiteral("text")).toString(), after, true, item.value(QStringLiteral("notes")).toString()),
        {QVariantMap{{QStringLiteral("kind"), QStringLiteral("数字／日期／单位建议：确认后保存为单句手工朗读修正")}, {QStringLiteral("beforeText"), before}, {QStringLiteral("afterText"), after}}}, QStringLiteral("请核对年份、编号与专有名词，规范化不会改写正文。"));
}
void VoiceStudioController::previewSubtitleImport()
{
    auto next = document_; auto paragraphs = next.value(QStringLiteral("paragraphs")).toArray(), items = next.value(QStringLiteral("segments")).toArray();
    const auto references = session_->subtitleReferences(); if (references.isEmpty()) { status_ = QStringLiteral("当前项目没有可引用字幕。"); emit changed(); return; }
    const auto paragraph = QUuid::createUuid().toString(QUuid::WithoutBraces); paragraphs.append(QJsonObject{{QStringLiteral("id"), paragraph}}); QVariantList differences;
    for (const auto &value : references) {
        const auto reference = value.toObject(); auto item = VoiceDocument::segment(paragraph, reference.value(QStringLiteral("text")).toString());
        item.insert(QStringLiteral("subtitleSource"), reference); items.append(item);
        differences.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("新增字幕引用")}, {QStringLiteral("afterText"), reference.value(QStringLiteral("text")).toString()}});
    }
    next.insert(QStringLiteral("paragraphs"), paragraphs); next.insert(QStringLiteral("segments"), items);
    propose(next, differences, QStringLiteral("建立独立朗读与字幕来源快照，不反写字幕。"));
}
void VoiceStudioController::previewRoughCutImport()
{
    const auto sources = session_->roughCutReferences(); if (sources.isEmpty()) { status_ = QStringLiteral("当前粗剪没有可导入的已分析时间轴。"); emit changed(); return; }
    auto next = document_; auto paragraphs = next.value(QStringLiteral("paragraphs")).toArray(), items = next.value(QStringLiteral("segments")).toArray();
    const auto paragraph = QUuid::createUuid().toString(QUuid::WithoutBraces); paragraphs.append(QJsonObject{{QStringLiteral("id"), paragraph}}); QVariantList differences;
    for (int i = 0; i < sources.size(); ++i) {
        const auto source = sources[i].toObject(); auto item = VoiceDocument::segment(paragraph, source.value(QStringLiteral("text")).toString());
        item.insert(QStringLiteral("roughCutSource"), source); item.insert(QStringLiteral("pauseMs"), 0);
        if (i + 1 < sources.size()) { const auto following = sources[i + 1].toObject(); const auto end = source.value(QStringLiteral("sequenceStartSample")).toString().toLongLong() + source.value(QStringLiteral("sourceEndSample")).toString().toLongLong() - source.value(QStringLiteral("sourceStartSample")).toString().toLongLong();
            const auto pause = (following.value(QStringLiteral("sequenceStartSample")).toString().toLongLong() - end) * 1000.0 / source.value(QStringLiteral("sampleRate")).toInt();
            if (pause < 0 || pause > 60000) { status_ = QStringLiteral("粗剪顺序存在重叠或间隔超过 60 秒，请先调整原编排，未截断或部分导入。"); emit changed(); return; }
            item.insert(QStringLiteral("pauseMs"), pause); }
        items.append(item); differences.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("新增粗剪来源：保留顺序／原始范围，独立生成新配音")}, {QStringLiteral("afterText"), source.value(QStringLiteral("text")).toString()},
            {QStringLiteral("beforeTime"), source.value(QStringLiteral("sourceStartSample")).toString() + QStringLiteral("～") + source.value(QStringLiteral("sourceEndSample")).toString() + QStringLiteral(" 源样本")}, {QStringLiteral("afterTime"), QStringLiteral("采样率 %1；仅保存来源范围，不覆盖原决定或强制拉伸新音频").arg(source.value(QStringLiteral("sampleRate")).toInt())}});
    }
    next.insert(QStringLiteral("paragraphs"), paragraphs); next.insert(QStringLiteral("segments"), items); propose(next, differences, QStringLiteral("确认后追加独立文稿和来源映射；已有台词、Take、人工裁切及粗剪决定保持不变。"));
}
void VoiceStudioController::previewSubtitleSync(bool text, bool timing)
{
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray(); const auto current = session_->subtitleReferences(); QVariantList differences;
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); const auto previous = item.value(QStringLiteral("subtitleSource")).toObject(); if (previous.isEmpty()) continue;
        QJsonObject found;
        for (const auto &value : current) if (value.toObject().value(QStringLiteral("id")) == previous.value(QStringLiteral("id"))
            && previous.value(QStringLiteral("originProjectId")).toString() == session_->projectId()) { found = value.toObject(); break; }
        if (found.isEmpty()) {
            item.insert(QStringLiteral("sourceMissing"), true); items[i] = item;
            differences.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("来源已删除或拆合：保留独立内容与历史，需重新关联")}, {QStringLiteral("beforeText"), item.value(QStringLiteral("text")).toString()}}); continue;
        }
        if (found == previous) continue;
        auto synchronized = previous;
        if (text) {
            const auto updated = VoiceDocument::edit(next, item.value(QStringLiteral("id")).toString(), found.value(QStringLiteral("text")).toString(),
                item.value(QStringLiteral("spoken")).toString(), item.value(QStringLiteral("manualSpoken")).toBool(), item.value(QStringLiteral("notes")).toString());
            item = itemFor(updated, item.value(QStringLiteral("id")).toString()); synchronized.insert(QStringLiteral("text"), found.value(QStringLiteral("text")));
        }
        if (timing) { synchronized.insert(QStringLiteral("startUs"), found.value(QStringLiteral("startUs"))); synchronized.insert(QStringLiteral("endUs"), found.value(QStringLiteral("endUs"))); }
        synchronized.remove(QStringLiteral("sourceVersion"));
        synchronized.insert(QStringLiteral("sourceVersion"), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(synchronized).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        item.insert(QStringLiteral("subtitleSource"), synchronized); item.insert(QStringLiteral("sourceMissing"), false); items[i] = item;
        differences.append(QVariantMap{{QStringLiteral("kind"), timing && !text ? QStringLiteral("仅同步字幕时间，音频不失效") : QStringLiteral("字幕差异同步")},
            {QStringLiteral("beforeText"), itemFor(document_, item.value(QStringLiteral("id")).toString()).value(QStringLiteral("text")).toString()}, {QStringLiteral("afterText"), text ? found.value(QStringLiteral("text")).toString() : item.value(QStringLiteral("text")).toString()},
            {QStringLiteral("beforeTime"), QStringLiteral("%1～%2秒").arg(previous.value(QStringLiteral("startUs")).toString().toDouble() / 1000000, 0, 'f', 3).arg(previous.value(QStringLiteral("endUs")).toString().toDouble() / 1000000, 0, 'f', 3)},
            {QStringLiteral("afterTime"), QStringLiteral("%1～%2秒").arg(found.value(QStringLiteral("startUs")).toString().toDouble() / 1000000, 0, 'f', 3).arg(found.value(QStringLiteral("endUs")).toString().toDouble() / 1000000, 0, 'f', 3)},
            {QStringLiteral("manualSpokenProtected"), item.value(QStringLiteral("manualSpoken")).toBool()}});
    }
    next.insert(QStringLiteral("segments"), items); propose(next, differences, QStringLiteral("分别选择文本／时间同步；已手工修正的朗读不会被覆盖。"));
}
QVariantList VoiceStudioController::subtitleChoices() const { return session_->subtitleReferences().toVariantList(); }
void VoiceStudioController::previewSubtitleRelink(const QString &subtitleId)
{
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray(); QJsonObject source;
    for (const auto &value : session_->subtitleReferences()) if (value.toObject().value(QStringLiteral("id")).toString() == subtitleId) { source = value.toObject(); break; }
    if (source.isEmpty()) { status_ = QStringLiteral("字幕已不存在，请重新选择。"); emit changed(); return; }
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject(); if (item.value(QStringLiteral("id")).toString() != selectedId_) continue;
        item.insert(QStringLiteral("subtitleSource"), source); item.insert(QStringLiteral("sourceMissing"), false); items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items);
    propose(next, {QVariantMap{{QStringLiteral("kind"), QStringLiteral("重新关联来源：正文和手工朗读不改写，需要时另行同步")}, {QStringLiteral("afterText"), source.value(QStringLiteral("text")).toString()}}}, QStringLiteral("关联仅替换来源快照，不按文字猜配，不使音频失效。"));
}
void VoiceStudioController::moveSelected(int direction, bool paragraph)
{
    if (direction != -1 && direction != 1) return;
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray(); const auto selected = itemFor(next, selectedId_);
    if (paragraph) {
        auto paragraphs = next.value(QStringLiteral("paragraphs")).toArray();
        for (int i = 0; i < paragraphs.size(); ++i) if (paragraphs[i].toObject().value(QStringLiteral("id")) == selected.value(QStringLiteral("paragraphId"))) {
            const int target = i + direction; if (target < 0 || target >= paragraphs.size()) return;
            const auto first = paragraphs[i]; paragraphs[i] = paragraphs[target]; paragraphs[target] = first; break;
        }
        QJsonArray ordered;
        for (const auto &value : paragraphs) for (const auto &item : items) if (item.toObject().value(QStringLiteral("paragraphId")) == value.toObject().value(QStringLiteral("id"))) ordered.append(item);
        next.insert(QStringLiteral("paragraphs"), paragraphs); next.insert(QStringLiteral("segments"), ordered);
    } else {
        for (int i = 0; i < items.size(); ++i) if (items[i].toObject().value(QStringLiteral("id")).toString() == selectedId_) {
            const int target = i + direction;
            if (target < 0 || target >= items.size() || items[target].toObject().value(QStringLiteral("paragraphId")) != selected.value(QStringLiteral("paragraphId"))) return;
            const auto first = items[i]; items[i] = items[target]; items[target] = first; break;
        }
        next.insert(QStringLiteral("segments"), items);
    }
    commit(next, QStringLiteral("文稿顺序已调整，声音输入与人工裁切保留。"));
}
void VoiceStudioController::applySafeCandidates(bool replaceApplied)
{
    if (busy_ || session_->assetMutationBlocked() || session_->storagePath().isEmpty()) return;
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); auto *cancel = &cancel_;
    const auto path = session_->storagePath(); const auto frozen = document_; emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, path, frozen, replaceApplied] {
        QString error; QJsonArray ready;
        try {
            ProjectStore store(path);
            for (const auto &value : frozen.value(QStringLiteral("segments")).toArray()) {
                if (cancel->load()) break; const auto item = value.toObject();
                if (item.value(QStringLiteral("locked")).toBool()) continue;
                if ((!replaceApplied && !item.value(QStringLiteral("selectedTake")).toObject().isEmpty())
                    || item.value(QStringLiteral("selectedTake")).toObject().value(QStringLiteral("manualCrop")).toBool()) continue;
                bool found = false;
                for (int offset = 0; !found && !cancel->load(); offset += 50) {
                    const auto rows = store.segmentTakes(item.value(QStringLiteral("id")).toString(), offset, 50, &error); if (!error.isEmpty()) break;
                    for (const auto &row : rows) {
                        const auto take = candidateFor(row.toObject());
                        if (take.value(QStringLiteral("inputVersion")) == item.value(QStringLiteral("inputVersion"))
                            && QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()) == VoiceDocument::inputKey(frozen, item)) { ready.append(take); found = true; break; }
                    }
                    if (rows.size() < 50) break;
                }
                if (!error.isEmpty()) break;
            }
        } catch (...) { error = QStringLiteral("批量候选读取失败，未应用任何变更。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, ready, error, replaceApplied] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty()) { self->status_ = cancel->load() ? QStringLiteral("批量应用已取消。") : error; emit self->changed(); return; }
            auto next = self->document_; int applied = 0; QVariantList differences;
            for (const auto &value : ready) {
                const auto take = value.toObject(); const auto id = take.value(QStringLiteral("segmentId")).toString(); const auto item = itemFor(next, id);
                if (item.isEmpty() || item.value(QStringLiteral("locked")).toBool() || (!replaceApplied && !item.value(QStringLiteral("selectedTake")).toObject().isEmpty()) || item.value(QStringLiteral("selectedTake")).toObject().value(QStringLiteral("manualCrop")).toBool() || item.value(QStringLiteral("inputVersion")) != take.value(QStringLiteral("inputVersion"))) continue;
                QString problem; const auto after = VoiceDocument::applyTake(next, id, take, {}, &problem); if (problem.isEmpty() && after != next) {
                    differences.append(QVariantMap{{QStringLiteral("kind"), item.value(QStringLiteral("selectedTake")).toObject().isEmpty() ? QStringLiteral("补齐未应用句") : QStringLiteral("替换已应用版本")}, {QStringLiteral("afterText"), item.value(QStringLiteral("text")).toString()}});
                    next = after; ++applied;
                }
            }
            if (replaceApplied && applied > 0) { self->propose(next, differences, QStringLiteral("将更新 %1 句。人工裁切跳过，增益与停顿保留；确认后才替换。").arg(applied)); return; }
            self->commit(next, QStringLiteral("已补齐 %1 句；已应用版本、过期输入及人工编排保持不变。").arg(applied));
        }, Qt::QueuedConnection);
    });
}
QVariantList VoiceStudioController::archivedSegments(int offset) const
{
    QVariantList result; const auto items = document_.value(QStringLiteral("archivedSegments")).toArray();
    for (int i = std::max(0, offset); i < items.size() && result.size() < 50; ++i) result.append(items[i].toVariant()); return result;
}
void VoiceStudioController::loadCandidates(int offset, const QString &segmentId)
{
    if (busy_ || session_->assetMutationBlocked() || (selectedId_.isEmpty() && segmentId.isEmpty()) || session_->storagePath().isEmpty()) return;
    if (!segmentId.isEmpty() && itemFor(document_, segmentId).isEmpty()) {
        bool found = false;
        for (const auto &value : document_.value(QStringLiteral("archivedSegments")).toArray()) if (value.toObject().value(QStringLiteral("id")).toString() == segmentId) { found = true; break; }
        if (!found) { status_ = QStringLiteral("该句不属于当前文稿或归档。"); emit changed(); return; }
    }
    if (candidateCancel_) *candidateCancel_ = true;
    candidateCancel_ = std::make_shared<std::atomic<bool>>(false); const auto cancel = candidateCancel_;
    candidatesLoading_ = true; const auto epoch = ++candidateEpoch_; const QPointer<VoiceStudioController> self(this); emit candidatesChanged();
    QStringList versions;
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray()) { const auto version = VoiceDocument::effectiveVoiceVersion(document_, value.toObject()); if (!version.isEmpty() && !versions.contains(version)) versions.append(version); }
    const auto names = document_.value(QStringLiteral("voiceNames")).toObject(); const auto frozen = document_;
    const auto path = session_->storagePath(), id = segmentId.isEmpty() ? selectedId_ : segmentId; emit changed();
    candidateTask_ = context_->submitProjectStorage([self, epoch, cancel, path, id, offset, versions, names, frozen, archived = !segmentId.isEmpty()] {
        if (cancel->load()) return;
        QString error; QVariantList candidates, voices;
        try {
            for (const auto &value : ProjectStore(path).segmentTakes(id, offset, 50, &error)) {
                candidates.append(candidateFor(value.toObject()).toVariantMap());
            }
        } catch (...) { error = QStringLiteral("候选读取失败，请检查项目数据库。"); }
        if (!cancel->load() && error.isEmpty()) {
            for (const auto &value : ProjectStore(path).voiceBindings(versions, &error)) {
                auto row = value.toObject(); row.insert(QStringLiteral("name"), names.value(row.value(QStringLiteral("versionId")).toString()).toString(QStringLiteral("项目音色"))); voices.append(row.toVariantMap());
            }
        }
        const auto states = cancel->load() || !error.isEmpty() ? QJsonObject{} : ProjectStore(path).voiceGenerationStates(frozen, &error, cancel.get());
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, id, candidates, voices, states, error, offset, archived] {
            if (!self || self->candidateEpoch_ != epoch) return; self->candidatesLoading_ = false; emit self->candidatesChanged();
            if (cancel->load()) { emit self->changed(); return; }
            if (!archived && id != self->selectedId_) { emit self->changed(); self->loadCandidates(); return; }
            self->candidateOffset_ = std::max(0, offset);
            self->projectVoices_ = error.isEmpty() ? voices : QVariantList{}; self->candidates_ = error.isEmpty() ? candidates : QVariantList{}; self->status_ = error.isEmpty() ? QStringLiteral("候选已读取；试听不改变文稿，应用需检查输入和人工裁切。") : error;
            self->generationStates_ = error.isEmpty() ? states : QJsonObject{};
            if (self->rowCount() > 0) emit self->dataChanged(self->index(0), self->index(self->rowCount() - 1), {GenerationState});
            emit self->candidatesChanged(); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::applyCandidate(const QString &takeId, const QString &cropPolicy)
{
    for (const auto &value : candidates_) {
        const auto take = value.toMap(); if (take.value(QStringLiteral("id")).toString() != takeId || take.value(QStringLiteral("segmentId")).toString() != selectedId_) continue;
        if (take.value(QStringLiteral("inputVersion")) != selected().value(QStringLiteral("inputVersion"))) { status_ = QStringLiteral("候选输入版本已过期，保留历史，不自动替换。"); emit changed(); return; }
        QString error; const auto next = VoiceDocument::applyTake(document_, selectedId_, QJsonObject::fromVariantMap(take), cropPolicy, &error);
        if (!error.isEmpty()) { status_ = error; emit changed(); return; }
        commit(next, QStringLiteral("候选已应用，增益、停顿与文稿顺序保留。")); return;
    }
}
bool VoiceStudioController::candidateIsCurrent(const QString &takeId) const
{
    const auto item = itemFor(document_, selectedId_);
    for (const auto &value : candidates_) {
        const auto take = value.toMap();
        if (take.value(QStringLiteral("id")).toString() == takeId)
            return take.value(QStringLiteral("segmentId")).toString() == selectedId_ && take.value(QStringLiteral("inputVersion")).toString() == item.value(QStringLiteral("inputVersion")).toString()
                && QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1()) == VoiceDocument::inputKey(document_, item);
    }
    return false;
}
QVariantMap VoiceStudioController::exportSummary() const
{
    int missing = 0, stale = 0; QVariantList problems;
    for (const auto &value : document_.value(QStringLiteral("segments")).toArray()) {
        const auto item = value.toObject(); const auto take = item.value(QStringLiteral("selectedTake")).toObject();
        QString reason;
        if (take.isEmpty()) { ++missing; reason = QStringLiteral("未应用音频，请生成或试听并应用候选"); }
        else if (VoiceDocument::inputKey(document_, item) != QByteArray::fromHex(take.value(QStringLiteral("inputKey")).toString().toLatin1())) {
            ++stale; reason = QStringLiteral("已应用音频失效，需要重新生成或明确使用旧音频");
        }
        if (!reason.isEmpty()) problems.append(QVariantMap{{QStringLiteral("id"), item.value(QStringLiteral("id")).toString()},
            {QStringLiteral("text"), item.value(QStringLiteral("text")).toString()}, {QStringLiteral("reason"), reason}});
    }
    return {{QStringLiteral("missing"), missing}, {QStringLiteral("stale"), stale}, {QStringLiteral("problems"), problems},
        {QStringLiteral("total"), rowCount()}};
}
QVariantMap VoiceStudioController::engineCapabilities(const QString &engine) const
{
    const auto capabilities = engine == QStringLiteral("cloud") ? CloudTtsProvider({}, {}, 1).capabilities()
        : LocalTtsProvider(engine == QStringLiteral("mock") ? LocalTtsProvider::Kind::Mock
            : engine == QStringLiteral("index") ? LocalTtsProvider::Kind::Index25
            : engine == QStringLiteral("gpt") ? LocalTtsProvider::Kind::GptProPlus : LocalTtsProvider::Kind::QwenBase, {}, {}).capabilities();
    return {{QStringLiteral("languages"), capabilities.languages}, {QStringLiteral("parameters"), capabilities.parameters},
        {QStringLiteral("referenceRequired"), capabilities.referenceRequired}, {QStringLiteral("transcriptRequired"), capabilities.transcriptRequired}};
}
QVariantMap VoiceStudioController::generationReadiness(const QString &engine, const QString &python, const QString &directory) const
{
    const auto result = [](const QString &state, const QString &text, const QString &action) {
        return QVariantMap{{QStringLiteral("state"), state}, {QStringLiteral("text"), text}, {QStringLiteral("action"), action}};
    };
    if (session_->assetMutationBlocked()) return result(QStringLiteral("waiting"), QStringLiteral("等待保存或资产迁移完成"), {});
    if (busy_ || candidatesLoading_ || (tasks_ && tasks_->busy())) return result(QStringLiteral("running"), QStringLiteral("任务正在处理，可切换模块继续其他工作"), {});
    if (selectedId_.isEmpty()) return result(QStringLiteral("text"), QStringLiteral("先添加文稿，再选择音色并生成当前句"), QStringLiteral("text"));
    for (const auto &key : itemFor(document_, selectedId_).value(QStringLiteral("parameters")).toObject().keys())
        if (!engineCapabilities(engine).value(QStringLiteral("parameters")).toStringList().contains(key))
            return result(QStringLiteral("parameters"), QStringLiteral("当前句保留了其它引擎的参数，请先清除；可撤销恢复原参数。"), QStringLiteral("parameters"));
    if (engine != QStringLiteral("mock") && engine != QStringLiteral("cloud") && VoiceDocument::effectiveVoiceVersion(document_, itemFor(document_, selectedId_)).isEmpty())
        return result(QStringLiteral("voice"), QStringLiteral("为工程选择默认旁白，或为当前句指定音色"), QStringLiteral("voice"));
    if (engine == QStringLiteral("cloud")) {
        QString key, error;
        const auto binding = context_->cloudConfig->resolveSynthesis(&key, &error);
        if (binding.isEmpty())
            return result(QStringLiteral("configuration"), error, QStringLiteral("settings"));
        if (selected().value(QStringLiteral("effectiveVoiceVersionId")).toString().isEmpty())
            return result(QStringLiteral("voice"), QStringLiteral("账号已连接，请从左栏选择当前模型音色，无需填写 Voice ID。"), QStringLiteral("voice"));
        for (const auto &value : projectVoices_) {
            const auto voice = value.toMap();
            if (voice.value(QStringLiteral("versionId")).toString() != selected().value(QStringLiteral("effectiveVoiceVersionId")).toString()) continue;
            for (const auto &field : {QStringLiteral("cloudAccountId"), QStringLiteral("model"), QStringLiteral("region"), QStringLiteral("businessSpace")})
                if (voice.value(field) != binding.value(field).toVariant()) return result(QStringLiteral("voice"), QStringLiteral("当前句音色与账号或型号不匹配；请重新选音色或切回原连接。旧音频保留。"), QStringLiteral("voice"));
            return result(QStringLiteral("ready"), QStringLiteral("先预览发送文本，再确认云端生成"), {});
        }
        return result(QStringLiteral("voice"), QStringLiteral("当前句使用本地参考，不能直接用于云端；请从左栏选用当前账号音色。"), QStringLiteral("voice"));
    }
    if (engine != QStringLiteral("mock")) for (const auto &value : projectVoices_)
        if (value.toMap().value(QStringLiteral("versionId")) == selected().value(QStringLiteral("effectiveVoiceVersionId")))
            return result(QStringLiteral("voice"), QStringLiteral("当前句是云端音色；切回云端或选择已审核的本地参考，不能跨引擎冒用。"), QStringLiteral("voice"));
    if (python.trimmed().isEmpty() || (engine != QStringLiteral("mock") && directory.trimmed().isEmpty()))
        return result(QStringLiteral("configuration"), QStringLiteral("当前引擎尚未配置独立 Python 或模型目录"), QStringLiteral("settings"));
    if (engine != QStringLiteral("mock")) {
        const auto kind = engine == QStringLiteral("index") ? LocalTtsProvider::Kind::Index25 : engine == QStringLiteral("gpt") ? LocalTtsProvider::Kind::GptProPlus : LocalTtsProvider::Kind::QwenBase;
        if (python != qwenPython_ || directory != qwenDirectory_ || qwenStatus_.value(QStringLiteral("provider")).toString() != LocalTtsProvider(kind, python, workerPath(), directory).capabilities().provider)
            return result(QStringLiteral("environment"), QStringLiteral("当前环境尚未验证，请先检查；参考音频会在生成前再次校验"), QStringLiteral("check"));
    }
    return result(QStringLiteral("ready"), engine == QStringLiteral("mock") ? QStringLiteral("可以生成测试音；测试音不代表真实模型音质") : QStringLiteral("可以生成当前句；已有候选请先试听，批量补缺会复用有效候选"), {});
}
void VoiceStudioController::clearUnsupportedParameters(const QString &engine)
{
    if (busy_ || session_->assetMutationBlocked() || selectedId_.isEmpty()
        || !QStringList{QStringLiteral("qwen"), QStringLiteral("index"), QStringLiteral("gpt"), QStringLiteral("cloud"), QStringLiteral("mock")}.contains(engine)) return;
    auto next = document_; auto items = next.value(QStringLiteral("segments")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        if (item.value(QStringLiteral("id")).toString() != selectedId_ || item.value(QStringLiteral("locked")).toBool()) continue;
        auto parameters = item.value(QStringLiteral("parameters")).toObject();
        for (const auto &key : parameters.keys())
            if (!engineCapabilities(engine).value(QStringLiteral("parameters")).toStringList().contains(key)) parameters.remove(key);
        item.insert(QStringLiteral("parameters"), parameters);
        if (VoiceDocument::inputKey(document_, items[i].toObject()) != VoiceDocument::inputKey(next, item))
            item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1));
        items[i] = item;
    }
    next.insert(QStringLiteral("segments"), items);
    commit(next, QStringLiteral("当前句不兼容参数已清除；支持的参数、旧音频和编排保留，可撤销恢复。"));
}
void VoiceStudioController::auditionSelectedTake()
{
    if (busy_ || session_->assetMutationBlocked() || session_->storagePath().isEmpty()) return;
    const auto item = itemFor(document_, selectedId_); const auto take = item.value(QStringLiteral("selectedTake")).toObject(); if (take.isEmpty()) return;
    busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this);
    const auto path = session_->storagePath(), id = selectedId_, takeId = take.value(QStringLiteral("id")).toString(); emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, path, id, takeId] {
        QString error; QJsonObject found; ProjectStore store(path);
        for (int offset = 0; !cancel->load(); offset += 50) {
            const auto rows = store.segmentTakes(id, offset, 50, &error);
            for (const auto &value : rows) if (value.toObject().value(QStringLiteral("id")).toString() == takeId) { found = candidateFor(value.toObject()); break; }
            if (!error.isEmpty() || !found.isEmpty() || rows.size() < 50) break;
        }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, id, takeId, found, error] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (!cancel->load() && self->selectedId_ == id && itemFor(self->document_, id).value(QStringLiteral("selectedTake")).toObject().value(QStringLiteral("id")).toString() == takeId) {
                if (found.isEmpty()) self->status_ = error.isEmpty() ? QStringLiteral("选中 Take 记录缺失，不能试听。") : error;
                else emit self->auditionRequested(found.value(QStringLiteral("path")).toString(), found.value(QStringLiteral("sha256")).toString());
            }
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::generateMock(const QString &python, const QString &scope, bool review)
{
    if (!tasks_ || busy_ || session_->assetMutationBlocked() || tasks_->busy()) return;
    if (!QStringList{QStringLiteral("selected"), QStringLiteral("paragraph"), QStringLiteral("all"), QStringLiteral("invalid"), QStringLiteral("selection")}.contains(scope)) { status_ = QStringLiteral("生成范围无效。"); emit changed(); return; }
    if (!QFileInfo::exists(python)) { status_ = QStringLiteral("请选择已配置的独立 Python。Mock 只产生测试音，不是真实配音。"); emit changed(); return; }
    const auto selection = selectedIds_; const auto frozen = document_; const auto selectedId = selectedId_, path = session_->storagePath();
    const auto projectId = session_->projectId(); const auto generation = session_->generation(); const auto worker = workerPath();
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; auto *cancel = &cancel_; const QPointer<VoiceStudioController> self(this); emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, frozen, selection, selectedId, path, projectId, generation, worker, python, scope, review] {
        QString error; ProjectStore store(path);
        int reused = 0; QVector<TtsJob> jobs; const auto selected = itemFor(frozen, selectedId);
        for (const auto &value : frozen.value(QStringLiteral("segments")).toArray()) {
            const auto item = value.toObject(); const auto id = item.value(QStringLiteral("id")).toString();
            if (item.value(QStringLiteral("locked")).toBool() || (scope == QStringLiteral("selection") && !selection.contains(id))) continue;
            if ((scope == QStringLiteral("selected") && id != selectedId) || (scope == QStringLiteral("paragraph") && item.value(QStringLiteral("paragraphId")) != selected.value(QStringLiteral("paragraphId")))) continue;
            if (scope == QStringLiteral("invalid") && hasCurrentCandidate(store, frozen, item, &error, cancel)) { ++reused; continue; }
            if (!error.isEmpty() || cancel->load()) break;
            if (item.value(QStringLiteral("provider")).toString() != QStringLiteral("mock")) { error = QStringLiteral("当前句绑定真实引擎，不能隐式改成 Mock。"); break; }
            TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            job.input.projectId = projectId; job.input.projectGeneration = generation; job.input.segmentId = id;
            job.input.inputVersion = item.value(QStringLiteral("inputVersion")).toString().toLongLong(); job.input.spokenText = VoiceDocument::spokenText(frozen, item);
            job.input.voiceVersionId = VoiceDocument::effectiveVoiceVersion(frozen, item); job.input.effectiveParameters = item.value(QStringLiteral("parameters")).toObject();
            job.input.language = item.value(QStringLiteral("language")).toString(); job.input.provider = item.value(QStringLiteral("provider")).toString();
            job.input.model = item.value(QStringLiteral("model")).toString(); job.input.modelVersion = item.value(QStringLiteral("modelVersion")).toString(); jobs.append(job);
        }

        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, frozen, jobs, error, python, worker, review, reused] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty() || self->document_ != frozen) {
                self->status_ = cancel->load() ? QStringLiteral("生成准备已取消。") : !error.isEmpty() ? error : QStringLiteral("文稿已修改，请重新生成。"); emit self->changed(); return;
            }
            if (jobs.isEmpty()) { self->status_ = QStringLiteral("已有有效音频或候选，无需重复生成；可试听后应用。"); emit self->changed(); return; }
            const auto launch = [self, frozen, jobs, python, worker, review] {
                if (!self) return;
                if (self->document_ != frozen || (self->session_->busy() || (review && self->session_->savePending())) || !self->tasks_ || self->tasks_->busy()) {
                    self->status_ = QStringLiteral("文稿或任务状态已变化，请重新检查生成范围。"); emit self->changed(); return;
                }
                const bool success = self->tasks_->start(LocalTtsProvider(LocalTtsProvider::Kind::Mock, python, worker), jobs);
                self->status_ = success ? QStringLiteral("Mock 批次已启动，测试音不会作为真实配音验收。") : self->tasks_->statusText(); emit self->changed();
            };
            if (review) {
                self->pendingGeneration_ = launch;
                self->generationPreview_ = {{QStringLiteral("needed"), jobs.size()}, {QStringLiteral("reused"), reused}, {QStringLiteral("blocked"), 0}};
                emit self->generationPreviewChanged();
            } else launch();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::discardGeneration()
{
    pendingGeneration_ = {}; generationPreview_.clear(); emit generationPreviewChanged();
}
void VoiceStudioController::confirmGeneration()
{
    const auto launch = std::exchange(pendingGeneration_, {});
    generationPreview_.clear(); emit generationPreviewChanged();
    if (launch) launch();
}
QString VoiceStudioController::workerPath() const
{
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("inference/tts_python_worker.py"));
    return QFileInfo::exists(bundled) ? bundled : QDir(QString::fromUtf8(SUBCUE_PROJECT_ROOT)).filePath(QStringLiteral("tools/tts_python_worker.py"));
}
void VoiceStudioController::checkLocal(const QString &engine, const QString &python, const QString &directory)
{
    if (busy_ || (tasks_ && tasks_->busy())) return;
    if (!QStringList{QStringLiteral("qwen"), QStringLiteral("index"), QStringLiteral("gpt")}.contains(engine)) return;
    const auto kind = engine == QStringLiteral("index") ? LocalTtsProvider::Kind::Index25 : engine == QStringLiteral("gpt") ? LocalTtsProvider::Kind::GptProPlus : LocalTtsProvider::Kind::QwenBase;
    busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); const auto worker = workerPath();
    qwenStatus_ = {}; qwenPython_.clear(); qwenDirectory_.clear(); status_ = QStringLiteral("离线预检环境与完整模型指纹，不加载权重、不下载依赖。"); emit changed();
    task_ = context_->submitAudioProcessing([self, epoch, cancel, python, directory, worker, kind] {
        QString error; QJsonObject checked;
        try { checked = LocalTtsProvider(kind, python, worker, directory).check(&error, cancel); } catch (...) { error = QStringLiteral("本地预检发生异常，请检查环境和模型文件。"); }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, python, directory, checked, error] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty() || checked.isEmpty()) self->status_ = cancel->load() ? QStringLiteral("Qwen 预检已取消。"): error;
            else { self->qwenStatus_ = checked; self->qwenPython_ = python; self->qwenDirectory_ = directory; self->status_ = QStringLiteral("本地引擎预检通过：%1；尚未验证真实生成和音质。").arg(checked.value(QStringLiteral("device")).toString()); }
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::generateLocal(const QString &engine, const QString &python, const QString &directory, const QString &scope, bool review)
{
    if (!tasks_ || busy_ || session_->assetMutationBlocked() || tasks_->busy()) return;
    if (!QStringList{QStringLiteral("qwen"), QStringLiteral("index"), QStringLiteral("gpt")}.contains(engine)) return;
    const auto kind = engine == QStringLiteral("index") ? LocalTtsProvider::Kind::Index25 : engine == QStringLiteral("gpt") ? LocalTtsProvider::Kind::GptProPlus : LocalTtsProvider::Kind::QwenBase;
    const auto capabilities = LocalTtsProvider(kind, python, workerPath(), directory).capabilities();
    if (!session_->unified() || session_->storagePath().isEmpty() || python != qwenPython_ || directory != qwenDirectory_ || qwenStatus_.value(QStringLiteral("provider")).toString() != capabilities.provider) { status_ = QStringLiteral("请在引擎配置中完成当前引擎、Python 和模型路径的预检，再生成配音。"); emit changed(); return; }
    if (!QStringList{QStringLiteral("selected"), QStringLiteral("paragraph"), QStringLiteral("all"), QStringLiteral("invalid"), QStringLiteral("selection")}.contains(scope)) return;
    const auto selection = selectedIds_; const auto before = document_, selected = itemFor(document_, selectedId_); const auto path = session_->storagePath(), version = qwenStatus_.value(QStringLiteral("modelVersion")).toString();
    const auto projectId = session_->projectId(); const auto generation = session_->generation(); const auto worker = workerPath();
    busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); status_ = QStringLiteral("校验项目声音快照，冻结所选语言生成输入…"); emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, before, selection, selected, scope, path, version, projectId, generation, python, directory, worker, kind, capabilities, review] {
        QString error; auto next = before; auto items = next.value(QStringLiteral("segments")).toArray(); QVector<TtsJob> jobs; int reused = 0;
        try {
            ProjectStore store(path);
            for (int i = 0; i < items.size() && !cancel->load(); ++i) {
                auto item = items[i].toObject(); const auto id = item.value(QStringLiteral("id")).toString();
                if (item.value(QStringLiteral("locked")).toBool() || (scope == QStringLiteral("selection") && !selection.contains(item.value(QStringLiteral("id")).toString()))) continue;
                if ((scope == QStringLiteral("selected") && item.value(QStringLiteral("id")) != selected.value(QStringLiteral("id"))) || (scope == QStringLiteral("paragraph") && item.value(QStringLiteral("paragraphId")) != selected.value(QStringLiteral("paragraphId")))) continue;
                const auto oldKey = VoiceDocument::inputKey(before, item);
                if (!capabilities.languages.contains(item.value(QStringLiteral("language")).toString())) { error = QStringLiteral("当前引擎尚未接入所选目标语言；中英日跨语言请使用 Qwen Base 或 Index 2.5。"); break; }
                item.insert(QStringLiteral("provider"), capabilities.provider); item.insert(QStringLiteral("model"), capabilities.model); item.insert(QStringLiteral("modelVersion"), version);
                const auto key = VoiceDocument::inputKey(next, item);
                if (oldKey != key) item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1)); items[i] = item;
                if (scope == QStringLiteral("invalid") && hasCurrentCandidate(store, next, item, &error, cancel)) { ++reused; continue; }
                if (!error.isEmpty()) break;
                const auto voice = store.voiceVersion(VoiceDocument::effectiveVoiceVersion(next, item), &error);
                if (!voice || voice->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("qualityState")).toString() != QStringLiteral("approved")) { if (error.isEmpty()) error = QStringLiteral("每句需绑定本项目已审核的本地参考版本；云 Voice ID 不能用于本地克隆。"); break; }
                const auto metadata = voice->value(QStringLiteral("metadata")).toObject();
                TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.input.projectId = projectId; job.input.projectGeneration = generation;
                job.input.segmentId = id; job.input.inputVersion = item.value(QStringLiteral("inputVersion")).toString().toLongLong(); job.input.spokenText = VoiceDocument::spokenText(next, item);
                job.input.provider = capabilities.provider; job.input.model = item.value(QStringLiteral("model")).toString(); job.input.modelVersion = version; job.input.language = item.value(QStringLiteral("language")).toString();
                job.input.voiceVersionId = VoiceDocument::effectiveVoiceVersion(next, item); job.input.referenceAssetId = voice->value(QStringLiteral("referenceAssetId")).toString();
                job.input.referenceSha256 = QByteArray::fromHex(voice->value(QStringLiteral("referenceSha256")).toString().toLatin1()); job.input.referenceText = metadata.value(QStringLiteral("referenceText")).toString(); job.input.effectiveParameters = item.value(QStringLiteral("parameters")).toObject();
                if (!capabilities.transcriptRequired) job.input.referenceText.clear();
                job.input.referenceLanguage = metadata.value(QStringLiteral("referenceLanguage")).toString(QStringLiteral("Chinese"));
                if (!job.input.validate(capabilities, &error)) break; jobs.append(job);
            }
        } catch (...) { error = QStringLiteral("声音快照校验异常，未派发生成。"); }
        next.insert(QStringLiteral("segments"), items);
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, before, next, jobs, python, directory, worker, error, kind, review, reused] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty() || self->document_ != before) { self->status_ = cancel->load() ? QStringLiteral("生成准备已取消。") : !error.isEmpty() ? error : QStringLiteral("校验期间编辑有变化，请重新生成。"); emit self->changed(); return; }
            if (jobs.isEmpty() || jobs.size() > 500) { self->status_ = jobs.isEmpty() ? QStringLiteral("该范围没有需要生成的句子。") : QStringLiteral("单批最多 500 句，请按段生成。"); emit self->changed(); return; }
            const auto launch = [self, before, next, jobs, python, worker, directory, kind, review] {
                if (!self) return;
                if (self->document_ != before || (self->session_->busy() || (review && self->session_->savePending())) || !self->tasks_ || self->tasks_->busy()) {
                    self->status_ = QStringLiteral("文稿或任务状态已变化，请重新检查生成范围。"); emit self->changed(); return;
                }
                if (!self->commit(next, QStringLiteral("生成条件已冻结，旧候选和人工编排保留。"))) return;
                const bool started = self->tasks_->start(LocalTtsProvider(kind, python, worker, directory), jobs);
                self->status_ = started ? QStringLiteral("真实本地批次已启动，新结果只进入候选，不自动覆盖编辑。") : self->tasks_->statusText(); emit self->changed();
            };
            if (review) {
                self->pendingGeneration_ = launch;
                self->generationPreview_ = {{QStringLiteral("needed"), jobs.size()}, {QStringLiteral("reused"), reused}, {QStringLiteral("blocked"), 0}};
                emit self->generationPreviewChanged();
            } else launch();
        }, Qt::QueuedConnection);
    });
}
bool VoiceStudioController::saveFlashCredential(const QString &reference, const QString &key)
{
    Q_UNUSED(reference); Q_UNUSED(key);
    status_ = QStringLiteral("旧配音密钥入口已停用，请在 AI 云端与模型中管理账号。"); emit changed(); return false;
}
bool VoiceStudioController::flashCredentialSaved(const QString &reference) const
{
    Q_UNUSED(reference);
    QString key, error; return !context_->cloudConfig->resolveSynthesis(&key, &error).isEmpty();
}
void VoiceStudioController::testFlashConnection(const QString &workspace, const QString &reference, const QString &draftKey, bool ignoreSaved)
{
    Q_UNUSED(workspace); Q_UNUSED(reference); Q_UNUSED(draftKey); Q_UNUSED(ignoreSaved);
    status_ = QStringLiteral("旧配音测试入口已停用，请在 AI 服务中测试统一账号。");
    flashConnection_ = {{QStringLiteral("state"), QStringLiteral("failed")}, {QStringLiteral("detail"), status_}};
    emit changed();
}
void VoiceStudioController::prepareFlash(const QString &scope, double budget, bool regenerate)
{
    if (busy_ || session_->assetMutationBlocked() || !tasks_ || tasks_->busy()) return;
    flashPreview_ = {}; flashJobs_.clear();
    if (!session_->unified() || session_->storagePath().isEmpty() || !std::isfinite(budget) || budget <= 0
        || !QStringList{QStringLiteral("selected"), QStringLiteral("paragraph"), QStringLiteral("all"), QStringLiteral("invalid"), QStringLiteral("selection")}.contains(scope)) {
        status_ = QStringLiteral("请在统一工作区填写大于 0 的人民币软预算，并选择生成范围。"); emit changed(); return;
    }
    QString configKey, configError;
    const auto cloud = context_->cloudConfig->resolveSynthesis(&configKey, &configError);
    if (cloud.isEmpty()) { status_ = configError; emit changed(); return; }
    const auto selection = selectedIds_; const auto before = document_, selected = itemFor(document_, selectedId_); const auto path = session_->storagePath();
    const auto projectId = session_->projectId(); const auto generation = session_->generation();
    const auto revisionDirectory = QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-revisions"));
    busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); emit changed();
    task_ = context_->submitProjectStorage([self, epoch, cancel, before, selection, selected, scope, path, projectId, generation, budget, regenerate, cloud, revisionDirectory] {
        QString error; const auto workspace = cloud.value(QStringLiteral("businessSpace")).toString(); auto next = before; auto items = next.value(QStringLiteral("segments")).toArray(); QVector<TtsJob> jobs; QJsonArray rows, history;
        bool unknownUsage = false; double reusedAmount = 0; int reused = 0; QJsonObject voiceRevisions;
        try {
            ProjectStore store(path);
            for (int offset = 0; offset < 500 && !cancel->load(); offset += 200) { const auto page = store.jobs(offset, std::min(200, 500 - offset), &error); for (const auto &row : page) history.append(row); if (page.size() < 200 || !error.isEmpty()) break; }
            for (int i = 0; i < items.size() && error.isEmpty() && !cancel->load(); ++i) {
                auto item = items[i].toObject(); const auto id = item.value(QStringLiteral("id")).toString();
                if (item.value(QStringLiteral("locked")).toBool() || (scope == QStringLiteral("selection") && !selection.contains(item.value(QStringLiteral("id")).toString()))) continue;
                if ((scope == QStringLiteral("selected") && item.value(QStringLiteral("id")) != selected.value(QStringLiteral("id")))
                    || (scope == QStringLiteral("paragraph") && item.value(QStringLiteral("paragraphId")) != selected.value(QStringLiteral("paragraphId")))) continue;
                const auto version = store.voiceVersion(VoiceDocument::effectiveVoiceVersion(next, item), &error);
                const auto binding = version ? version->value(QStringLiteral("metadata")).toObject() : QJsonObject{};
                if (!version || binding.value(QStringLiteral("provider")) != QLatin1String("qwen-cloud")
                    || binding.value(QStringLiteral("cloudAccountId")) != cloud.value(QStringLiteral("cloudAccountId"))
                    || binding.value(QStringLiteral("model")) != cloud.value(QStringLiteral("model"))
                    || binding.value(QStringLiteral("region")) != cloud.value(QStringLiteral("region"))
                    || binding.value(QStringLiteral("businessSpace")) != cloud.value(QStringLiteral("businessSpace"))) {
                    if (error.isEmpty()) error = QStringLiteral("音色与当前账号、地域、模型或 Workspace 不匹配。请在声音库明确关联当前云账号，再使用该新版本；已有音频保留。");
                    break;
                }
                const auto revision = CloudTtsProvider::voiceRevision(revisionDirectory, binding, &error);
                if (!revision || binding.value(QStringLiteral("cloudVoiceRevision")).toString() != *revision) { if (error.isEmpty()) error = QStringLiteral("云端音色已更新，请查询并重新选用声音版本；已有音频保留。"); break; }
                voiceRevisions.insert(version->value(QStringLiteral("id")).toString(), binding);
                const auto old = VoiceDocument::inputKey(before, item);
                item.insert(QStringLiteral("provider"), QStringLiteral("qwen-cloud")); item.insert(QStringLiteral("model"), QStringLiteral("qwen-audio-3.1-tts-flash")); item.insert(QStringLiteral("modelVersion"), QStringLiteral("http-wav24k-20261014"));
                if (item.value(QStringLiteral("language")).toString() != QStringLiteral("Chinese")) { error = QStringLiteral("首版云端仅验收中文。"); break; }
                const auto key = VoiceDocument::inputKey(next, item);
                if (scope == QStringLiteral("invalid") && hasCurrentCandidate(store, next, item, &error, cancel)) { ++reused; continue; }
                if (old != key) item.insert(QStringLiteral("inputVersion"), QString::number(item.value(QStringLiteral("inputVersion")).toString().toLongLong() + 1)); items[i] = item;
                TtsJob job; job.id = QUuid::createUuid().toString(QUuid::WithoutBraces); job.input.projectId = projectId; job.input.projectGeneration = generation; job.input.segmentId = id;
                job.input.inputVersion = item.value(QStringLiteral("inputVersion")).toString().toLongLong(); job.input.spokenText = VoiceDocument::spokenText(next, item); job.input.language = QStringLiteral("Chinese");
                job.input.provider = QStringLiteral("qwen-cloud"); job.input.model = item.value(QStringLiteral("model")).toString(); job.input.modelVersion = item.value(QStringLiteral("modelVersion")).toString();
                job.input.voiceVersionId = VoiceDocument::effectiveVoiceVersion(next, item); job.input.providerVoiceId = binding.value(QStringLiteral("providerVoiceId")).toString(); job.input.region = QStringLiteral("beijing"); job.input.effectiveParameters = item.value(QStringLiteral("parameters")).toObject();
                if (!job.input.validate(CloudTtsProvider(workspace, {}, budget).capabilities(), &error)) break;
                if (job.input.spokenText.size() > 4096 || job.input.effectiveParameters.value(QStringLiteral("seed")).toInteger(0) > 65535) { error = QStringLiteral("Flash 每句最多 4096 字符，seed 上限 65535；请调整后重新预览。"); break; }
                bool existing = false;
                if (!regenerate && hasCurrentCandidate(store, next, item, &error, cancel)) for (const auto &value : history) { const auto previous = value.toObject(); if (previous.value(QStringLiteral("state")).toString() == QStringLiteral("completed") && previous.value(QStringLiteral("inputHash")).toString() == QString::fromLatin1(job.input.inputHash().toHex())) {
                    existing = true; const auto amount = TtsPricing::amount(previous.value(QStringLiteral("usage")).toObject(), previous.value(QStringLiteral("usage")).toObject().value(QStringLiteral("price")).toObject()); if (amount) reusedAmount += *amount; else unknownUsage = true; break; } }
                if (existing) { ++reused; continue; }
                job.billing = {{QStringLiteral("price"), TtsPricing::flashPrice()}, {QStringLiteral("businessSpace"), workspace}, {QStringLiteral("cloudAccountId"), cloud.value(QStringLiteral("cloudAccountId"))}};
                job.billing.insert(QStringLiteral("manualRetry"), regenerate);
                rows.append(QJsonObject{{QStringLiteral("text"), job.input.spokenText}, {QStringLiteral("voice"), job.input.providerVoiceId}}); jobs.append(job);
            }
        } catch (...) { error = QStringLiteral("云端任务准备失败，请检查工程和声音绑定。"); }
        next.insert(QStringLiteral("segments"), items);
        const QJsonObject preview{{QStringLiteral("rows"), rows}, {QStringLiteral("unknownUsage"), unknownUsage}, {QStringLiteral("voiceRevisions"), voiceRevisions},
            {QStringLiteral("reused"), reused}, {QStringLiteral("reusedAmount"), reusedAmount}, {QStringLiteral("budget"), budget}, {QStringLiteral("workspace"), workspace},
            {QStringLiteral("cloud"), cloud}, {QStringLiteral("baseHash"), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(before).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())}};
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, cancel, before, next, preview, jobs, error] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty() || self->document_ != before || jobs.size() > 500) { self->status_ = !error.isEmpty() ? error : QStringLiteral("准备已取消、编辑变化或超过 500 句，请重新准备。"); emit self->changed(); return; }
            self->flashDocument_ = next; self->flashPreview_ = preview; self->flashJobs_ = jobs;
            self->status_ = jobs.isEmpty() ? QStringLiteral("已有有效候选可复用，不重复收费；需要新版本时明确勾选重新付费。") : QStringLiteral("已准备待发送文本；实际 Token 与金额将在生成后记录，仅在确认后调用云端。"); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::confirmFlash()
{
    if (busy_ || session_->assetMutationBlocked() || !tasks_ || tasks_->busy() || flashPreview_.isEmpty()) return;
    QString error, key;
    const auto cloud = context_->cloudConfig->resolveSynthesis(&key, &error);
    if (cloud.isEmpty() || cloud != flashPreview_.value(QStringLiteral("cloud")).toObject()) {
        status_ = cloud.isEmpty() ? error : QStringLiteral("云账号、模型或 Workspace 已变化，请重新预览文本。");
        discardFlash(); emit changed(); return;
    }
    for (const auto &value : flashPreview_.value(QStringLiteral("voiceRevisions")).toObject()) {
        const auto revision = CloudTtsProvider::voiceRevision(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voice-revisions")), value.toObject(), &error);
        if (!revision || value.toObject().value(QStringLiteral("cloudVoiceRevision")).toString() != *revision) { status_ = error.isEmpty() ? QStringLiteral("云端音色已更新，请重新查询、选用并预览；未发送生成请求。") : error; discardFlash(); emit changed(); return; }
    }
    if (flashJobs_.isEmpty()) { if (flashPreview_.value(QStringLiteral("baseHash")).toString() == QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(document_).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) (void)commit(flashDocument_, QStringLiteral("已明确采用云端条件，可复用已有候选；未再次调用云端。")); discardFlash(); return; }
    if (flashPreview_.value(QStringLiteral("unknownUsage")).toBool()) { status_ = QStringLiteral("已有候选用量未知，请先核对账单；不会按零计算并继续派发。"); emit changed(); return; }
    if (flashPreview_.value(QStringLiteral("budget")).toDouble() <= flashPreview_.value(QStringLiteral("reusedAmount")).toDouble()) { status_ = QStringLiteral("已知费用达到该范围软预算，未派发。请调整预算后重新预览。"); emit changed(); return; }
    if (flashJobs_.first().input.projectGeneration != session_->generation() || flashJobs_.first().input.projectId != session_->projectId()
        || flashPreview_.value(QStringLiteral("baseHash")).toString() != QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(document_).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) { status_ = QStringLiteral("工程或编辑变化，请重新预览文本。"); emit changed(); return; }
    const auto jobs = flashJobs_;
    if (!commit(flashDocument_, QStringLiteral("已明确选择 Flash，候选和人工编排保留。"))) return;
    const bool started = tasks_->start(CloudTtsProvider(flashPreview_.value(QStringLiteral("workspace")).toString(), key,
        flashPreview_.value(QStringLiteral("budget")).toDouble() - flashPreview_.value(QStringLiteral("reusedAmount")).toDouble(), context_->aiHttp), jobs);
    status_ = started ? QStringLiteral("云端批次已手动确认；生成后记录实际 Token 与金额，已生成候选不会重复收费。") : tasks_->statusText(); flashPreview_ = {}; flashJobs_.clear(); emit changed();
}

void VoiceStudioController::previewJson(const QUrl &file, bool targeted)
{
    if (busy_ || session_->assetMutationBlocked() || !file.isLocalFile()) return;
    busy_ = true; cancel_ = false; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this);
    const auto before = document_; const auto projectId = session_->projectId(); const auto projectPath = session_->unified() ? session_->storagePath() : QString(); auto *cancel = &cancel_; emit changed();
    task_ = context_->submitProjectStorage([self, epoch, before, projectId, projectPath, cancel, file, targeted] {
        QFile input(file.toLocalFile()); QString error; QJsonObject next; QVariantList differences;
        if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024) error = QStringLiteral("无法读取导入文件或超过 16MiB。");
        else {
            const auto bytes = input.readAll();
            if (file.toLocalFile().endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                QJsonParseError parsed; const auto json = QJsonDocument::fromJson(bytes, &parsed);
                if (!json.isObject() || parsed.error != QJsonParseError::NoError) error = QStringLiteral("导入 JSON 无效。");
                else if (targeted) {
                    QJsonArray changes; next = VoiceDocument::targetedUpdate(before, json.object(), projectId, &changes, &error); differences = changes.toVariantList();
                    for (const auto &value : changes) {
                        const auto item = value.toObject().value(QStringLiteral("after")).toObject(); const auto voice = item.value(QStringLiteral("voiceVersionId")).toString();
                        if (!voice.isEmpty() && (projectPath.isEmpty() || !ProjectStore(projectPath).voiceVersion(voice, &error))) {
                            if (error.isEmpty()) error = QStringLiteral("JSON 指定的声音版本不在当前项目。请先绑定该版本，或在 JSON 中明确映射为已有版本，再重新导入。");
                            break;
                        }
                    }
                }
                else {
                    next = VoiceDocument::appendStructured(before, json.object(), &error);
                    auto items = next.value(QStringLiteral("segments")).toArray();
                    for (int i = before.value(QStringLiteral("segments")).toArray().size(); error.isEmpty() && i < items.size(); ++i) {
                        auto item = items[i].toObject(); const auto requested = item.value(QStringLiteral("requestedVoiceVersionId")).toString();
                        if (!requested.isEmpty() && !projectPath.isEmpty() && ProjectStore(projectPath).voiceVersion(requested, &error)) item.insert(QStringLiteral("voiceVersionId"), requested);
                        items[i] = item; differences.append(QVariantMap{{QStringLiteral("kind"), requested.isEmpty() ? QStringLiteral("新增句子") : item.value(QStringLiteral("voiceVersionId")).toString().isEmpty() ? QStringLiteral("新增句子，声音版本需手动映射") : QStringLiteral("新增句子，复用项目声音版本")}, {QStringLiteral("after"), item.toVariantMap()}});
                    }
                    next.insert(QStringLiteral("segments"), items);
                }
            } else if (targeted) error = QStringLiteral("定向更新仅接受版本化 JSON。");
            else next = VoiceDocument::appendText(before, QString::fromUtf8(bytes));
        }
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, before, next, differences, error, cancel, targeted] {
            if (!self || self->epoch_ != epoch) return; self->busy_ = false;
            if (cancel->load() || !error.isEmpty() || self->document_ != before) { self->status_ = cancel->load() ? QStringLiteral("导入已取消。") : error.isEmpty() ? QStringLiteral("读取期间文稿有修改，请重新导入。") : error; emit self->changed(); return; }
            if (next == before) { self->status_ = QStringLiteral("导入没有产生变更。"); emit self->changed(); return; }
            self->propose(next, differences.isEmpty() ? QVariantList{QVariantMap{{QStringLiteral("kind"), targeted ? QStringLiteral("定向更新") : QStringLiteral("新增导入：创建新 ID，不覆盖已有句子")}}} : differences, QStringLiteral("请核对新增／更新、生成参数、声音映射与手工保护后确认。"));
        }, Qt::QueuedConnection);
    });
}
void VoiceStudioController::exportJson(const QUrl &file)
{
    if (busy_ || session_->assetMutationBlocked() || !file.isLocalFile()) return;
    QString error;
    auto inputs = session_->inputPaths(); inputs.append(context_->settingsManager.path()); inputs.append(QFileInfo(context_->settingsManager.path()).absoluteDir().filePath(QStringLiteral("voices.sqlite")));
    if (!safeOutputPath(file.toLocalFile(), inputs, &error)) { status_ = error; emit changed(); return; }
    auto frozen = document_; frozen.insert(QStringLiteral("originProjectId"), session_->projectId());
    busy_ = true; cancel_ = false; auto *cancel = &cancel_; const auto epoch = ++epoch_; const QPointer<VoiceStudioController> self(this); emit changed();
    const auto path = session_->unified() ? session_->storagePath() : QString();
    task_ = context_->submitProjectStorage([self, epoch, frozen, file, cancel, inputs, path] {
        QString error; auto protectedPaths = inputs;
        if (!path.isEmpty()) for (const auto &value : ProjectStore(path).assets(&error)) protectedPaths.append(ProjectStore(path).resolveAudioPath(value.toObject().value(QStringLiteral("path")).toString()));
        QSaveFile output(file.toLocalFile()); const auto bytes = QJsonDocument(frozen).toJson();
        const bool success = error.isEmpty() && safeOutputPath(file.toLocalFile(), protectedPaths, &error) && !cancel->load() && output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && !cancel->load() && output.commit();
        if (self) QMetaObject::invokeMethod(self.data(), [self, epoch, success, error] { if (!self || self->epoch_ != epoch) return; self->busy_ = false; self->status_ = success ? QStringLiteral("文稿 JSON 已导出。") : !error.isEmpty() ? error : QStringLiteral("文稿导出失败或已取消，请检查磁盘和权限。"); emit self->changed(); }, Qt::QueuedConnection);
    });
}
}
