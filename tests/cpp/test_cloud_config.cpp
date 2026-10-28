#include "settings/cloud_config_store.h"
#include "ai/ai_review_settings.h"
#include "ai/qwen_omni_provider.h"
#include "settings/settings_manager.h"
#include "platform/windows/dpapi_credential_store.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUrl>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtTest/QTest>

using namespace subcue;
namespace {
class MemoryCredentials final : public ICredentialStore {
public:
    mutable QMap<QString, QString> values;
    mutable bool rejectWrite = false;
    mutable bool rejectRemove = false;
    QString load(const QString &id, QString *) const override { return values.value(id); }
    bool save(const QString &id, const QString &value, QString *error) const override {
        if (rejectWrite) { if (error) *error = QStringLiteral("测试写入失败"); return false; }
        values.insert(id, value); return true;
    }
    bool remove(const QString &id, QString *) const override { if (rejectRemove) return false; values.remove(id); return true; }
    bool exists(const QString &id) const override { return values.contains(id); }
    QStringList list(const QString &prefix, QString *) const override {
        QStringList result; for (const auto &id : values.keys()) if (id.startsWith(prefix)) result.append(id); return result;
    }
};
QJsonObject account(QJsonObject state, const QString &id, const QString &vendor = QStringLiteral("bailian"))
{
    auto accounts = state.value(QStringLiteral("accounts")).toArray();
    accounts.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), id}, {QStringLiteral("vendor"), vendor},
        {QStringLiteral("region"), vendor == QLatin1String("bailian") ? "beijing" : "default"}, {QStringLiteral("enabled"), true}});
    state.insert(QStringLiteral("accounts"), accounts); return state;
}
QString reference(const QJsonObject &state, const QString &id)
{
    for (const auto &value : state.value(QStringLiteral("accounts")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == id) return value.toObject().value(QStringLiteral("credentialRef")).toString();
    return {};
}
bool executeSql(const QString &path, const QString &sql)
{
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("cloud-test"));
        db.setDatabaseName(path);
        if (db.open()) { QSqlQuery query(db); ok = query.exec(sql); }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("cloud-test")); return ok;
}
}
class CloudConfigTests : public QObject {
    Q_OBJECT
private slots:
    void migrationAndReopen();
    void rotationAndFrozenTask();
    void failuresAndConflict();
    void rejectsUnsafeMetadata();
    void scopedCleanupAndCorruptDatabase();
    void nativeCredentialNamespace();
    void legacyRetirementPreservesUnifiedAndRetries();
    void asrUsesUnifiedSnapshot();
    void reviewUsesUnifiedSnapshot_data();
    void reviewUsesUnifiedSnapshot();
};
void CloudConfigTests::migrationAndReopen()
{
    QTemporaryDir directory; MemoryCredentials keys;
    const auto path = directory.filePath(QStringLiteral("connections.sqlite"));
    CloudConfigStore store(path, &keys); QString error;
    auto legacy = SettingsManager::defaults();
    legacy.insert(QStringLiteral("ttsConnections"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("work")}, {QStringLiteral("name"), QStringLiteral("工作")}, {QStringLiteral("workspace"), QStringLiteral("workspace-1")}, {QStringLiteral("credential"), QStringLiteral("SubCue/VoiceStudio/old")}}});
    legacy.insert(QStringLiteral("ttsActiveConnection"), QStringLiteral("work"));
    QVERIFY2(store.initialize(legacy, &error), qPrintable(error));
    auto state = store.load(&error);
    QCOMPARE(state.value(QStringLiteral("bindings")).toArray().size(), 5);
    QCOMPARE(state.value(QStringLiteral("accounts")).toArray().size(), 2);
    QVERIFY(keys.values.isEmpty());
    CloudConfigStore reopened(path, &keys);
    QVERIFY(reopened.initialize({}, &error));
    QCOMPARE(reopened.load(), state);
    SettingsManager settings(directory.filePath(QStringLiteral("settings.json")));
    QVERIFY(!settings.load().contains(QStringLiteral("ttsConnectionReadError")));
}
void CloudConfigTests::rotationAndFrozenTask()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error;
    const auto path = directory.filePath(QStringLiteral("connections.sqlite"));
    CloudConfigStore store(path, &keys); QVERIFY(store.initialize(SettingsManager::defaults(), &error));
    auto draft = account(store.load(), QStringLiteral("shared"));
    auto models = draft.value(QStringLiteral("models")).toArray();
    models.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("text")}, {QStringLiteral("accountId"), QStringLiteral("shared")}, {QStringLiteral("remoteId"), QStringLiteral("test-model")}, {QStringLiteral("name"), QStringLiteral("文本")}, {QStringLiteral("adapter"), QStringLiteral("compatible-text")}, {QStringLiteral("enabled"), true}});
    draft.insert(QStringLiteral("models"), models);
    auto bindings = draft.value(QStringLiteral("bindings")).toArray();
    for (qsizetype i = 0; i < bindings.size(); ++i) if (bindings[i].toObject().value(QStringLiteral("taskId")) == QJsonValue(QStringLiteral("subtitleWordMapping"))) bindings[i] = QJsonObject{{QStringLiteral("taskId"), QStringLiteral("subtitleWordMapping")}, {QStringLiteral("target"), QStringLiteral("text")}};
    draft.insert(QStringLiteral("bindings"), bindings);
    QVERIFY2(store.save(draft, 0, {{QStringLiteral("shared"), {CloudSecretAction::Replace, QStringLiteral("sentinel-old")}}}, &error), qPrintable(error));
    QVERIFY(store.markModelVerified(QStringLiteral("text"), 1, &error));
    QString taskKey;
    const auto task = store.resolveTask(QStringLiteral("subtitleWordMapping"), &taskKey, &error);
    QVERIFY2(!task.isEmpty(), qPrintable(error)); QCOMPARE(taskKey, QStringLiteral("sentinel-old"));
    const auto oldRef = reference(store.load(), QStringLiteral("shared"));
    QVERIFY(store.save(store.load(), 1, {{QStringLiteral("shared"), {CloudSecretAction::Replace, QStringLiteral("sentinel-new")}}}, &error));
    QCOMPARE(taskKey, QStringLiteral("sentinel-old"));
    QCOMPARE(store.resolveKey(QStringLiteral("shared")), QStringLiteral("sentinel-new"));
    QVERIFY(!keys.values.contains(oldRef)); QCOMPARE(keys.values.size(), 1);
    QVERIFY(!QJsonDocument(store.load()).toJson().contains("sentinel"));
    QFile database(path); QVERIFY(database.open(QIODevice::ReadOnly)); QVERIFY(!database.readAll().contains("sentinel")); database.close();
    QVERIFY(store.save(store.load(), 2, {{QStringLiteral("shared"), {CloudSecretAction::Remove, {}}}}, &error));
    QVERIFY(store.resolveKey(QStringLiteral("shared")).isEmpty()); QVERIFY(keys.values.isEmpty());
}
void CloudConfigTests::failuresAndConflict()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error;
    const auto path = directory.filePath(QStringLiteral("connections.sqlite"));
    CloudConfigStore store(path, &keys); QVERIFY(store.initialize(SettingsManager::defaults()));
    auto draft = account(store.load(), QStringLiteral("a"));
    keys.rejectWrite = true;
    QVERIFY(!store.save(draft, 0, {{QStringLiteral("a"), {CloudSecretAction::Replace, QStringLiteral("test-key")}}}, &error));
    QCOMPARE(store.load().value(QStringLiteral("revision")).toInteger(), 0);
    keys.rejectWrite = false;
    QVERIFY(executeSql(path, QStringLiteral("CREATE TRIGGER reject_cloud BEFORE INSERT ON cloud_accounts BEGIN SELECT RAISE(ABORT,'test'); END")));
    QVERIFY(!store.save(draft, 0, {{QStringLiteral("a"), {CloudSecretAction::Replace, QStringLiteral("test-key")}}}, &error));
    QVERIFY(keys.values.isEmpty()); QCOMPARE(store.load().value(QStringLiteral("revision")).toInteger(), 0);
    QVERIFY(executeSql(path, QStringLiteral("DROP TRIGGER reject_cloud")));
    QVERIFY(store.save(draft, 0, {{QStringLiteral("a"), {CloudSecretAction::Replace, QStringLiteral("test-key")}}}, &error));
    QVERIFY(!store.save(draft, 0, {}, &error)); QCOMPARE(keys.values.size(), 1);
    keys.rejectRemove = true;
    QVERIFY(store.save(store.load(), 1, {{QStringLiteral("a"), {CloudSecretAction::Replace, QStringLiteral("new-key")}}}, &error));
    QVERIFY(error.contains(QStringLiteral("清理待重试"))); QCOMPARE(keys.values.size(), 2);
    keys.rejectRemove = false; QVERIFY(store.cleanup(&error)); QCOMPARE(keys.values.size(), 1);
}
void CloudConfigTests::rejectsUnsafeMetadata()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error;
    CloudConfigStore store(directory.filePath(QStringLiteral("connections.sqlite")), &keys);
    QVERIFY(store.initialize(SettingsManager::defaults()));
    auto draft = account(store.load(), QStringLiteral("a"));
    auto accounts = draft.value(QStringLiteral("accounts")).toArray();
    auto row = accounts.last().toObject(); row.insert(QStringLiteral("apiKey"), QStringLiteral("must-not-persist")); accounts[accounts.size() - 1] = row;
    draft.insert(QStringLiteral("accounts"), accounts);
    QVERIFY(!store.save(draft, 0, {}, &error));
    draft = store.load(); draft.insert(QStringLiteral("accounts"), QStringLiteral("invalid-collection"));
    QVERIFY(!store.save(draft, 0, {}, &error));
    draft = store.load(); draft.insert(QStringLiteral("bindings"), QJsonArray{});
    QVERIFY(!store.save(draft, 0, {}, &error));
    draft = account(store.load(), QStringLiteral("a")); QVERIFY(store.save(draft, 0, {}));
    accounts = store.load().value(QStringLiteral("accounts")).toArray(); row = accounts.last().toObject(); row.insert(QStringLiteral("region"), QStringLiteral("singapore")); accounts[accounts.size() - 1] = row;
    draft = store.load(); draft.insert(QStringLiteral("accounts"), accounts); QVERIFY(!store.save(draft, 1, {}, &error));
    QLockFile lock(directory.filePath(QStringLiteral("connections.sqlite.cloud.lock"))); QVERIFY(lock.tryLock());
    QVERIFY(store.load(&error).isEmpty()); QVERIFY(!error.isEmpty());
}
void CloudConfigTests::scopedCleanupAndCorruptDatabase()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error;
    const auto path = directory.filePath(QStringLiteral("connections.sqlite"));
    CloudConfigStore store(path, &keys); QVERIFY(store.initialize(SettingsManager::defaults()));
    const auto prefix = QStringLiteral("SubCue/Cloud/%1/").arg(store.load().value(QStringLiteral("storeId")).toString());
    keys.values.insert(prefix + QStringLiteral("orphan"), QStringLiteral("not-logged"));
    keys.values.insert(QStringLiteral("SubCue/Cloud/other-store/keep"), QStringLiteral("keep"));
    QVERIFY(store.cleanup(&error)); QCOMPARE(keys.values.size(), 1);
    keys.values.insert(prefix + QStringLiteral("preserve-on-corruption"), QStringLiteral("keep"));
    QVERIFY(executeSql(path, QStringLiteral("DROP TABLE cloud_models")));
    QVERIFY(!store.cleanup(&error)); QCOMPARE(keys.values.size(), 2);
    QVERIFY(!store.initialize(SettingsManager::defaults(), &error));
    QCOMPARE(keys.values.size(), 2);
}
void CloudConfigTests::asrUsesUnifiedSnapshot()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error, key;
    CloudConfigStore store(directory.filePath(QStringLiteral("connections.sqlite")), &keys);
    QVERIFY(store.initialize(SettingsManager::defaults()));
    auto draft = account(store.load(), QStringLiteral("work"));
    auto models = draft.value(QStringLiteral("models")).toArray();
    models.append(QJsonObject{{"id", "asr-work"}, {"accountId", "work"}, {"remoteId", "fun-asr-flash"},
        {"name", "识别"}, {"adapter", "dashscope-asr"}, {"workspace", "workspace-work"}, {"enabled", true}});
    draft.insert(QStringLiteral("models"), models);
    auto bindings = draft.value(QStringLiteral("bindings")).toArray();
    for (qsizetype index = 0; index < bindings.size(); ++index)
        if (bindings.at(index).toObject().value(QStringLiteral("taskId")) == QLatin1String("speechRecognition"))
            bindings[index] = QJsonObject{{"taskId", "speechRecognition"}, {"target", "asr-work"}};
    draft.insert(QStringLiteral("bindings"), bindings);
    QVERIFY(store.save(draft, 0, {{QStringLiteral("work"), {CloudSecretAction::Replace, QStringLiteral("sentinel-unified")}}}, &error));
    keys.values.insert(QStringLiteral("SubCue/ASR/dashscope"), QStringLiteral("legacy-must-not-be-used"));
    auto settings = SettingsManager::defaults();
    QVERIFY2(store.resolveAsr(&settings, &key, &error), qPrintable(error));
    QCOMPARE(key, QStringLiteral("sentinel-unified"));
    QCOMPARE(settings.value(QStringLiteral("asrModel")).toString(), QStringLiteral("fun-asr-flash"));
    QCOMPARE(settings.value(QStringLiteral("asrApiHost")).toString(), QStringLiteral("https://workspace-work.cn-beijing.maas.aliyuncs.com"));
    QCOMPARE(settings.value(QStringLiteral("asrProvider")).toString(), QStringLiteral("dashscope"));
    draft = store.load();
    QVERIFY(store.save(draft, 1, {{QStringLiteral("work"), {CloudSecretAction::Remove, {}}}}, &error));
    QVERIFY(!store.resolveAsr(&settings, &key, &error));
    QVERIFY(key.isEmpty());
}
void CloudConfigTests::reviewUsesUnifiedSnapshot_data()
{
    QTest::addColumn<QString>("vendor"); QTest::addColumn<QString>("task");
    for (const auto &vendor : {QStringLiteral("bailian"), QStringLiteral("deepseek"), QStringLiteral("moonshot")})
        QTest::newRow(qPrintable(vendor)) << vendor << QStringLiteral("subtitleWordMapping");
    QTest::newRow("subtitle-audio") << QStringLiteral("bailian") << QStringLiteral("subtitleAudioReview");
    QTest::newRow("roughcut-audio") << QStringLiteral("bailian") << QStringLiteral("roughcutAudioReview");
}
void CloudConfigTests::reviewUsesUnifiedSnapshot()
{
    QFETCH(QString, vendor); QFETCH(QString, task);
    QTemporaryDir directory; MemoryCredentials keys; QString error, key;
    CloudConfigStore store(directory.filePath(QStringLiteral("connections.sqlite")), &keys);
    QVERIFY(store.initialize(SettingsManager::defaults()));
    auto draft = account(store.load(), QStringLiteral("work"), vendor);
    const bool text = task == QLatin1String("subtitleWordMapping");
    auto models = draft.value(QStringLiteral("models")).toArray();
    models.append(QJsonObject{{"id", "review-work"}, {"accountId", "work"}, {"remoteId", "selected-model"},
        {"name", "复核"}, {"adapter", text ? "compatible-text" : "qwen-omni"}, {"enabled", true}});
    draft.insert(QStringLiteral("models"), models);
    auto bindings = draft.value(QStringLiteral("bindings")).toArray();
    for (qsizetype index = 0; index < bindings.size(); ++index)
        if (bindings.at(index).toObject().value(QStringLiteral("taskId")).toString() == task)
            bindings[index] = QJsonObject{{"taskId", task}, {"target", "review-work"}};
    draft.insert(QStringLiteral("bindings"), bindings);
    QVERIFY(store.save(draft, 0, {{QStringLiteral("work"), {CloudSecretAction::Replace, QStringLiteral("sentinel-review")}}}, &error));
    keys.values.insert(QStringLiteral("SubCue/AIReview/qwen_omni"), QStringLiteral("legacy-must-not-be-used"));
    QVERIFY(store.markModelVerified(QStringLiteral("review-work"), store.load().value(QStringLiteral("revision")).toInteger(), &error));
    OmniReviewSettings settings;
    QVERIFY2(OmniReviewSettingsStore::resolveTask(store, task, {}, &settings, &key, &error), qPrintable(error));
    QCOMPARE(key, QStringLiteral("sentinel-review")); QCOMPARE(settings.model, QStringLiteral("selected-model"));
    QCOMPARE(QUrl(settings.baseUrl).host(), vendor == QLatin1String("bailian") ? QStringLiteral("dashscope.aliyuncs.com")
        : vendor == QLatin1String("deepseek") ? QStringLiteral("api.deepseek.com") : QStringLiteral("api.moonshot.cn"));
    const auto frozen = settings; const auto frozenKey = key;
    QVERIFY(store.save(store.load(), 1, {{QStringLiteral("work"), {CloudSecretAction::Remove, {}}}}, &error));
    QVERIFY(!OmniReviewSettingsStore::resolveTask(store, task, {}, &settings, &key, &error));
    QVERIFY(key.isEmpty()); QCOMPARE(frozenKey, QStringLiteral("sentinel-review"));
    QCOMPARE(frozen.model, QStringLiteral("selected-model"));
}
void CloudConfigTests::legacyRetirementPreservesUnifiedAndRetries()
{
    QTemporaryDir directory; MemoryCredentials keys; QString error;
    SettingsManager manager(directory.filePath(QStringLiteral("settings.json")));
    QVERIFY(manager.save(SettingsManager::defaults()));
    CloudConfigStore store(directory.filePath(QStringLiteral("connections.sqlite")), &keys);
    QVERIFY(store.initialize(manager.load()));
    QVERIFY(store.save(account(store.load(), QStringLiteral("keep")), 0,
        {{QStringLiteral("keep"), {CloudSecretAction::Replace, QStringLiteral("new-sentinel")}}}));
    const auto unified = store.load();
    keys.values.insert(QStringLiteral("SubCue/VoiceStudio/orphan"), QStringLiteral("old-sentinel"));
    keys.values.insert(QStringLiteral("OtherApp/keep"), QStringLiteral("other-sentinel"));
    QFile file(manager.path()); QVERIFY(file.open(QIODevice::WriteOnly));
    const auto original = QJsonDocument(QJsonObject{{"voiceApiKey", "plaintext-sentinel"}, {"localAsrPython", "keep-python"}, {"customSetting", "keep-custom"}}).toJson();
    file.write(original); file.close();
    auto preview = manager.legacyCloudPreview(keys, &error); QVERIFY2(!preview.isEmpty(), qPrintable(error));
    QVERIFY(!QJsonDocument(preview).toJson().contains("plaintext-sentinel"));
    QVERIFY(file.open(QIODevice::Append)); file.write(" "); file.close();
    QVERIFY(!manager.retireLegacyCloud(keys, preview, &error)); QVERIFY(keys.values.contains(QStringLiteral("SubCue/VoiceStudio/orphan")));
    keys.rejectRemove = true; preview = manager.legacyCloudPreview(keys);
    QVERIFY(!manager.retireLegacyCloud(keys, preview, &error));
    QVERIFY(manager.load().value(QStringLiteral("legacyCloudRetired")).toBool());
    QVERIFY(!manager.load().value(QStringLiteral("legacyPendingCredentials")).toArray().isEmpty());
    QCOMPARE(store.load(), unified); QCOMPARE(store.resolveKey(QStringLiteral("keep")), QStringLiteral("new-sentinel"));
    keys.rejectRemove = false;
    QVERIFY2(manager.retireLegacyCloud(keys, manager.legacyCloudPreview(keys), &error), qPrintable(error));
    QVERIFY(!keys.values.contains(QStringLiteral("SubCue/VoiceStudio/orphan")));
    QVERIFY(keys.values.contains(QStringLiteral("OtherApp/keep"))); QCOMPARE(store.load(), unified);
    QVERIFY(manager.load().value(QStringLiteral("legacyPendingCredentials")).toArray().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly)); const auto cleaned = file.readAll(); file.close();
    QVERIFY(!cleaned.contains("plaintext-sentinel")); QVERIFY(cleaned.contains("keep-custom"));
    QCOMPARE(manager.load().value(QStringLiteral("localAsrPython")).toString(), QStringLiteral("keep-python"));
    QVERIFY(manager.save(manager.load())); QVERIFY(!manager.load().contains(QStringLiteral("ttsCloudWorkspace")));
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(QJsonObject{{"legacyPendingCredentials", QJsonArray{reference(unified, QStringLiteral("keep"))}}}).toJson()); file.close();
    QVERIFY(manager.legacyCloudPreview(keys, &error).isEmpty());
    QCOMPARE(store.resolveKey(QStringLiteral("keep")), QStringLiteral("new-sentinel"));
}

void CloudConfigTests::nativeCredentialNamespace()
{
    QTemporaryDir directory; DpapiCredentialStore keys(directory.filePath(QStringLiteral("credentials.dat"))); QString error;
    const auto ref = QStringLiteral("SubCue/Cloud/test-scope/test-version");
    QVERIFY2(keys.save(ref, QStringLiteral("isolated-test-key"), &error), qPrintable(error));
    const auto refs = keys.list(QStringLiteral("SubCue/Cloud/test-scope/"), &error);
    const bool found = refs.contains(ref);
    QVERIFY(keys.remove(ref, &error));
    QVERIFY2(found, qPrintable(error)); QVERIFY(!keys.exists(ref));
}
QTEST_GUILESS_MAIN(CloudConfigTests)
#include "test_cloud_config.moc"
