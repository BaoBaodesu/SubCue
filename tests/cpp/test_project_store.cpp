#include "project/project_store.h"
#include "project/project_storage_adapter.h"
#include "project/project_manager.h"
#include "tts/tts_types.h"
#include "tts/voice_library.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <QtCore/QScopeGuard>
#include <limits>

using namespace subcue;

class ProjectStoreTests final : public QObject {
    Q_OBJECT
private slots:
    void abruptWriterExit_data()
    {
        QTest::addColumn<bool>("committed");
        QTest::newRow("exit-before-commit") << false;
        QTest::newRow("exit-after-commit") << true;
    }
    void abruptWriterExit()
    {
        const auto python = qEnvironmentVariable("SUBCUE_CRASH_TEST_PYTHON");
        if (python.isEmpty()) QSKIP("进程退出故障验收须显式提供独立 Python，仅使用标准库。");
        QVERIFY(QFileInfo(python).isFile()); QFETCH(bool, committed);
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("crash.sqlite"));
        ProjectStore store(path); QVERIFY(store.create());
        auto before = store.load(); QVERIFY(before);
        before->modules.insert(QStringLiteral("manual"), QJsonObject{{QStringLiteral("text"), QStringLiteral("保留人工编辑")}});
        QVERIFY(store.save(*before)); before = store.load(); QVERIFY(before);
        QProcess worker;
        const auto stopped = qScopeGuard([&] { if (worker.state() != QProcess::NotRunning) { worker.kill(); worker.waitForFinished(10000); } });
        // 小缓存强制脏页写入回滚日志；os._exit 不执行连接析构或 Python 清理。
        worker.start(python, {QStringLiteral("-I"), QStringLiteral("-B"), QStringLiteral("-c"), QString::fromLatin1(R"PY(
import json, os, sqlite3, sys
db = sqlite3.connect(sys.argv[1])
assert db.execute('PRAGMA journal_mode=DELETE').fetchone()[0] == 'delete'
db.execute('PRAGMA synchronous=FULL')
db.execute('PRAGMA cache_size=8')
db.execute('BEGIN IMMEDIATE')
db.execute('UPDATE project SET edit_revision=edit_revision+1,modules=?',
           (json.dumps({'crashProbe': 'x' * 1048576}),))
assert os.path.getsize(sys.argv[1] + '-journal') > 512
if sys.argv[2] == 'commit':
    db.commit()
os._exit(73)
)PY"), path, committed ? QStringLiteral("commit") : QStringLiteral("rollback")});
        QVERIFY(worker.waitForStarted(10000)); QVERIFY(worker.waitForFinished(30000));
        QVERIFY2(worker.exitCode() == 73, worker.readAllStandardError().constData());
        const auto recovered = store.load(); QVERIFY(recovered);
        QCOMPARE(recovered->projectId, before->projectId);
        QCOMPARE(recovered->revision, before->revision + (committed ? 1 : 0));
        if (committed) QCOMPARE(recovered->modules.value(QStringLiteral("crashProbe")).toString().size(), 1048576);
        else QCOMPARE(recovered->modules, before->modules);
        auto next = *recovered; next.modules = before->modules;
        QVERIFY(store.save(next)); QCOMPARE(store.load()->revision, recovered->revision + 1);
        const auto backup = directory.filePath(QStringLiteral("recovered-backup.sqlite"));
        QVERIFY(store.backup(backup)); QCOMPARE(ProjectStore(backup).load()->modules, before->modules);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral("-journal")));
    }
    void emotionPredictionRejectsStaleAndMalformedResults()
    {
        QTemporaryDir directory; VoiceLibrary library(directory.filePath(QStringLiteral("voices.sqlite"))); QVERIFY(library.initialize());
        const auto profile = library.createProfile(QStringLiteral("预测存储 fixture")); QVERIFY(profile);
        QFile source(directory.filePath(QStringLiteral("reference.bin"))); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("prediction storage fixture"); source.close();
        const auto hash = QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(source.fileName()).toHex());
        QJsonObject prediction{{QStringLiteral("assetSha256"), hash}, {QStringLiteral("modelVersion"), QString(64, QLatin1Char('a'))},
            {QStringLiteral("weightSha256"), QString(64, QLatin1Char('b'))}, {QStringLiteral("model"), QStringLiteral("emotion2vec_plus_base")},
            {QStringLiteral("policy"), QStringLiteral("review-v1-score0.6-margin0.15")}, {QStringLiteral("label"), QStringLiteral("neutral")},
            {QStringLiteral("scores"), QJsonArray{0.3,0,0,0.3,0.4,0,0,0,0}}, {QStringLiteral("score"), 0.4}, {QStringLiteral("margin"), 0.1},
            {QStringLiteral("requiresReview"), true}, {QStringLiteral("reviewed"), false}};
        QVERIFY(VoiceLibrary::validEmotionPrediction(prediction));
        const QJsonObject metadata{{QStringLiteral("manualEmotion"), QStringLiteral("happy")}, {QStringLiteral("emotionPrediction"), prediction}};
        const auto version = library.addVersion(*profile, metadata, source.fileName()); QVERIFY(version);
        ProjectStore project(directory.filePath(QStringLiteral("project.sqlite"))); QVERIFY(project.create()); QVERIFY(library.copyVersionToProject(*version, project));
        const auto snapshot = project.voiceVersion(*version); QVERIFY(snapshot);
        for (const auto &key : {QStringLiteral("label"), QStringLiteral("scores"), QStringLiteral("score"), QStringLiteral("margin"), QStringLiteral("requiresReview"), QStringLiteral("modelVersion")}) {
            auto invalid = prediction; invalid.insert(key, QJsonValue());
            QVERIFY(!VoiceLibrary::validEmotionPrediction(invalid));
            QVERIFY(!library.addVersion(*profile, {{QStringLiteral("emotionPrediction"), invalid}}, source.fileName()));
        }
        prediction.insert(QStringLiteral("assetSha256"), QString(64, QLatin1Char('c')));
        QVERIFY(!library.addVersion(*profile, {{QStringLiteral("emotionPrediction"), prediction}}, source.fileName()));
        QCOMPARE(library.versions(*profile).size(), 1); QCOMPARE(project.voiceVersion(*version), snapshot);
        QCOMPARE(library.version(*version)->value(QStringLiteral("metadata")).toObject(), metadata);
    }
    void manualEmotionVersionsPreserveProjectSnapshot()
    {
        QTemporaryDir directory; VoiceLibrary library(directory.filePath(QStringLiteral("voices.sqlite")));
        QVERIFY(library.initialize()); const auto profile = library.createProfile(QStringLiteral("情绪审核")); QVERIFY(profile);
        QFile source(directory.filePath(QStringLiteral("reference.bin")));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("emotion metadata storage fixture"); source.close();
        const QJsonObject metadata{{QStringLiteral("referenceText"), QStringLiteral("参考转写")}, {QStringLiteral("manualEmotion"), QStringLiteral("happy")}};
        const auto first = library.addVersion(*profile, metadata, source.fileName()); QVERIFY(first);
        const auto original = library.version(*first);
        ProjectStore project(directory.filePath(QStringLiteral("project.sqlite"))); QVERIFY(project.create());
        QVERIFY(library.copyVersionToProject(*first, project)); const auto snapshot = project.voiceVersion(*first);
        auto changed = metadata; changed.insert(QStringLiteral("manualEmotion"), QStringLiteral("unclassified"));
        changed.insert(QStringLiteral("sourceVersionId"), *first);
        const auto second = library.addVersion(*profile, changed, source.fileName()); QVERIFY(second);
        QVERIFY(*first != *second); QCOMPARE(library.version(*first), original); QCOMPARE(project.voiceVersion(*first), snapshot);
        QCOMPARE(library.version(*second)->value(QStringLiteral("metadata")).toObject().value(QStringLiteral("manualEmotion")).toString(), QStringLiteral("unclassified"));
        for (const auto &invalid : {QJsonValue(QStringLiteral("excited")), QJsonValue(0.9), QJsonValue(QJsonValue::Null)}) {
            changed.insert(QStringLiteral("manualEmotion"), invalid); QString error;
            QVERIFY(!library.addVersion(*profile, changed, source.fileName(), &error)); QVERIFY(!error.isEmpty());
        }
        QCOMPARE(library.versions(*profile).size(), 2);
        for (const auto &label : {QStringLiteral("angry"), QStringLiteral("disgusted"), QStringLiteral("fearful"), QStringLiteral("neutral"), QStringLiteral("other"), QStringLiteral("sad"), QStringLiteral("surprised"), QStringLiteral("unknown")}) QVERIFY(VoiceLibrary::validEmotionLabel(label));
    }
    void voiceLibraryV1MigrationPreservesVersions()
    {
        QTemporaryDir directory; const QString path = directory.filePath(QStringLiteral("legacy-voices.sqlite"));
        VoiceLibrary library(path); QVERIFY(library.initialize());
        const auto profile = library.createProfile(QStringLiteral("旧角色")); QVERIFY(profile);
        const auto version = library.addVersion(*profile, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("providerVoiceId"), QStringLiteral("Cherry")}}); QVERIFY(version);
        const auto snapshot = library.version(*version);
        const QString connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection); db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db); QVERIFY(query.exec(QStringLiteral("DROP TABLE profile_defaults"))); QVERIFY(query.exec(QStringLiteral("UPDATE library SET version=1")));
        }
        QSqlDatabase::removeDatabase(connection);
        QVERIFY(library.defaultVersion(*profile).isEmpty());
        QCOMPARE(library.version(*version), snapshot);
        QVERIFY(library.initialize()); QVERIFY(library.setDefaultVersion(*profile, *version));
        QCOMPARE(library.defaultVersion(*profile), *version); QCOMPARE(library.version(*version), snapshot);
    }
    void voiceDefaultsDoNotChangeVersionsOrProjectSnapshots()
    {
        QTemporaryDir directory;
        VoiceLibrary library(directory.filePath(QStringLiteral("voices.sqlite"))); QVERIFY(library.initialize());
        const auto first = library.createProfile(QStringLiteral("旁白")); const auto second = library.createProfile(QStringLiteral("其它角色"));
        QVERIFY(first && second); QVERIFY(library.defaultVersion(*first).isEmpty());
        QFile source(directory.filePath(QStringLiteral("ref.bin")));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("default voice storage fixture"); source.close();
        const auto pending = library.addVersion(*first, {{QStringLiteral("referenceText"), QStringLiteral("转写")}}, source.fileName()); QVERIFY(pending);
        QVERIFY(!library.setDefaultVersion(*first, *pending));
        const auto hash = RoughCutProjectSerializer::mediaSha256(source.fileName());
        const auto approved = library.addVersion(*first, {{QStringLiteral("referenceText"), QStringLiteral("转写")},
            {QStringLiteral("qualityState"), QStringLiteral("approved")}, {QStringLiteral("quality"), QJsonObject{{QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())}}}}, source.fileName());
        QVERIFY(approved); QVERIFY(library.setDefaultVersion(*first, *approved));
        QCOMPARE(library.defaultVersion(*first), *approved); QVERIFY(!library.setDefaultVersion(*second, *approved));
        ProjectStore project(directory.filePath(QStringLiteral("project.sqlite"))); QVERIFY(project.create());
        QVERIFY(library.copyVersionToProject(*approved, project)); const auto snapshot = project.voiceVersion(*approved);
        const auto cloud = library.addVersion(*first, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("providerVoiceId"), QStringLiteral("Cherry")}}); QVERIFY(cloud);
        QVERIFY(library.setDefaultVersion(*first, *cloud)); QCOMPARE(library.defaultVersion(*first), *cloud);
        QCOMPARE(project.voiceVersion(*approved), snapshot);
        QVERIFY(library.setProfileArchived(*first, true)); QVERIFY(!library.setDefaultVersion(*first, *approved));
        QVERIFY(library.setProfileArchived(*first, false)); QCOMPARE(library.defaultVersion(*first), *cloud);
        QCOMPARE(library.versions(*first).size(), 3);
    }
    void takeCommitRejectsChangedQualityFingerprint()
    {
        QTemporaryDir directory; ProjectStore store(directory.filePath(QStringLiteral("quality.sqlite")));
        QVERIFY(store.create()); QVERIFY(store.enqueue(QStringLiteral("job"), {{QStringLiteral("text"), QStringLiteral("输入")}}));
        QFile audio(directory.filePath(QStringLiteral("audio.bin")));
        QVERIFY(audio.open(QIODevice::WriteOnly)); audio.write("original storage fixture"); audio.close();
        const auto hash = RoughCutProjectSerializer::mediaSha256(audio.fileName());
        QVERIFY(audio.open(QIODevice::WriteOnly)); audio.write("changed storage fixture"); audio.close();
        QString error;
        QVERIFY(!store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName(), &error, hash));
        QVERIFY(error.contains(QStringLiteral("指纹"))); QVERIFY(store.history().isEmpty());
        QCOMPARE(store.jobs().first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        std::atomic<bool> cancel{true};
        QVERIFY(!store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName(), &error,
            RoughCutProjectSerializer::mediaSha256(audio.fileName()), &cancel));
        QVERIFY(store.history().isEmpty());
        QVERIFY(store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName(), &error,
            RoughCutProjectSerializer::mediaSha256(audio.fileName())));
        QCOMPARE(store.history().size(), 1);
    }
    void globalVoiceVersionsRemainIndependentOfProjects()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        VoiceLibrary library(directory.filePath(QStringLiteral("voices.sqlite")));
        QString error;
        QVERIFY2(library.initialize(&error), qPrintable(error));
        QVERIFY(library.initialize());
        const auto profile = library.createProfile(QStringLiteral("旁白"));
        QVERIFY(profile);
        QVERIFY(!library.createProfile(QStringLiteral("  ")));
        QFile source(directory.filePath(QStringLiteral("sample.bin")));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("reference storage fixture"); source.close();
        const QJsonObject metadata{{QStringLiteral("referenceText"), QStringLiteral("人工审核的参考转写")}};
        const auto first = library.addVersion(*profile, metadata, source.fileName(), &error);
        QVERIFY2(first, qPrintable(error));
        QVERIFY(source.remove());
        const auto original = library.version(*first);
        QVERIFY(original); QVERIFY(QFileInfo::exists(original->value(QStringLiteral("referencePath")).toString()));
        ProjectStore project(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(project.create());
        QVERIFY2(library.copyVersionToProject(*first, project, &error), qPrintable(error));
        QVERIFY(library.renameProfile(*profile, QStringLiteral("改名")));
        const auto second = library.addVersion(*profile, {{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("providerVoiceId"), QStringLiteral("Cherry")}}, {}, &error);
        QVERIFY2(second, qPrintable(error));
        QVERIFY(*first != *second);
        QCOMPARE(library.versions(*profile).size(), 2);
        QCOMPARE(library.versions(*profile, 1, 1).size(), 1);
        QVERIFY(library.copyVersionToProject(*second, project));
        QVERIFY(library.setProfileArchived(*profile, true));
        QVERIFY(library.profiles().isEmpty()); QCOMPARE(library.profiles(true).size(), 1);
        QVERIFY(!library.addVersion(*profile, metadata));
        QCOMPARE(library.version(*first), original);
        QCOMPARE(project.voiceVersion(*first)->value(QStringLiteral("metadata")).toObject(), metadata);
        const QString owned = project.voiceVersion(*first)->value(QStringLiteral("referencePath")).toString();
        QVERIFY(QFile::remove(original->value(QStringLiteral("referencePath")).toString()));
        QVERIFY(QFileInfo::exists(owned));
        QVERIFY(!library.copyVersionToProject(*first, project));
        QVERIFY(library.setProfileArchived(*profile, false));
        QVERIFY(!library.addVersion(*profile, {{QStringLiteral("binding"), QJsonObject{{QStringLiteral("apiKey"), QStringLiteral("forbidden")}}}}));
        std::atomic<bool> cancel{true};
        QVERIFY(!library.addVersion(*profile, metadata, {}, nullptr, &cancel));
        QVERIFY(!library.renameProfile(QUuid::createUuid().toString(), QStringLiteral("不存在")));
    }
    void projectVoiceVersionIsImmutableAndPortable()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        QFile reference(directory.filePath(QStringLiteral("global-reference.bin")));
        QVERIFY(reference.open(QIODevice::WriteOnly)); reference.write("storage voice sample fixture"); reference.close();
        const auto hash = RoughCutProjectSerializer::mediaSha256(reference.fileName());
        const QString voiceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString versionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject metadata{{QStringLiteral("name"), QStringLiteral("旁白角色")}, {QStringLiteral("referenceText"), QStringLiteral("已审核转写")}};
        const auto revision = store.load()->revision;
        QVERIFY(store.importVoiceVersion(voiceId, versionId, reference.fileName(), hash, metadata));
        QVERIFY(store.importVoiceVersion(voiceId, versionId, reference.fileName(), hash, metadata));
        auto changed = metadata; changed.insert(QStringLiteral("referenceText"), QStringLiteral("不可覆盖旧版"));
        QVERIFY(!store.importVoiceVersion(voiceId, versionId, reference.fileName(), hash, changed));
        QCOMPARE(store.load()->revision, revision);
        const auto copied = store.voiceVersion(versionId);
        QVERIFY(copied); QCOMPARE(copied->value(QStringLiteral("metadata")).toObject(), metadata);
        QVERIFY(copied->value(QStringLiteral("referencePath")).toString() != reference.fileName());
        QVERIFY(reference.remove());
        const QString backup = directory.filePath(QStringLiteral("portable.sqlite"));
        QVERIFY(store.backup(backup));
        const auto portable = ProjectStore(backup).voiceVersion(versionId);
        QVERIFY(portable); QCOMPARE(portable->value(QStringLiteral("referenceAssetId")), copied->value(QStringLiteral("referenceAssetId")));
        QFile owned(portable->value(QStringLiteral("referencePath")).toString());
        QVERIFY(owned.open(QIODevice::ReadOnly)); QCOMPARE(owned.readAll(), QByteArray("storage voice sample fixture"));
        QVERIFY(!store.voiceVersion(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const QString cloudVersion = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject cloud{{QStringLiteral("provider"), QStringLiteral("qwen-cloud")}, {QStringLiteral("voiceId"), QStringLiteral("Cherry")}};
        QVERIFY(store.importVoiceVersion(voiceId, cloudVersion, {}, {}, cloud));
        const auto binding = store.voiceVersion(cloudVersion);
        QVERIFY(binding); QVERIFY(binding->value(QStringLiteral("referencePath")).toString().isEmpty());
        QCOMPARE(binding->value(QStringLiteral("metadata")).toObject(), cloud);
    }
    void ttsInputHashSeparatesGenerationFromEditing()
    {
        TtsRequest request;
        request.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.voiceVersionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.referenceAssetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.projectGeneration = 1; request.spokenText = QStringLiteral("完整朗读输入");
        request.provider = QStringLiteral("qwen-local"); request.model = QStringLiteral("Qwen3-TTS-12Hz-0.6B-Base");
        request.modelVersion = QStringLiteral("weights-version"); request.language = QStringLiteral("Chinese");
        request.referenceSha256 = QByteArray(32, 's'); request.referenceText = QStringLiteral("参考转写");
        TtsCapabilities capabilities{request.provider, request.model, {request.language}, {}, false, true, true, false};
        QVERIFY(request.validate(capabilities));
        const auto hash = request.inputHash();
        ++request.inputVersion; ++request.projectGeneration;
        request.segmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        request.referenceAssetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(request.inputHash(), hash);
        request.spokenText += QStringLiteral("修正");
        QVERIFY(request.inputHash() != hash);
        request.effectiveParameters.insert(QStringLiteral("instruct"), QStringLiteral("自由表演"));
        QVERIFY(!request.validate(capabilities));
        capabilities.parameters = {QStringLiteral("temperature")};
        request.effectiveParameters = {{QStringLiteral("temperature"), QJsonValue(QJsonValue::Null)}};
        QVERIFY(!request.validate(capabilities));
        request.effectiveParameters = {};
        request.referenceText.clear();
        QVERIFY(!request.validate(capabilities));
        capabilities = {QStringLiteral("qwen-cloud"), QStringLiteral("qwen-audio-3.1-tts-flash"), {request.language}, {}, true, false, false, true};
        request.provider = capabilities.provider; request.model = capabilities.model;
        request.providerVoiceId = QStringLiteral("Cherry"); request.region = QStringLiteral("beijing");
        QVERIFY(!request.validate(capabilities));
        request.referenceAssetId.clear(); request.referenceSha256.clear();
        QVERIFY(request.validate(capabilities));
        const auto snapshot = request.snapshot();
        QCOMPARE(snapshot.value(QStringLiteral("projectGeneration")).toString(), QStringLiteral("2"));
        QCOMPARE(snapshot.value(QStringLiteral("inputHash")).toString(), QString::fromLatin1(request.inputHash().toHex()));
        const auto restored = TtsRequest::fromSnapshot(snapshot, capabilities);
        QVERIFY(restored); QCOMPARE(restored->snapshot(), snapshot);
        auto corrupt = snapshot; corrupt.insert(QStringLiteral("spokenText"), QStringLiteral("偷偷改稿"));
        QVERIFY(!TtsRequest::fromSnapshot(corrupt, capabilities));
        corrupt = snapshot; corrupt.insert(QStringLiteral("projectGeneration"), 2);
        QVERIFY(!TtsRequest::fromSnapshot(corrupt, capabilities));
        request.projectGeneration = std::numeric_limits<quint64>::max();
        const auto exact = TtsRequest::fromSnapshot(request.snapshot(), capabilities);
        QVERIFY(exact); QCOMPARE(exact->projectGeneration, request.projectGeneration);
    }
    void schemaTwoTaskUpgradeIsTransactionalAndReadOnlyOnOpen()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("schema-two.sqlite"));
        ProjectStore store(path);
        QVERIFY(store.create());
        const QJsonObject input{{QStringLiteral("text"), QStringLiteral("旧任务完整输入")}};
        QVERIFY(store.enqueue(QStringLiteral("old"), input));
        const QString connectionName = QStringLiteral("job-schema-two-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE job_details")));
            QVERIFY(query.exec(QStringLiteral("UPDATE project SET schema_version=2")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        const auto hash = RoughCutProjectSerializer::mediaSha256(path);
        const auto checkpoint = store.load();
        QVERIFY(checkpoint);
        QCOMPARE(store.jobs().first().toObject().value(QStringLiteral("input")).toObject(), input);
        QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), hash);
        auto stale = *checkpoint; ++stale.revision;
        QVERIFY(!store.save(stale));
        QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), hash);
        QVERIFY(store.save(*checkpoint));
        const auto migrated = store.jobs().first().toObject();
        QCOMPARE(migrated.value(QStringLiteral("input")).toObject(), input);
        QCOMPARE(migrated.value(QStringLiteral("createdAt")).toInteger(), qint64(0));
        QCOMPARE(migrated.value(QStringLiteral("inputHash")).toString().size(), 64);
        QVERIFY(store.transitionJob(QStringLiteral("old"), ProjectStore::JobState::Queued, ProjectStore::JobState::Running));
    }
    void failedRecoveryDoesNotSwitchSession()
    {
        QTemporaryDir directory;
        ProjectManager manager;
        const auto current = manager.create(directory.filePath(QStringLiteral("current.sqlite")));
        QVERIFY(current);
        const QString target = directory.filePath(QStringLiteral("target.sqlite"));
        ProjectStore store(target);
        QVERIFY(store.create());
        QVERIFY(store.enqueue(QStringLiteral("queued"), QJsonObject{{QStringLiteral("text"), QStringLiteral("恢复")}}));
        QLockFile mutation(target + QStringLiteral(".lock"));
        QVERIFY(mutation.tryLock(0));
        const auto generation = manager.generation();
        int recovered = -1;
        QString error;
        QVERIFY(!manager.open(target, ProjectStorageAdapter::Format::UnifiedSqlite, &error, {}, true, &recovered));
        QCOMPARE(manager.generation(), generation);
        QCOMPARE(recovered, -1);
        QVERIFY(manager.save(*current));
        mutation.unlock();
        QVERIFY(manager.open(target, ProjectStorageAdapter::Format::UnifiedSqlite, &error, {}, true, &recovered));
        QCOMPARE(recovered, 1);
        QCOMPARE(store.jobs().first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
    }
    void providerJobStatesAndImmutableAttempts()
    {
        using State = ProjectStore::JobState;
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("jobs.sqlite")));
        QVERIFY(store.create());
        ProjectStore::JobOptions options;
        options.provider = QStringLiteral("mock"); options.batchId = QStringLiteral("batch");
        options.inputHash = QByteArray(32, 'h');
        const QJsonObject input{{QStringLiteral("text"), QStringLiteral("冻结输入")}, {QStringLiteral("segmentId"), QStringLiteral("segment")}};
        QVERIFY(store.enqueue(QStringLiteral("job"), input, options));
        QFile audio(directory.filePath(QStringLiteral("storage-fixture.bin")));
        QVERIFY(audio.open(QIODevice::WriteOnly)); audio.write("not an audio quality fixture"); audio.close();
        QVERIFY(!store.finish(QStringLiteral("job"), QStringLiteral("premature"), audio.fileName()));
        QVERIFY(!store.transitionJob(QStringLiteral("job"), State::Queued, State::Completed));
        QVERIFY(store.transitionJob(QStringLiteral("job"), State::Queued, State::Running));
        QVERIFY(!store.transitionJob(QStringLiteral("job"), State::Queued, State::Failed));
        QVERIFY(!store.recordJobResult(QStringLiteral("job"), {}, QJsonObject{{QStringLiteral("state"), QStringLiteral("known")}}));
        const QJsonObject unknown{{QStringLiteral("state"), QStringLiteral("unknown")}};
        QVERIFY(store.recordJobResult(QStringLiteral("job"), QJsonObject{{QStringLiteral("sampleRate"), 24000}}, unknown));
        QVERIFY(store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName()));
        QVERIFY(!store.cancel(QStringLiteral("job")));
        QVERIFY(!store.finish(QStringLiteral("job"), QStringLiteral("duplicate"), audio.fileName()));
        QCOMPARE(store.history().size(), 1);
        QCOMPARE(store.jobs().first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("completed"));
        options.retryOf = QStringLiteral("job");
        QVERIFY(!store.enqueue(QStringLiteral("retry"), input, options));
        options.manualRetry = true;
        QVERIFY(store.enqueue(QStringLiteral("retry"), input, options));
        QVERIFY(!store.enqueue(QStringLiteral("retry"), QJsonObject{{QStringLiteral("text"), QStringLiteral("禁止覆写")}}, options));
        const auto latest = store.jobs(0, 1).first().toObject();
        QCOMPARE(latest.value(QStringLiteral("input")).toObject(), input);
        QCOMPARE(latest.value(QStringLiteral("inputHash")).toString(), QString::fromLatin1(options.inputHash.toHex()));
        QCOMPARE(latest.value(QStringLiteral("retryOf")).toString(), QStringLiteral("job"));
        QCOMPARE(store.jobs(0, 50, nullptr, QStringLiteral("batch")).size(), 2);
        QVERIFY(store.jobs(0, 50, nullptr, QStringLiteral("missing-batch")).isEmpty());
        QVERIFY(store.cancel(QStringLiteral("retry")));
        QCOMPARE(store.jobs(1, 1).first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("job"));
    }
    void cancellationKeepsKnownUsageAndRejectsLateAudio()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("usage.sqlite")));
        QVERIFY(store.create());
        ProjectStore::JobOptions options; options.provider = QStringLiteral("qwen-cloud"); options.cloud = true;
        QVERIFY(store.enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("付费请求")}}, options));
        QVERIFY(store.transitionJob(QStringLiteral("job"), ProjectStore::JobState::Queued, ProjectStore::JobState::Running));
        QVERIFY(store.cancel(QStringLiteral("job")));
        const QJsonObject known{{QStringLiteral("state"), QStringLiteral("known")}, {QStringLiteral("inputTokens"), 20},
            {QStringLiteral("outputTokens"), 200}, {QStringLiteral("requestId"), QStringLiteral("request")}};
        QVERIFY(store.recordJobUsage(QStringLiteral("job"), known));
        QVERIFY(store.recordJobUsage(QStringLiteral("job"), known));
        QVERIFY(!store.recordJobUsage(QStringLiteral("job"), QJsonObject{{QStringLiteral("state"), QStringLiteral("unknown")}}));
        QVERIFY(!store.recordJobResult(QStringLiteral("job"), {}, known));
        const auto task = store.jobs().first().toObject();
        QCOMPARE(task.value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
        QCOMPARE(task.value(QStringLiteral("usage")).toObject(), known);
        QCOMPARE(store.history().size(), 0);
    }
    void recoveryIsManualAndPreservesBilling()
    {
        using State = ProjectStore::JobState;
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("recovery.sqlite")));
        QVERIFY(store.create());
        const QJsonObject input{{QStringLiteral("text"), QStringLiteral("完整旧输入")}};
        ProjectStore::JobOptions options; options.provider = QStringLiteral("qwen-local");
        QVERIFY(store.enqueue(QStringLiteral("local"), input, options));
        QVERIFY(store.transitionJob(QStringLiteral("local"), State::Queued, State::Running));
        options.provider = QStringLiteral("qwen-cloud"); options.cloud = true;
        QVERIFY(store.enqueue(QStringLiteral("cloud"), input, options));
        QVERIFY(store.transitionJob(QStringLiteral("cloud"), State::Queued, State::Running));
        QVERIFY(store.enqueue(QStringLiteral("queued"), input, options));
        QVERIFY(store.enqueue(QStringLiteral("validating"), input, options));
        QVERIFY(store.transitionJob(QStringLiteral("validating"), State::Queued, State::Running));
        const QJsonObject usage{{QStringLiteral("state"), QStringLiteral("known")}, {QStringLiteral("inputTokens"), 10}, {QStringLiteral("outputTokens"), 120}};
        QVERIFY(store.recordJobResult(QStringLiteral("validating"), QJsonObject{{QStringLiteral("requestId"), QStringLiteral("known-request")}}, usage));
        const auto revision = store.load()->revision;
        QCOMPARE(store.recoverJobs(), std::optional<int>(4));
        QCOMPARE(store.recoverJobs(), std::optional<int>(0));
        QCOMPARE(store.load()->revision, revision);
        for (const auto &value : store.jobs()) {
            const auto job = value.toObject();
            QCOMPARE(job.value(QStringLiteral("input")).toObject(), input);
            const QString id = job.value(QStringLiteral("id")).toString();
            QCOMPARE(job.value(QStringLiteral("state")).toString(), id == QStringLiteral("cloud") ? QStringLiteral("outcome_unknown") : QStringLiteral("interrupted"));
            if (id == QStringLiteral("validating")) QCOMPARE(job.value(QStringLiteral("usage")).toObject(), usage);
            else QCOMPARE(job.value(QStringLiteral("usage")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("unknown"));
        }
        options.retryOf = QStringLiteral("cloud");
        QVERIFY(!store.enqueue(QStringLiteral("automatic-retry"), input, options));
        QCOMPARE(store.jobs().size(), 4);
    }
    void preparedVersionMustMatchBeforeSessionSwitch()
    {
        QTemporaryDir directory;
        ProjectManager manager;
        const auto current = manager.create(directory.filePath(QStringLiteral("current.sqlite")));
        QVERIFY(current);
        const auto generation = manager.generation();
        const QString otherPath = directory.filePath(QStringLiteral("other.sqlite"));
        QVERIFY(ProjectStore(otherPath).create());
        auto other = ProjectStore(otherPath).load();
        QVERIFY(other);
        const auto previousRevision = QByteArray::number(other->revision);
        QVERIFY(ProjectStore(otherPath).save(*other));
        QString error;
        QVERIFY(!manager.open(otherPath, ProjectStorageAdapter::Format::UnifiedSqlite, &error, previousRevision));
        QVERIFY(!error.isEmpty());
        QCOMPARE(manager.generation(), generation);
        ProjectManager freeTarget;
        QVERIFY(freeTarget.open(otherPath, ProjectStorageAdapter::Format::UnifiedSqlite));
        ProjectStorageAdapter::SaveResult result;
        QVERIFY(manager.save(*current, &error, nullptr, &result));
    }
    void referenceBackupKeepsPreviouslyOwnedMedia()
    {
        QTemporaryDir directory;
        const QString original = directory.filePath(QStringLiteral("original.sqlite"));
        ProjectStore store(original);
        QVERIFY(store.create());
        QFile media(directory.filePath(QStringLiteral("media.bin")));
        QVERIFY(media.open(QIODevice::WriteOnly)); media.write("storage media fixture"); media.close();
        const auto asset = store.importAsset(media.fileName(), QStringLiteral("media"));
        QVERIFY(asset);
        const QString portable = directory.filePath(QStringLiteral("portable.sqlite"));
        QVERIFY(store.backup(portable, nullptr, ProjectStore::BackupMode::Portable));
        const QString copied = directory.filePath(QStringLiteral("copied.sqlite"));
        QVERIFY(ProjectStore(portable).backup(copied));
        const auto copiedAssets = ProjectStore(copied).assets();
        QCOMPARE(copiedAssets.size(), 1);
        const auto owned = copiedAssets.first().toObject();
        QVERIFY(!owned.value(QStringLiteral("external")).toBool());
        const QString ownedPath = ProjectStore(copied).resolveAudioPath(owned.value(QStringLiteral("path")).toString());
        QVERIFY(QFileInfo::exists(ownedPath));
        QCOMPARE(ProjectStore(copied).importAsset(ownedPath, QStringLiteral("media")), asset);
        QCOMPARE(ProjectStore(copied).assets().size(), 1);
    }
    void projectSessionHasSingleOwner()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("project.sqlite"));
        ProjectManager first;
        QVERIFY(first.create(path));
        ProjectManager second;
        QString error;
        QVERIFY(!second.open(path, ProjectStorageAdapter::Format::UnifiedSqlite, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(first.open(path, ProjectStorageAdapter::Format::UnifiedSqlite));
        first.close();
        QVERIFY(second.open(path, ProjectStorageAdapter::Format::UnifiedSqlite));
        QVERIFY(!first.open(path, ProjectStorageAdapter::Format::UnifiedSqlite));
        second.close();
        QVERIFY(first.open(path, ProjectStorageAdapter::Format::UnifiedSqlite));
    }
    void convertedModuleUsesPortableAssets()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("old.subcue"));
        QFile media(directory.filePath(QStringLiteral("video.mp4")));
        QVERIFY(media.open(QIODevice::WriteOnly));
        QVERIFY(media.write("media storage fixture") > 0);
        media.close();
        Project project;
        project.mediaPath = media.fileName();
        project.mediaFingerprint.insert(QStringLiteral("sha256"), QString::fromLatin1(RoughCutProjectSerializer::mediaSha256(media.fileName()).toHex()));
        project.state.insert(QStringLiteral("assets"), QJsonArray{QJsonObject{{QStringLiteral("path"), media.fileName()},
            {QStringLiteral("kind"), QStringLiteral("视频")}, {QStringLiteral("custom"), QStringLiteral("保留")},
            {QStringLiteral("assetId"), QStringLiteral("原格式自定义字段")}}});
        QVERIFY(ProjectSerializer::save(source, project));
        ProjectManager manager;
        const QString converted = directory.filePath(QStringLiteral("converted.sqlite"));
        QVERIFY(manager.convert(source, ProjectStorageAdapter::Format::SubtitleJson, converted));
        QCOMPARE(ProjectStore(converted).assets().size(), 1);
        const QString portable = directory.filePath(QStringLiteral("portable.sqlite"));
        QVERIFY(ProjectStore(converted).backup(portable, nullptr, ProjectStore::BackupMode::Portable));
        QVERIFY(media.remove());
        const auto loaded = ProjectStorageAdapter(portable, ProjectStorageAdapter::Format::UnifiedSqlite).loadModule(QStringLiteral("subtitle"));
        QVERIFY(loaded);
        const auto restored = std::get<Project>(*loaded);
        QVERIFY(restored.mediaPath != media.fileName());
        QFile owned(restored.mediaPath);
        QVERIFY(owned.open(QIODevice::ReadOnly));
        QCOMPARE(owned.readAll(), QByteArray("media storage fixture"));
        QCOMPARE(restored.state.value(QStringLiteral("assets")).toArray().first().toObject().value(QStringLiteral("path")).toString(), restored.mediaPath);
        QCOMPARE(restored.state.value(QStringLiteral("assets")).toArray().first().toObject().value(QStringLiteral("custom")).toString(), QStringLiteral("保留"));
        QCOMPARE(restored.state.value(QStringLiteral("assets")).toArray().first().toObject().value(QStringLiteral("assetId")).toString(), QStringLiteral("原格式自定义字段"));
        // 同一路径后来出现不同内容，不得悄悄绑定为旧工程的原素材。
        QVERIFY(media.open(QIODevice::WriteOnly));
        QVERIFY(media.write("changed") > 0);
        media.close();
        const auto generation = manager.generation();
        const QString rejected = directory.filePath(QStringLiteral("rejected.sqlite"));
        QVERIFY(!manager.convert(source, ProjectStorageAdapter::Format::SubtitleJson, rejected));
        QVERIFY(!QFileInfo::exists(rejected));
        QCOMPARE(manager.generation(), generation);
    }
    void legacyConversionPreservesModulesAndSource()
    {
        QTemporaryDir directory;
        for (const auto format : {ProjectStorageAdapter::Format::SubtitleJson, ProjectStorageAdapter::Format::RoughCutSqlite}) {
            const bool subtitle = format == ProjectStorageAdapter::Format::SubtitleJson;
            const QString source = directory.filePath(subtitle ? QStringLiteral("old.subcue") : QStringLiteral("old.roughcut"));
            const QString destination = directory.filePath(subtitle ? QStringLiteral("subtitle.sqlite") : QStringLiteral("roughcut.sqlite"));
            if (subtitle) {
                Project project;
                project.mediaPath = directory.filePath(QStringLiteral("offline.mp4"));
                project.metadata.insert(QStringLiteral("custom"), QStringLiteral("保留"));
                project.metadata.insert(QStringLiteral("projectIdentity"), QJsonObject{{QStringLiteral("id"), QStringLiteral("legacy-subtitle")}});
                project.state.insert(QStringLiteral("scriptText"), QStringLiteral("手动编辑的文稿"));
                Subtitle cue;
                cue.id = QStringLiteral("subtitle-stable-id");
                cue.text = QStringLiteral("人工修改的字幕");
                cue.start = MediaTime::fromMicroseconds(1);
                cue.end = MediaTime::fromMicroseconds(3'333'333);
                cue.metadata.insert(QStringLiteral("custom"), QStringLiteral("保留"));
                project.tracks.append({QStringLiteral("main"), QStringLiteral("字幕"), {cue}, {}});
                QVERIFY(ProjectSerializer::save(source, project));
            } else {
                RoughCutProject project;
                project.state.insert(QStringLiteral("custom"), QStringLiteral("保留"));
                project.state.insert(QStringLiteral("projectIdentity"), QJsonObject{{QStringLiteral("id"), QStringLiteral("legacy-roughcut")}});
                project.scriptText = QStringLiteral("手动编辑的文稿");
                project.sourceSampleCount = 50'000;
                project.sampleRate = 24'000;
                project.channels = 1;
                project.recording.append({QStringLiteral("segment-stable-id"), QStringLiteral("人工决定的片段"), 100, 250});
                RoughCutSegmentDecision decision;
                decision.recordingIndex = 0;
                decision.userDecision = RoughCutDecision::Keep;
                decision.reason = QStringLiteral("人工保留");
                project.decisions.append(decision);
                project.state.insert(QStringLiteral("timeline"), QJsonArray{QJsonObject{
                    {QStringLiteral("start"), 100}, {QStringLiteral("end"), 250}, {QStringLiteral("timelineStart"), 0},
                    {QStringLiteral("recording"), 0}, {QStringLiteral("decision"), int(RoughCutDecision::Keep)}}});
                QVERIFY(RoughCutProjectSerializer::save(source, project));
            }
            const auto before = RoughCutProjectSerializer::mediaSha256(source);
            const auto legacy = ProjectStorageAdapter(source, format).load();
            QVERIFY(legacy);
            ProjectManager manager;
            QString error;
            const auto converted = manager.convert(source, format, destination, &error);
            QVERIFY2(converted, qPrintable(error));
            QVERIFY(converted->projectId != legacy->projectId);
            QCOMPARE(RoughCutProjectSerializer::mediaSha256(source), before);
            const auto snapshot = std::get<ProjectStore::Snapshot>(converted->data);
            QCOMPARE(snapshot.modules.value(QStringLiteral("provenance")).toObject().value(QStringLiteral("sourceProjectId")).toString(), legacy->projectId);
            const auto loaded = ProjectStorageAdapter(destination, ProjectStorageAdapter::Format::UnifiedSqlite)
                .loadModule(subtitle ? QStringLiteral("subtitle") : QStringLiteral("roughcut"), &error);
            QVERIFY2(loaded, qPrintable(error));
            if (subtitle) {
                const auto project = std::get<Project>(*loaded);
                QCOMPARE(project.metadata.value(QStringLiteral("custom")).toString(), QStringLiteral("保留"));
                QCOMPARE(project.mediaPath, directory.filePath(QStringLiteral("offline.mp4")));
                QCOMPARE(project.state.value(QStringLiteral("scriptText")).toString(), QStringLiteral("手动编辑的文稿"));
                QCOMPARE(project.tracks.first().subtitles.first().id, QStringLiteral("subtitle-stable-id"));
                QCOMPARE(project.tracks.first().subtitles.first().start.microseconds(), qint64(1));
                QCOMPARE(project.tracks.first().subtitles.first().end.microseconds(), qint64(3'333'333));
            } else {
                const auto project = std::get<RoughCutProject>(*loaded);
                QCOMPARE(project.state.value(QStringLiteral("custom")).toString(), QStringLiteral("保留"));
                QCOMPARE(project.scriptText, QStringLiteral("手动编辑的文稿"));
                QCOMPARE(project.recording.first().id, QStringLiteral("segment-stable-id"));
                QCOMPARE(project.recording.first().startSample, qint64(100));
                QCOMPARE(project.recording.first().endSample, qint64(250));
                QCOMPARE(project.decisions.first().userDecision, std::optional<RoughCutDecision>(RoughCutDecision::Keep));
                QCOMPARE(project.state.value(QStringLiteral("timeline")).toArray().first().toObject().value(QStringLiteral("end")).toInt(), 250);
            }
            const auto generation = manager.generation();
            QVERIFY(!manager.convert(source, format, destination));
            QCOMPARE(manager.generation(), generation);
        }
        QVERIFY(!ProjectStorageAdapter::unpackModule({{QStringLiteral("snapshotVersion"), 1},
            {QStringLiteral("format"), QStringLiteral("subtitle-json")}, {QStringLiteral("payloadBase64"), QStringLiteral("invalid?!")}}));
    }
    void assetsAndBackupModes()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        QFile reference(directory.filePath(QStringLiteral("reference.wav")));
        QVERIFY(reference.open(QIODevice::WriteOnly));
        QVERIFY(reference.write("reference storage fixture") > 0);
        reference.close();
        QFile media(directory.filePath(QStringLiteral("media.mp4")));
        QVERIFY(media.open(QIODevice::WriteOnly));
        QVERIFY(media.write("media storage fixture") > 0);
        media.close();
        QString error;
        QVERIFY2(store.importAsset(reference.fileName(), QStringLiteral("reference"), &error), qPrintable(error));
        QVERIFY(store.importAsset(media.fileName(), QStringLiteral("media")));
        const auto duplicate = store.importAsset(media.fileName(), QStringLiteral("media"));
        QVERIFY(duplicate);
        QVERIFY(!store.importAsset(reference.fileName(), QStringLiteral("unknown")));
        QCOMPARE(store.assets().size(), 2);
        QVERIFY(reference.remove());
        const QString referenceBackup = directory.filePath(QStringLiteral("reference.sqlite"));
        const QString portableBackup = directory.filePath(QStringLiteral("portable.sqlite"));
        QVERIFY2(store.backup(referenceBackup, &error), qPrintable(error));
        QVERIFY2(store.backup(portableBackup, &error, ProjectStore::BackupMode::Portable), qPrintable(error));
        QVERIFY(media.remove());
        ProjectStore portable(portableBackup);
        for (const auto &asset : portable.assets()) {
            QVERIFY(!asset.toObject().value(QStringLiteral("external")).toBool());
            QVERIFY(QFileInfo::exists(portable.resolveAudioPath(asset.toObject().value(QStringLiteral("path")).toString())));
        }
        // 引用模式允许外部视频离线；声音参考仍必须包含。
        QVERIFY(ProjectStore(referenceBackup).backup(directory.filePath(QStringLiteral("offline.sqlite"))));
        QVERIFY(!ProjectStore(referenceBackup).backup(directory.filePath(QStringLiteral("missing-media.sqlite")),
            &error, ProjectStore::BackupMode::Portable));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("missing-media.sqlite"))));
        const QString restored = directory.filePath(QStringLiteral("restored.sqlite"));
        QVERIFY(portable.backup(restored, &error, ProjectStore::BackupMode::Portable));
        QCOMPARE(ProjectStore(restored).load()->projectId, portable.load()->projectId);
        std::atomic<bool> canceled{true};
        QVERIFY(!store.backup(directory.filePath(QStringLiteral("canceled.sqlite")), &error,
            ProjectStore::BackupMode::Portable, &canceled));
        QVERIFY(!store.importAsset(directory.filePath(QStringLiteral("absent.wav")), QStringLiteral("reference"), &error, &canceled));
        QCOMPARE(store.assets().size(), 2);
    }
    void schemaOneOpenIsReadOnlyAndSaveMigrates()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("old.sqlite"));
        ProjectStore store(path);
        QVERIFY(store.create());
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("schema-one-fixture"));
            database.setDatabaseName(path);
            QVERIFY(database.open());
            {
                QSqlQuery query(database);
                QVERIFY(query.exec(QStringLiteral("DROP TABLE assets")));
                QVERIFY(query.exec(QStringLiteral("UPDATE project SET schema_version=1")));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("schema-one-fixture"));
        const auto before = RoughCutProjectSerializer::mediaSha256(path);
        const auto snapshot = store.load();
        QVERIFY(snapshot);
        QVERIFY(store.assets().isEmpty());
        QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), before);
        QVERIFY(store.save(*snapshot));
        QCOMPARE(store.load()->revision, qint64(1));
        QVERIFY(store.assets().isEmpty());
    }
    void restoreFailurePreservesCurrentSession()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.sqlite"));
        QVERIFY(ProjectStore(source).create());
        ProjectManager manager;
        QVERIFY(manager.create(directory.filePath(QStringLiteral("current.sqlite"))));
        const auto id = manager.projectId();
        const auto generation = manager.generation();
        QVERIFY(!manager.restore(source, manager.path()));
        QCOMPARE(manager.projectId(), id);
        QCOMPARE(manager.generation(), generation);
        std::atomic<bool> canceled{true};
        const QString canceledPath = directory.filePath(QStringLiteral("canceled.sqlite"));
        QVERIFY(!manager.restore(source, canceledPath, nullptr,
            ProjectStore::BackupMode::ExternalMediaReferences, &canceled));
        QVERIFY(!QFileInfo::exists(canceledPath));
        QCOMPARE(manager.generation(), generation);
        const auto restored = manager.restore(source, directory.filePath(QStringLiteral("restored.sqlite")));
        QVERIFY(restored);
        QCOMPARE(restored->projectId, ProjectStore(source).load()->projectId);
        QVERIFY(manager.generation() > generation);
    }
    void damagedReferenceDoesNotPublishBackup()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        QFile reference(directory.filePath(QStringLiteral("reference.wav")));
        QVERIFY(reference.open(QIODevice::WriteOnly));
        QVERIFY(reference.write("original") > 0);
        reference.close();
        QVERIFY(store.importAsset(reference.fileName(), QStringLiteral("reference")));
        QFile owned(store.resolveAudioPath(store.assets().first().toObject().value(QStringLiteral("path")).toString()));
        QVERIFY(owned.open(QIODevice::WriteOnly));
        QVERIFY(owned.write("damaged") > 0);
        owned.close();
        const QString target = directory.filePath(QStringLiteral("backup.sqlite"));
        QVERIFY(!store.backup(target));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".assets")));
        QCOMPARE(store.load()->revision, qint64(0));
    }
    void damagedGeneratedAudioDoesNotPublishBackup()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        QFile audio(directory.filePath(QStringLiteral("audio.wav")));
        QVERIFY(audio.open(QIODevice::WriteOnly));
        QVERIFY(audio.write("audio storage fixture") > 0);
        audio.close();
        QVERIFY(store.enqueue(QStringLiteral("job"), {{QStringLiteral("text"), QStringLiteral("输入")}}));
        QVERIFY(store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName()));
        QVERIFY(audio.remove());
        const QString ownedPath = store.resolveAudioPath(store.history().first().toObject().value(QStringLiteral("audioPath")).toString());
        QVERIFY(QFileInfo::exists(ownedPath));
        QFile owned(ownedPath);
        QVERIFY(owned.open(QIODevice::WriteOnly));
        QVERIFY(owned.write("damaged") > 0);
        owned.close();
        const QString target = directory.filePath(QStringLiteral("backup.sqlite"));
        QVERIFY(!store.backup(target));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral(".assets")));
        QCOMPARE(store.history().size(), 1);
    }
    void committedRevisionSupportsRepeatedCheckpoints()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        auto snapshot = store.load();
        QVERIFY(snapshot);
        const auto stale = *snapshot;
        qint64 committedRevision = -1;
        snapshot->modules.insert(QStringLiteral("voice"), QJsonObject{{QStringLiteral("text"), QStringLiteral("初稿")}});
        std::atomic<bool> canceled{true};
        QVERIFY(!store.save(*snapshot, nullptr, &committedRevision, &canceled));
        QCOMPARE(committedRevision, qint64(-1));
        QCOMPARE(store.load()->revision, qint64(0));
        QVERIFY(store.load()->modules.isEmpty());
        auto invalid = *snapshot;
        invalid.revision = std::numeric_limits<qint64>::max();
        QVERIFY(!store.save(invalid));
        QVERIFY(store.save(*snapshot, nullptr, &committedRevision));
        QCOMPARE(committedRevision, qint64(1));
        // 保存只提交捕获的检查点，不覆盖之后继续编辑的文稿。
        snapshot->modules.insert(QStringLiteral("voice"), QJsonObject{{QStringLiteral("text"), QStringLiteral("修改稿")}});
        snapshot->revision = committedRevision;
        QVERIFY(store.enqueue(QStringLiteral("job"), {{QStringLiteral("text"), QStringLiteral("初稿")}}));
        QVERIFY(store.save(*snapshot, nullptr, &committedRevision));
        QCOMPARE(committedRevision, qint64(2));
        QVERIFY(!store.save(stale, nullptr, &committedRevision));
        QCOMPARE(committedRevision, qint64(2));
        QCOMPARE(store.load()->modules, snapshot->modules);
    }
    void createPreservesSessionOnFailure()
    {
        QTemporaryDir directory;
        ProjectManager manager;
        const QString path = directory.filePath(QStringLiteral("project.sqlite"));
        const auto snapshot = manager.create(path);
        QVERIFY(snapshot);
        QCOMPARE(manager.projectId(), snapshot->projectId);
        const auto generation = manager.generation();
        QVERIFY(!manager.create(path));
        QCOMPARE(manager.generation(), generation);
        QCOMPARE(manager.path(), path);
        QVERIFY(manager.save(*snapshot));
    }
    void managerReturnsCommittedRevision()
    {
        QTemporaryDir directory;
        ProjectManager manager;
        auto snapshot = manager.create(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(snapshot);
        ProjectStorageAdapter::SaveResult result;
        QVERIFY(manager.save(*snapshot, nullptr, nullptr, &result));
        QCOMPARE(result.revision, std::optional<qint64>(1));
        snapshot->storageVersion = QByteArray::number(*result.revision);
        std::get<ProjectStore::Snapshot>(snapshot->data).revision = *result.revision;
        QVERIFY(manager.save(*snapshot, nullptr, nullptr, &result));
        QCOMPARE(result.revision, std::optional<qint64>(2));
        QVERIFY(!manager.save(*snapshot, nullptr, nullptr, &result));
        QCOMPARE(result.revision, std::optional<qint64>(2));
    }
    void backupOwnsGeneratedAudio()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("project.sqlite"));
        ProjectStore store(path);
        QString error;
        QVERIFY2(store.create(&error), qPrintable(error));
        QFile audio(directory.filePath(QStringLiteral("original.wav")));
        QVERIFY(audio.open(QIODevice::WriteOnly));
        QCOMPARE(audio.write("audio fixture"), qint64(13));
        audio.close();
        QVERIFY(store.enqueue(QStringLiteral("job"), {{QStringLiteral("text"), QStringLiteral("旁白")}}));
        QVERIFY(store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName()));
        const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite"));
        QVERIFY2(store.backup(backupPath, &error), qPrintable(error));
        QVERIFY(audio.remove());
        ProjectStore backup(backupPath);
        const auto history = backup.history();
        QCOMPARE(history.size(), 1);
        const QString stored = history.first().toObject().value(QStringLiteral("audioPath")).toString();
        QVERIFY(QFileInfo(stored).isRelative());
        QFile restored(backup.resolveAudioPath(stored));
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("audio fixture"));
        QVERIFY(!backup.backup(backupPath));
        const QString missingPath = directory.filePath(QStringLiteral("missing.sqlite"));
        QVERIFY(QFile::remove(store.resolveAudioPath(store.history().first().toObject().value(QStringLiteral("audioPath")).toString())));
        QVERIFY(!store.backup(missingPath, &error));
        QVERIFY(!QFileInfo::exists(missingPath));
        QVERIFY(!QFileInfo::exists(missingPath + QStringLiteral(".assets")));
    }
    void failedOpenPreservesSession()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("project.sqlite"));
        QVERIFY(ProjectStore(path).create());
        ProjectManager manager;
        const auto snapshot = manager.open(path, ProjectStorageAdapter::Format::UnifiedSqlite);
        QVERIFY(snapshot);
        const auto generation = manager.generation();
        QVERIFY(!manager.open(directory.filePath(QStringLiteral("missing.sqlite")), ProjectStorageAdapter::Format::UnifiedSqlite));
        QCOMPARE(manager.generation(), generation);
        QCOMPARE(manager.projectId(), snapshot->projectId);
        QVERIFY(manager.save(*snapshot));
        const auto reopened = manager.open(path, ProjectStorageAdapter::Format::UnifiedSqlite);
        QVERIFY(reopened);
        QVERIFY(!manager.save(*snapshot));
        QVERIFY(manager.save(*reopened));
        manager.close();
        QVERIFY(!manager.save(*snapshot));
        QVERIFY(manager.generation() > generation);
    }
    void legacyAdaptersPreserveIdentityAndRejectConflict()
    {
        QTemporaryDir directory;
        for (const auto format : {ProjectStorageAdapter::Format::SubtitleJson,
                                  ProjectStorageAdapter::Format::RoughCutSqlite}) {
            const QString path = directory.filePath(format == ProjectStorageAdapter::Format::SubtitleJson
                ? QStringLiteral("subtitle.subcue") : QStringLiteral("rough.subcue-roughcut"));
            if (format == ProjectStorageAdapter::Format::SubtitleJson) {
                Project project;
                project.metadata.insert(QStringLiteral("custom"), QStringLiteral("保留"));
                QVERIFY(ProjectSerializer::save(path, project));
            } else {
                RoughCutProject project;
                project.state.insert(QStringLiteral("custom"), QStringLiteral("保留"));
                QVERIFY(RoughCutProjectSerializer::save(path, project));
            }
            const auto before = RoughCutProjectSerializer::mediaSha256(path);
            ProjectStorageAdapter adapter(path, format);
            auto snapshot = adapter.load();
            QVERIFY(snapshot);
            QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), before);
            QVERIFY(!adapter.supports(QStringLiteral("voice")));
            QVERIFY(adapter.save(*snapshot));
            const auto loaded = adapter.load();
            QVERIFY(loaded);
            QCOMPARE(loaded->projectId, snapshot->projectId);
            if (format == ProjectStorageAdapter::Format::SubtitleJson)
                QCOMPARE(std::get<Project>(loaded->data).metadata.value(QStringLiteral("custom")).toString(), QStringLiteral("保留"));
            else
                QCOMPARE(std::get<RoughCutProject>(loaded->data).state.value(QStringLiteral("custom")).toString(), QStringLiteral("保留"));
            QVERIFY(!adapter.save(*snapshot));
            auto current = *loaded;
            ProjectStorageAdapter::SaveResult saved;
            QVERIFY(adapter.save(current, nullptr, nullptr, &saved));
            QVERIFY(!saved.revision);
            QCOMPARE(saved.storageVersion, RoughCutProjectSerializer::mediaSha256(path));
            current.storageVersion = saved.storageVersion;
            QVERIFY(adapter.save(current, nullptr, nullptr, &saved));
            QCOMPARE(saved.storageVersion, RoughCutProjectSerializer::mediaSha256(path));
            current.storageVersion = saved.storageVersion;
            QFile external(path);
            QVERIFY(external.open(QIODevice::Append));
            QVERIFY(external.write("external change") > 0);
            external.close();
            const auto externalVersion = RoughCutProjectSerializer::mediaSha256(path);
            QVERIFY(!adapter.save(current, nullptr, nullptr, &saved));
            QCOMPARE(saved.storageVersion, current.storageVersion);
            QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), externalVersion);
            const auto stable = RoughCutProjectSerializer::mediaSha256(path);
            std::atomic<bool> canceled{true};
            QVERIFY(!adapter.save(*loaded, nullptr, &canceled));
            QCOMPARE(RoughCutProjectSerializer::mediaSha256(path), stable);
        }
    }
    void unifiedAdapterPreservesGeneratedHistory()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("project.sqlite"));
        QVERIFY(ProjectStore(path).create());
        ProjectStorageAdapter adapter(path, ProjectStorageAdapter::Format::UnifiedSqlite);
        auto snapshot = adapter.load();
        QVERIFY(snapshot);
        QVERIFY(adapter.supports(QStringLiteral("voice")));
        auto &data = std::get<ProjectStore::Snapshot>(snapshot->data);
        data.modules.insert(QStringLiteral("subtitle"), QJsonObject{{QStringLiteral("text"), QStringLiteral("正文")}});
        QVERIFY(ProjectStore(path).enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("输入")}}));
        QVERIFY(adapter.save(*snapshot));
        QVERIFY(!adapter.save(*snapshot));
        QCOMPARE(ProjectStore(path).load()->modules, data.modules);
    }
    void checkpointPreservesHistory()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QString error;
        QVERIFY2(store.create(&error), qPrintable(error));
        auto snapshot = store.load(&error);
        QVERIFY(snapshot);
        QVERIFY(!snapshot->projectId.isEmpty());
        snapshot->modules.insert(QStringLiteral("voice"), QJsonObject{{QStringLiteral("spokenText"), QStringLiteral("测试旁白")}});
        QVERIFY(store.enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("尚未保存的文本")}}, &error));
        QFile audio(directory.filePath(QStringLiteral("take.wav")));
        QVERIFY(audio.open(QIODevice::WriteOnly));
        QCOMPARE(audio.write("fixture"), qint64(7));
        audio.close();
        QVERIFY2(store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName(), &error), qPrintable(error));
        QVERIFY2(store.save(*snapshot, &error), qPrintable(error));
        QCOMPARE(store.history().size(), 1);
        QCOMPARE(store.load()->revision, qint64(1));
        QVERIFY(!store.save(*snapshot, &error));
        QCOMPARE(store.load()->modules, snapshot->modules);
        QVERIFY(!store.cancel(QStringLiteral("job"), &error));
        QVERIFY(store.backup(directory.filePath(QStringLiteral("backup.sqlite")), &error));
        ProjectStore backup(directory.filePath(QStringLiteral("backup.sqlite")));
        QCOMPARE(backup.load()->projectId, snapshot->projectId);
        QCOMPARE(backup.history().size(), store.history().size());
        QCOMPARE(backup.history().first().toObject().value(QStringLiteral("input")),
                 store.history().first().toObject().value(QStringLiteral("input")));
    }
    void cancellationRejectsLateResult()
    {
        QTemporaryDir directory;
        ProjectStore store(directory.filePath(QStringLiteral("project.sqlite")));
        QVERIFY(store.create());
        QVERIFY(store.enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("取消")}}));
        QVERIFY(store.cancel(QStringLiteral("job")));
        QFile audio(directory.filePath(QStringLiteral("take.wav")));
        QVERIFY(audio.open(QIODevice::WriteOnly));
        QVERIFY(audio.write("fixture") > 0); audio.close();
        QVERIFY(!store.finish(QStringLiteral("job"), QStringLiteral("take"), audio.fileName()));
        QVERIFY(store.history().isEmpty());
        QCOMPARE(store.load()->revision, qint64(0));
    }
    void missingStoreDoesNotCreateFile()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("missing.sqlite"));
        ProjectStore store(path);
        QVERIFY(!store.load());
        QVERIFY(!store.enqueue(QStringLiteral("job"), QJsonObject{{QStringLiteral("text"), QStringLiteral("测试")}}));
        QVERIFY(!QFile::exists(path));
    }
};
QTEST_GUILESS_MAIN(ProjectStoreTests)
#include "test_project_store.moc"
