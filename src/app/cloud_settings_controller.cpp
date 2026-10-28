#include "cloud_settings_controller.h"
#include "application_context.h"
#include "asr/http_client.h"
#include "asr/dashscope_asr_service.h"
#include "ai/qwen_omni_provider.h"
#include "tts/cloud_tts_provider.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QUuid>

namespace subcue {
namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
}
CloudSettingsController::CloudSettingsController(ApplicationContext *context, QObject *parent)
    : QObject(parent), context_(context) { requests_.setMaxThreadCount(1); begin(); }
CloudSettingsController::~CloudSettingsController()
{
    cancelDiscovery(); requests_.waitForDone(); secrets_.clear();
}
QVariantList CloudSettingsController::accounts() const
{
    QVariantList result;
    for (const auto &value : draft_.value(QStringLiteral("accounts")).toArray()) {
        auto row = value.toObject(); const auto id = row.value(QStringLiteral("id")).toString();
        QString state = row.value(QStringLiteral("credentialRef")).toString().isEmpty() ? QStringLiteral("未配置") : QStringLiteral("已保存");
        const auto action = secrets_.value(id).action;
        if (action == CloudSecretAction::Replace) state = QStringLiteral("待替换");
        if (action == CloudSecretAction::Remove) state = QStringLiteral("待删除");
        bool existing = false;
        for (const auto &old : original_.value(QStringLiteral("accounts")).toArray())
            if (old.toObject().value(QStringLiteral("id")).toString() == id) existing = true;
        row.remove(QStringLiteral("credentialRef")); row.insert(QStringLiteral("keyState"), state);
        row.insert(QStringLiteral("existing"), existing); result.append(row.toVariantMap());
    }
    return result;
}
QVariantList CloudSettingsController::models() const { return draft_.value(QStringLiteral("models")).toArray().toVariantList(); }
QVariantList CloudSettingsController::bindings() const { return draft_.value(QStringLiteral("bindings")).toArray().toVariantList(); }
bool CloudSettingsController::dirty() const { return draft_ != original_ || !secrets_.isEmpty(); }
void CloudSettingsController::begin()
{
    cancelDiscovery(); secrets_.clear(); discovered_.clear(); tests_.clear();
    draft_ = context_->cloudConfig->load(&status_); original_ = draft_;
    for (const auto &value : draft_.value(QStringLiteral("models")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("id")).toString();
        if (context_->cloudConfig->modelVerified(id)) tests_.insert(id, QVariantMap{{QStringLiteral("state"), QStringLiteral("passed")}, {QStringLiteral("message"), QStringLiteral("此配置的任务能力已验证。")}});
    }
    emit changed();
}
void CloudSettingsController::discard()
{
    cancelDiscovery(); secrets_.clear(); draft_ = original_; discovered_.clear(); status_.clear(); tests_.clear(); emit changed();
}
QString CloudSettingsController::addAccount(const QString &vendor)
{
    if (draft_.isEmpty()) return {};
    const QString id = newId(); auto rows = draft_.value(QStringLiteral("accounts")).toArray();
    rows.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("vendor"), vendor},
        {QStringLiteral("name"), vendor == QLatin1String("bailian") ? QStringLiteral("百炼账号") : vendor == QLatin1String("deepseek") ? QStringLiteral("DeepSeek 账号") : QStringLiteral("Moonshot 账号")},
        {QStringLiteral("region"), vendor == QLatin1String("bailian") ? QStringLiteral("beijing") : QStringLiteral("default")}, {QStringLiteral("enabled"), true}});
    draft_.insert(QStringLiteral("accounts"), rows); emit changed(); return id;
}
void CloudSettingsController::updateRow(const QString &table, const QString &id, const QVariantMap &fields, const QStringList &allowed)
{
    auto rows = draft_.value(table).toArray();
    for (qsizetype i = 0; i < rows.size(); ++i) {
        auto row = rows[i].toObject(); if (row.value(QStringLiteral("id")).toString() != id) continue;
        for (auto it = fields.begin(); it != fields.end(); ++it) if (allowed.contains(it.key())) row.insert(it.key(), QJsonValue::fromVariant(it.value()));
        rows[i] = row;
    }
    draft_.insert(table, rows); cancelDiscovery(); tests_.clear(); status_.clear(); emit changed();
}
void CloudSettingsController::editAccount(const QString &id, const QVariantMap &fields)
{
    for (const auto &value : original_.value(QStringLiteral("accounts")).toArray()) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("id")).toString() == id && fields.contains(QStringLiteral("region"))
            && row.value(QStringLiteral("region")).toString() != fields.value(QStringLiteral("region")).toString()) {
            status_ = QStringLiteral("变更地域请新建账号，不会向新地域发送原密钥"); emit changed(); return;
        }
    }
    updateRow(QStringLiteral("accounts"), id, fields, {QStringLiteral("name"), QStringLiteral("region"), QStringLiteral("notes"), QStringLiteral("enabled"), QStringLiteral("workspace")});
}
void CloudSettingsController::moveAccount(const QString &id, int direction)
{
    auto rows = draft_.value(QStringLiteral("accounts")).toArray();
    for (qsizetype i = 0; i < rows.size(); ++i) if (rows[i].toObject().value(QStringLiteral("id")).toString() == id) {
        const auto target = i + (direction < 0 ? -1 : 1);
        if (target >= 0 && target < rows.size()) { const auto row = rows.takeAt(i); rows.insert(target, row); draft_.insert(QStringLiteral("accounts"), rows); emit changed(); } return;
    }
}
void CloudSettingsController::removeAccount(const QString &id)
{
    cancelDiscovery(); auto rows = draft_.value(QStringLiteral("accounts")).toArray();
    for (qsizetype i = rows.size(); i-- > 0;) if (rows[i].toObject().value(QStringLiteral("id")).toString() == id) rows.removeAt(i);
    const auto models = draft_.value(QStringLiteral("models")).toArray();
    for (const auto &row : models) if (row.toObject().value(QStringLiteral("accountId")).toString() == id) removeModel(row.toObject().value(QStringLiteral("id")).toString());
    secrets_.remove(id); draft_.insert(QStringLiteral("accounts"), rows); emit changed();
}
bool CloudSettingsController::stageKey(const QString &id, const QString &key)
{
    bool found = false; for (const auto &row : draft_.value(QStringLiteral("accounts")).toArray()) if (row.toObject().value(QStringLiteral("id")).toString() == id) found = true;
    if (!found || key.trimmed().isEmpty() || key.contains(QLatin1Char('\n')) || key.contains(QLatin1Char('\r'))) { status_ = QStringLiteral("请填写有效密钥，不能包含换行"); emit changed(); return false; }
    cancelDiscovery(); tests_.clear(); secrets_.insert(id, {CloudSecretAction::Replace, key.trimmed()}); status_.clear(); emit changed(); return true;
}
void CloudSettingsController::removeKey(const QString &id) { cancelDiscovery(); tests_.clear(); secrets_.insert(id, {CloudSecretAction::Remove, {}}); emit changed(); }
void CloudSettingsController::keepKey(const QString &id) { cancelDiscovery(); tests_.clear(); secrets_.remove(id); emit changed(); }
QString CloudSettingsController::addModel(const QString &accountId, const QString &remoteId, const QString &adapter)
{
    if (remoteId.trimmed().isEmpty()) { status_ = QStringLiteral("请填写实际模型 ID"); emit changed(); return {}; }
    auto rows = draft_.value(QStringLiteral("models")).toArray();
    for (const auto &value : rows) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("accountId")).toString() == accountId && row.value(QStringLiteral("remoteId")).toString() == remoteId.trimmed()
            && row.value(QStringLiteral("adapter")).toString() == adapter) return row.value(QStringLiteral("id")).toString();
    }
    const auto id = newId();
    rows.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("accountId"), accountId}, {QStringLiteral("remoteId"), remoteId.trimmed()},
        {QStringLiteral("name"), remoteId.trimmed()}, {QStringLiteral("adapter"), adapter}, {QStringLiteral("workspace"), QString()}, {QStringLiteral("enabled"), true}});
    draft_.insert(QStringLiteral("models"), rows); emit changed(); return id;
}
void CloudSettingsController::editModel(const QString &id, const QVariantMap &fields)
{ updateRow(QStringLiteral("models"), id, fields, {QStringLiteral("name"), QStringLiteral("remoteId"), QStringLiteral("workspace"), QStringLiteral("enabled")}); }
void CloudSettingsController::removeModel(const QString &id)
{
    auto rows = draft_.value(QStringLiteral("models")).toArray();
    for (qsizetype i = rows.size(); i-- > 0;) if (rows[i].toObject().value(QStringLiteral("id")).toString() == id) rows.removeAt(i);
    auto bindings = draft_.value(QStringLiteral("bindings")).toArray();
    for (qsizetype i = 0; i < bindings.size(); ++i) { auto row = bindings[i].toObject(); if (row.value(QStringLiteral("target")).toString() == id) { row.insert(QStringLiteral("target"), QString()); bindings[i] = row; } }
    draft_.insert(QStringLiteral("models"), rows); draft_.insert(QStringLiteral("bindings"), bindings); emit changed();
}
void CloudSettingsController::bindModel(const QString &task, const QString &model)
{
    auto rows = draft_.value(QStringLiteral("bindings")).toArray();
    for (qsizetype i = 0; i < rows.size(); ++i) { auto row = rows[i].toObject(); if (row.value(QStringLiteral("taskId")).toString() == task) { row.insert(QStringLiteral("target"), model); rows[i] = row; } }
    draft_.insert(QStringLiteral("bindings"), rows); emit changed();
}
bool CloudSettingsController::save()
{
    cancelDiscovery(); requests_.waitForDone();
    QStringList removed;
    for (const auto &value : original_.value(QStringLiteral("accounts")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("id")).toString();
        bool exists = false;
        for (const auto &row : draft_.value(QStringLiteral("accounts")).toArray()) if (row.toObject().value(QStringLiteral("id")).toString() == id) exists = true;
        if (!exists || secrets_.value(id).action == CloudSecretAction::Remove) removed.append(id);
    }
    if (!removed.isEmpty() && !context_->beginCloudMutation(removed)) { status_ = QStringLiteral("云端配置正在修改或有其他 SubCue 实例运行，请正常关闭其他实例后再删除凭据。"); emit changed(); return false; }
    const bool saved = context_->cloudConfig->save(draft_, original_.value(QStringLiteral("revision")).toInteger(), secrets_, &status_);
    if (!removed.isEmpty()) context_->endCloudMutation();
    if (!saved) { emit changed(); return false; }
    const auto tested = tests_;
    const auto savedState = context_->cloudConfig->load(&status_);
    for (const auto &value : savedState.value(QStringLiteral("models")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("id")).toString();
        if (tested.value(id).toMap().value(QStringLiteral("state")) == QStringLiteral("passed")
            && !context_->cloudConfig->markModelVerified(id, savedState.value(QStringLiteral("revision")).toInteger(), &status_)) {
            // 配置已保存；验证写入失败不能恢复成旧配置，也不能冒充全部成功。
            const auto message = status_; begin(); status_ = QStringLiteral("配置已保存，但验证记录保存失败：") + message; emit changed(); return false;
        }
    }
    const auto message = status_; begin(); if (status_.isEmpty()) status_ = message; emit changed(); return true;
}
QVariantList CloudSettingsController::taskChoices(const QString &task) const
{
    QVariantList rows;
    if (task == QLatin1String("speechRecognition")) rows = {QVariantMap{{QStringLiteral("id"), QStringLiteral("local:funasr")}, {QStringLiteral("name"), QStringLiteral("本地 Fun-ASR")}}, QVariantMap{{QStringLiteral("id"), QStringLiteral("local:qwen3")}, {QStringLiteral("name"), QStringLiteral("本地 Qwen3-ASR")}}};
    const auto required = task == QLatin1String("speechRecognition") ? QStringLiteral("dashscope-asr") : task == QLatin1String("subtitleWordMapping") ? QStringLiteral("compatible-text") : task == QLatin1String("speechSynthesis") ? QStringLiteral("qwen-tts") : QStringLiteral("qwen-omni");
    for (const auto &value : models()) {
        auto model = value.toMap(); if (model.value(QStringLiteral("adapter")) != required || !model.value(QStringLiteral("enabled"), true).toBool()) continue;
        for (const auto &owner : accounts()) {
            const auto account = owner.toMap(); if (account.value(QStringLiteral("id")) != model.value(QStringLiteral("accountId")) || !account.value(QStringLiteral("enabled"), true).toBool()) continue;
            if (!CloudConfigStore::adaptedModel(QJsonObject::fromVariantMap(account), QJsonObject::fromVariantMap(model)) && tests_.value(model.value(QStringLiteral("id")).toString()).toMap().value(QStringLiteral("state")) != QStringLiteral("passed")) continue;
            if (required == QLatin1String("qwen-tts") && (model.value(QStringLiteral("remoteId")) != QStringLiteral("qwen-audio-3.1-tts-flash") || account.value(QStringLiteral("region")) != QStringLiteral("beijing"))) continue;
            model.insert(QStringLiteral("name"), account.value(QStringLiteral("name")).toString() + QStringLiteral(" / ") + model.value(QStringLiteral("name")).toString()); rows.append(model);
        }
    }
    return rows;
}
QString CloudSettingsController::taskStatus(const QString &task) const
{
    QString target; for (const auto &value : bindings()) if (value.toMap().value(QStringLiteral("taskId")) == task) target = value.toMap().value(QStringLiteral("target")).toString();
    if (target.isEmpty()) return QStringLiteral("未配置 · 请选择模型");
    if (target.startsWith(QLatin1String("local:"))) return QStringLiteral("本地引擎 · 安装状态在语音识别页检查");
    bool compatible = false; for (const auto &row : taskChoices(task)) if (row.toMap().value(QStringLiteral("id")) == target) compatible = true;
    if (!compatible) return QStringLiteral("原选择不可用 · 检查启用状态与模型能力");
    for (const auto &row : models()) if (row.toMap().value(QStringLiteral("id")) == target) {
        const auto model = row.toMap();
        if (task == QLatin1String("speechSynthesis") && model.value(QStringLiteral("workspace")).toString().isEmpty()) return QStringLiteral("缺少 Workspace · 在模型详情填写");
        for (const auto &owner : accounts()) if (owner.toMap().value(QStringLiteral("id")) == model.value(QStringLiteral("accountId"))) {
            if (QStringList{QStringLiteral("未配置"), QStringLiteral("待删除")}.contains(owner.toMap().value(QStringLiteral("keyState")).toString())) return QStringLiteral("需要添加密钥 · 在账号详情配置");
        }
    }
    return tests_.value(target).toMap().value(QStringLiteral("state")) == QStringLiteral("passed") ? QStringLiteral("任务能力已验证") : QStringLiteral("配置完整 · 已适配，尚未测试");
}
bool CloudSettingsController::switchTask(const QString &task, const QString &model)
{
    if (draft_.isEmpty()) begin();
    if (dirty()) { status_ = QStringLiteral("设置中有未保存草稿，请先保存或放弃，再快速切换。"); emit changed(); return false; }
    bool found = false; for (const auto &row : taskChoices(task)) if (row.toMap().value(QStringLiteral("id")) == model) found = true;
    if (!found) { status_ = QStringLiteral("此模型与用途不兼容，请在 AI 服务检查。"); emit changed(); return false; }
    const auto before = draft_; bindModel(task, model);
    if (save()) return true;
    const auto message = status_; draft_ = before; status_ = message; emit changed(); return false;
}
void CloudSettingsController::testCapability(const QString &modelId, const QString &voiceId)
{
    cancelDiscovery(); QJsonObject model, account;
    for (const auto &value : draft_.value(QStringLiteral("models")).toArray()) if (value.toObject().value(QStringLiteral("id")).toString() == modelId) model = value.toObject();
    for (const auto &value : draft_.value(QStringLiteral("accounts")).toArray()) if (value.toObject().value(QStringLiteral("id")) == model.value(QStringLiteral("accountId"))) account = value.toObject();
    const auto accountId = account.value(QStringLiteral("id")).toString(), adapter = model.value(QStringLiteral("adapter")).toString();
    const auto reject = [&](const QString &message) { tests_.insert(modelId, QVariantMap{{QStringLiteral("state"), QStringLiteral("failed")}, {QStringLiteral("message"), message}}); emit changed(); };
    if (account.isEmpty() || model.isEmpty() || !account.value(QStringLiteral("enabled")).toBool(true) || !model.value(QStringLiteral("enabled")).toBool(true)) { reject(QStringLiteral("请先启用账号和模型。")); return; }
    const auto action = secrets_.value(accountId);
    const auto key = action.action == CloudSecretAction::Replace ? action.value : action.action == CloudSecretAction::Remove ? QString() : context_->cloudConfig->resolveKey(accountId, &status_);
    if (key.isEmpty()) { reject(QStringLiteral("请先添加密钥，测试不会自动保存。")); return; }
    if (adapter != QLatin1String("compatible-text") && account.value(QStringLiteral("vendor")) != QLatin1String("bailian")) { reject(QStringLiteral("此厂商尚未适配音频协议。")); return; }
    if (adapter == QLatin1String("qwen-tts") && (!CloudConfigStore::adaptedModel(account, model) || !QRegularExpression(QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9-]{0,127}$")).match(model.value(QStringLiteral("workspace")).toString()).hasMatch() || voiceId.trimmed().isEmpty())) { reject(QStringLiteral("仅适配北京 Flash，请填写 Workspace 和测试音色。")); return; }
    cancel_ = std::make_shared<std::atomic<bool>>(false); const auto cancel = cancel_; const auto generation = generation_;
    if (!context_->trackCloudTask(this, accountId, [this] { cancelDiscovery(); })) return;
    busy_ = true; status_.clear(); tests_.insert(modelId, QVariantMap{{QStringLiteral("state"), QStringLiteral("running")}, {QStringLiteral("message"), QStringLiteral("正在测试任务能力；取消后不自动重发。")}}); emit changed();
    const QPointer<CloudSettingsController> self(this); auto *http = context_->aiHttp;
    requests_.start([self, cancel, generation, key, account, model, modelId, adapter, voiceId, http] {
        bool success = false;
        if (adapter == QLatin1String("qwen-tts")) {
            QTemporaryDir directory; CloudTtsProvider provider(model.value(QStringLiteral("workspace")).toString(), key, 1.0, http); TtsJob job;
            job.id = newId(); job.input.projectId = newId(); job.input.projectGeneration = 1; job.input.segmentId = newId(); job.input.voiceVersionId = newId();
            job.input.spokenText = QStringLiteral("连接测试。"); job.input.language = QStringLiteral("Chinese"); job.input.provider = QStringLiteral("qwen-cloud"); job.input.model = model.value(QStringLiteral("remoteId")).toString(); job.input.modelVersion = QStringLiteral("probe-v1"); job.input.providerVoiceId = voiceId.trimmed(); job.input.region = QStringLiteral("beijing");
            job.billing = {{QStringLiteral("price"), TtsPricing::flashPrice()}};
            const auto report = provider.generate({job}, directory.path(), cancel.get(), {}, 30000, {}); success = report.success() && report.results.size() == 1;
        } else if (adapter == QLatin1String("dashscope-asr")) {
            // 固定测试音，不上传工程；非空词级结果才算协议能力通过。
            QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("probe.wav")); QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(QwenOmniProvider::builtInProbeClip().data) > 0) {
                file.close(); const auto workspace = model.value(QStringLiteral("workspace")).toString();
                const auto host = workspace.isEmpty() ? QString() : QStringLiteral("https://%1.%2.maas.aliyuncs.com").arg(workspace, account.value(QStringLiteral("region")) == QLatin1String("singapore") ? QStringLiteral("ap-southeast-1") : QStringLiteral("cn-beijing"));
                DashScopeAsrService provider(key, model.value(QStringLiteral("remoteId")).toString(), account.value(QStringLiteral("region")).toString(), host, http);
                const auto result = provider.transcribe({path, cancel.get()}); success = std::holds_alternative<Transcript>(result) && !std::get<Transcript>(result).words.isEmpty();
            }
        } else {
            OmniReviewSettings settings; settings.model = model.value(QStringLiteral("remoteId")).toString(); settings.timeoutMs = 30000;
            const auto vendor = account.value(QStringLiteral("vendor")).toString();
            settings.baseUrl = vendor == QLatin1String("deepseek") ? QStringLiteral("https://api.deepseek.com") : vendor == QLatin1String("moonshot") ? QStringLiteral("https://api.moonshot.cn/v1") : account.value(QStringLiteral("region")) == QLatin1String("singapore") ? QStringLiteral("https://dashscope-intl.aliyuncs.com/compatible-mode/v1") : QString::fromLatin1(kOmniReviewDefaultBaseUrl);
            settings.provider = adapter == QLatin1String("compatible-text") ? vendor + QStringLiteral("-text") : QStringLiteral("qwen_omni");
            QwenOmniProvider provider(key, settings, http); OmniChatRequest request;
            request.task = adapter == QLatin1String("compatible-text") ? OmniTaskType::WordMappingReview : OmniTaskType::ConnectionTest;
            request.systemPrompt = QStringLiteral("Call submit_connection_test with ok=true."); request.userText = QStringLiteral("Built-in protocol probe. Return ok=true using the required tool.");
            if (adapter == QLatin1String("qwen-omni")) request.audio = QwenOmniProvider::builtInProbeClip();
            request.tools = QwenOmniProvider::connectionTestToolSchema(); request.toolChoiceName = QStringLiteral("submit_connection_test");
            const auto result = provider.complete(request, cancel.get()); success = std::holds_alternative<OmniChatResponse>(result) && std::get<OmniChatResponse>(result).arguments.value(QStringLiteral("ok")).toBool();
        }
        if (cancel->load() || !self) return;
        QMetaObject::invokeMethod(self.data(), [self, generation, modelId, success] {
            if (!self || self->generation_ != generation) return;
            if (success && !self->dirty()) {
                QString error;
                if (!self->context_->cloudConfig->markModelVerified(modelId, self->original_.value(QStringLiteral("revision")).toInteger(), &error)) {
                    self->busy_ = false; self->context_->clearCloudTask(self.data()); self->status_ = error; emit self->changed(); return;
                }
            }
            self->context_->clearCloudTask(self.data()); self->busy_ = false; self->tests_.insert(modelId, QVariantMap{{QStringLiteral("state"), success ? QStringLiteral("passed") : QStringLiteral("failed")}, {QStringLiteral("message"), success ? QStringLiteral("模型访问通过 · 样例协议能力通过；不代表真实内容质量。保存后记录验证。") : QStringLiteral("任务能力未通过：可能是权限、协议或样例结果无效。未自动重发，可在实际任务查看具体阻断。")}}); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
void CloudSettingsController::cancelDiscovery()
{
    if (cancel_) cancel_->store(true);
    requests_.waitForDone();
    context_->clearCloudTask(this);
    for (auto it = tests_.begin(); it != tests_.end(); ++it) {
        if (it.value().toMap().value(QStringLiteral("state")) == QStringLiteral("running"))
            it.value() = QVariantMap{{QStringLiteral("state"), QStringLiteral("unverified")}, {QStringLiteral("message"), QStringLiteral("已取消，请手动重新测试。")}};
    }
    ++generation_; busy_ = false; discovered_.clear(); emit changed();
}
void CloudSettingsController::discoverModels(const QString &accountId) { requestModels(accountId, true); }
void CloudSettingsController::testConnection(const QString &accountId) { requestModels(accountId, false); }
void CloudSettingsController::requestModels(const QString &accountId, bool discovery)
{
    cancelDiscovery(); QJsonObject account;
    for (const auto &value : draft_.value(QStringLiteral("accounts")).toArray()) if (value.toObject().value(QStringLiteral("id")).toString() == accountId) account = value.toObject();
    if (account.isEmpty()) return;
    QString key;
    const auto action = secrets_.value(accountId);
    if (action.action == CloudSecretAction::Replace) key = action.value;
    else if (action.action != CloudSecretAction::Remove) key = context_->cloudConfig->resolveKey(accountId, &status_);
    const auto vendor = account.value(QStringLiteral("vendor")).toString();
    QString workspace = account.value(QStringLiteral("workspace")).toString();
    if (workspace.isEmpty()) for (const auto &value : draft_.value(QStringLiteral("models")).toArray()) {
        const auto model = value.toObject(); if (model.value(QStringLiteral("accountId")).toString() == accountId && !model.value(QStringLiteral("workspace")).toString().isEmpty()) { workspace = model.value(QStringLiteral("workspace")).toString(); break; }
    }
    const auto endpoint = vendor == QLatin1String("deepseek") ? QStringLiteral("https://api.deepseek.com/models")
        : vendor == QLatin1String("moonshot") ? QStringLiteral("https://api.moonshot.cn/v1/models")
        : account.value(QStringLiteral("region")) == QLatin1String("singapore") ? QStringLiteral("https://dashscope-intl.aliyuncs.com/api/v1/models")
        : !workspace.isEmpty() ? QStringLiteral("https://%1.cn-beijing.maas.aliyuncs.com/api/v1/models").arg(workspace) : QString();
    QVariantList fallback;
    if (vendor == QLatin1String("bailian") && account.value(QStringLiteral("region")) == QLatin1String("beijing")) fallback.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("qwen-audio-3.1-tts-flash")}, {QStringLiteral("name"), QStringLiteral("通义 Flash · 语音合成")}, {QStringLiteral("adapter"), QStringLiteral("qwen-tts")}, {QStringLiteral("source"), QStringLiteral("官方静态目录 v1 · 2026-10-17 · 权限未验证")}});
    if (key.isEmpty() || endpoint.isEmpty()) {
        discovered_ = fallback;
        tests_.insert(accountId, QVariantMap{{QStringLiteral("state"), QStringLiteral("unverified")}, {QStringLiteral("message"), key.isEmpty() ? QStringLiteral("请先添加 API Key；显示官方静态目录，未验证权限。") : QStringLiteral("workspace_required：北京原生目录需要业务空间；请在账号详情填写一次。显示官方目录退路，未伪装查询成功。")}});
        emit changed(); return;
    }
    cancel_ = std::make_shared<std::atomic<bool>>(false); const auto cancel = cancel_; const auto generation = generation_;
    if (!context_->trackCloudTask(this, accountId, [self = QPointer<CloudSettingsController>(this)] { if (self) self->cancelDiscovery(); })) return;
    busy_ = true; status_.clear();
    tests_.insert(accountId, QVariantMap{{QStringLiteral("state"), QStringLiteral("running")}, {QStringLiteral("message"), QStringLiteral("仅查询官方元数据，不调用推理…")}}); emit changed();
    const QPointer<CloudSettingsController> self(this); auto *http = context_->aiHttp;
    requests_.start([self, cancel, generation, key, endpoint, http, accountId, discovery, account, vendor, fallback] {
        QtNetworkHttpClient native; QVariantList rows; QString message; bool success = false;
        for (int page = 1; page <= 100 && !cancel->load(); ++page) {
            HttpRequest request; request.url = QUrl(endpoint + (vendor == QLatin1String("bailian") ? QStringLiteral("?page_no=%1&page_size=100").arg(page) : QString())); request.method = QByteArrayLiteral("GET");
            request.headers = {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + key.toUtf8()}};
            request.followRedirects = false; request.maxResponseBytes = 2 * 1024 * 1024; request.transferTimeoutMs = 15000;
            const auto response = (http ? http : &native)->send(request, cancel.get());
            if (!std::holds_alternative<HttpResponse>(response)) { message = QStringLiteral("offline：网络不可用或请求取消，请重试。"); break; }
            const auto value = std::get<HttpResponse>(response);
            if (value.status != 200) { message = value.status == 401 || value.status == 403 ? QStringLiteral("unauthorized：Key 无效或权限不足。") : value.status == 429 ? QStringLiteral("throttled：请求限流，请稍后刷新。") : QStringLiteral("目录查询失败（HTTP %1），检查地域与 Workspace。").arg(value.status); break; }
            const auto document = QJsonDocument::fromJson(value.body).object(); const auto output = document.value(QStringLiteral("output")).toObject();
            const auto models = vendor == QLatin1String("bailian") ? output.value(QStringLiteral("models")) : document.value(QStringLiteral("data"));
            if (!models.isArray()) { message = QStringLiteral("目录响应格式无效，未将错误当作空列表。"); break; }
            for (const auto &entry : models.toArray()) {
                const auto model = entry.toObject(); const auto id = model.value(vendor == QLatin1String("bailian") ? QStringLiteral("model") : QStringLiteral("id")).toString();
                if (id.isEmpty() || id.contains(key) || id.size() > 256 || rows.size() >= 10000) continue;
                QString adapter = vendor == QLatin1String("bailian") ? QString() : QStringLiteral("compatible-text");
                if (vendor == QLatin1String("bailian")) for (const auto &candidate : {QStringLiteral("qwen-tts"), QStringLiteral("dashscope-asr"), QStringLiteral("qwen-omni"), QStringLiteral("compatible-text")})
                    if (CloudConfigStore::adaptedModel(account, {{QStringLiteral("remoteId"), id}, {QStringLiteral("adapter"), candidate}})) { adapter = candidate; break; }
                if (adapter.isEmpty()) continue;
                rows.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), adapter == QLatin1String("qwen-tts") ? QStringLiteral("通义 Flash · 语音合成") : model.value(QStringLiteral("name")).toString(id)}, {QStringLiteral("adapter"), adapter}, {QStringLiteral("source"), QStringLiteral("官方目录可见 · 生成权限未验证")}});
            }
            if (vendor != QLatin1String("bailian") || page * 100 >= output.value(QStringLiteral("total")).toInt(models.toArray().size()) || models.toArray().isEmpty()) { success = true; break; }
        }
        if (success) message = rows.isEmpty() ? QStringLiteral("empty：目录查询成功，未找到当前适配器支持的型号。") : QStringLiteral("已读取 %1 个已适配型号；目录可见不代表生成权限已验证。").arg(rows.size());
        else if (!fallback.isEmpty()) { rows = fallback; message += QStringLiteral(" 使用官方静态目录 v1（2026-10-17），来源：help.aliyun.com/zh/model-studio/list-models；权限未验证。"); }
        if (cancel->load() || !self) return;
        QMetaObject::invokeMethod(self.data(), [self, generation, rows, message, success, accountId, discovery] {
            if (!self || self->generation_ != generation) return;
            self->context_->clearCloudTask(self.data()); self->busy_ = false; if (discovery) self->discovered_ = rows;
            self->tests_.insert(accountId, QVariantMap{{QStringLiteral("state"), success ? QStringLiteral("passed") : QStringLiteral("unverified")}, {QStringLiteral("message"), message}}); emit self->changed();
        }, Qt::QueuedConnection);
    });
}
} // namespace subcue
