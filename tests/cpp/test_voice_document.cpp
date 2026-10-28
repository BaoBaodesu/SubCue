#include "tts/voice_document.h"
#include "tts/tts_types.h"
#include "project/project_store.h"
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtCore/QUuid>
using namespace subcue;
class VoiceDocumentTests final : public QObject {
    Q_OBJECT
private slots:
    void defaultVoiceOnlyInvalidatesInheritedSentences()
    {
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("旁白。局部角色。"));
        auto items = document.value(QStringLiteral("segments")).toArray();
        auto local = items[1].toObject();
        local.insert(QStringLiteral("voiceVersionId"), QUuid::createUuid().toString()); items[1] = local;
        document.insert(QStringLiteral("segments"), items);
        const auto originalKey = VoiceDocument::inputKey(document, items[0].toObject());
        const auto localKey = VoiceDocument::inputKey(document, local);
        const auto voice = QUuid::createUuid().toString();
        const auto changed = VoiceDocument::setDefaultVoice(document, voice);
        QVERIFY(VoiceDocument::validate(changed));
        const auto after = changed.value(QStringLiteral("segments")).toArray();
        QCOMPARE(VoiceDocument::effectiveVoiceVersion(changed, after[0].toObject()), voice);
        QVERIFY(originalKey != VoiceDocument::inputKey(changed, after[0].toObject()));
        QCOMPARE(VoiceDocument::inputKey(changed, after[1].toObject()), localKey);
        QCOMPARE(after[1], items[1]);
        QCOMPARE(VoiceDocument::setDefaultVoice(changed, voice), changed);
        auto legacy = document; legacy.insert(QStringLiteral("version"), 1); legacy.remove(QStringLiteral("defaultVoiceVersionId"));
        QVERIFY(VoiceDocument::validate(legacy));
        QCOMPARE(VoiceDocument::inputKey(legacy, local), localKey);
    }
    void languageInputsPreserveLegacyHashesAndRejectUnsupportedProviders()
    {
        TtsRequest request; request.projectId = request.segmentId = request.voiceVersionId = request.referenceAssetId = QUuid::createUuid().toString();
        request.projectGeneration = 1; request.provider = QStringLiteral("qwen-local"); request.model = QStringLiteral("base"); request.modelVersion = QStringLiteral("v1");
        request.language = QStringLiteral("English"); request.spokenText = QStringLiteral("The weather is nice today."); request.referenceText = QStringLiteral("中文参考"); request.referenceSha256 = QByteArray(32, 'a');
        TtsCapabilities caps{request.provider, request.model, {QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")}, {QStringLiteral("seed")}, false, true, true, false};
        QString error; auto legacy = request.snapshot(); legacy.remove(QStringLiteral("referenceLanguage"));
        const auto restored = TtsRequest::fromSnapshot(legacy, caps, &error); QVERIFY2(restored, qPrintable(error)); QCOMPARE(restored->inputHash(), request.inputHash());
        request.referenceLanguage = QStringLiteral("Japanese"); QVERIFY(request.inputHash() != restored->inputHash()); QVERIFY(TtsRequest::fromSnapshot(request.snapshot(), caps));
        auto tampered = request.snapshot(); tampered.insert(QStringLiteral("referenceLanguage"), QStringLiteral("English")); QVERIFY(!TtsRequest::fromSnapshot(tampered, caps));
        caps.languages = {QStringLiteral("Chinese")}; QVERIFY(!request.validate(caps, &error));
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。")); auto items = document.value(QStringLiteral("segments")).toArray();
        const auto original = items[0].toObject(); auto changed = original; changed.insert(QStringLiteral("language"), QStringLiteral("English"));
        QVERIFY(VoiceDocument::inputKey(document, changed) != VoiceDocument::inputKey(document, original)); items[0] = changed; document.insert(QStringLiteral("segments"), items);
        QVERIFY(VoiceDocument::validate(document)); QCOMPARE(document.value(QStringLiteral("segments")).toArray()[1], items[1]);
    }
    void gptSpeedRoundTripsAndInvalidatesOnlyItsSentence()
    {
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。"));
        auto items = document.value(QStringLiteral("segments")).toArray(); const auto original = items[0].toObject(); const auto untouched = items[1];
        auto item = original; item.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("speedFactor"), 0.85}});
        items[0] = item; document.insert(QStringLiteral("segments"), items); QString error;
        QVERIFY2(VoiceDocument::validate(document, &error), qPrintable(error));
        QVERIFY(VoiceDocument::inputKey(document, original) != VoiceDocument::inputKey(document, item));
        QCOMPARE(document.value(QStringLiteral("segments")).toArray()[1], untouched);
        const auto restored = QJsonDocument::fromJson(QJsonDocument(document).toJson()).object();
        QVERIFY(VoiceDocument::validate(restored)); QCOMPARE(restored, document);
        for (const auto &invalid : {QJsonValue(0.49), QJsonValue(2.01), QJsonValue(true), QJsonValue(QStringLiteral("0.85")), QJsonValue(QJsonValue::Null)})
            QVERIFY(!validateTtsParameters({{QStringLiteral("speedFactor"), invalid}}, &error));
        TtsRequest request; request.projectId = request.segmentId = request.voiceVersionId = QUuid::createUuid().toString(); request.projectGeneration = 1;
        request.spokenText = QStringLiteral("第一句。"); request.language = QStringLiteral("Chinese"); request.provider = QStringLiteral("gpt-local"); request.model = QStringLiteral("GPT-SoVITS-v2ProPlus"); request.modelVersion = QStringLiteral("v1");
        const auto originalHash = request.inputHash(); request.effectiveParameters = item.value(QStringLiteral("parameters")).toObject();
        QVERIFY(request.inputHash() != originalHash);
        TtsCapabilities caps{request.provider, request.model, {request.language}, {QStringLiteral("seed"), QStringLiteral("speedFactor")}};
        QVERIFY(request.validate(caps, &error)); caps.parameters = {QStringLiteral("seed")}; QVERIFY(!request.validate(caps, &error));
        request.effectiveParameters = {}; QCOMPARE(request.inputHash(), originalHash);
    }
    void indexControlsRoundTripAndInvalidateOnlyTheirSentence()
    {
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。"));
        auto items = document.value(QStringLiteral("segments")).toArray(); const auto original = items[0].toObject(); const auto untouched = items[1];
        auto item = original; item.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("durationFactor"), 1.15}, {QStringLiteral("emotion"), QStringLiteral("happy")}, {QStringLiteral("emotionStrength"), 0.2}});
        items[0] = item; document.insert(QStringLiteral("segments"), items); QString error;
        QVERIFY2(VoiceDocument::validate(document, &error), qPrintable(error));
        QVERIFY(VoiceDocument::inputKey(document, original) != VoiceDocument::inputKey(document, item)); QCOMPARE(items[1], untouched);
        QVERIFY(VoiceDocument::validate(QJsonDocument::fromJson(QJsonDocument(document).toJson()).object()));
        for (const auto &invalid : {QJsonObject{{QStringLiteral("durationFactor"), 0.49}}, QJsonObject{{QStringLiteral("durationFactor"), QStringLiteral("1.2")}},
             QJsonObject{{QStringLiteral("emotion"), QStringLiteral("happy")}}, QJsonObject{{QStringLiteral("emotion"), QStringLiteral("unknown")}, {QStringLiteral("emotionStrength"), 0.2}},
             QJsonObject{{QStringLiteral("emotion"), QStringLiteral("happy")}, {QStringLiteral("emotionStrength"), 0.0}}}) QVERIFY(!validateTtsParameters(invalid, &error));
        TtsRequest request; request.projectId = request.segmentId = request.voiceVersionId = QUuid::createUuid().toString(); request.projectGeneration = 1;
        request.spokenText = QStringLiteral("第一句。"); request.language = QStringLiteral("Chinese"); request.provider = QStringLiteral("test"); request.model = QStringLiteral("test"); request.modelVersion = QStringLiteral("v1"); request.effectiveParameters = item.value(QStringLiteral("parameters")).toObject();
        TtsCapabilities caps{request.provider, request.model, {request.language}, {QStringLiteral("seed")}};
        QVERIFY(!request.validate(caps, &error)); caps.parameters += QStringList{QStringLiteral("durationFactor"), QStringLiteral("emotion"), QStringLiteral("emotionStrength")};
        QVERIFY2(request.validate(caps, &error), qPrintable(error));
        request.spokenText = QStringLiteral("小<变化|BIAN4 HUA4>。"); QVERIFY(!request.validate(caps, &error));
        request.provider = caps.provider = QStringLiteral("index-local"); QVERIFY(request.validate(caps, &error));
    }
    void normalizationAndStructuredImportRemainExplicitAndIsolated()
    {
        QCOMPARE(VoiceDocument::normalizeChinese(QStringLiteral("2026-10-13温度20℃，电量25%。")), QStringLiteral("二零二六年十月十三日温度二十摄氏度，电量百分之二十五。"));
        const auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("原段落。"));
        auto incoming = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("外来正文。"));
        const auto originalId = incoming.value(QStringLiteral("segments")).toArray()[0].toObject().value(QStringLiteral("id"));
        incoming = VoiceDocument::edit(incoming, originalId.toString(), QStringLiteral("外来正文。"), QStringLiteral("外来手工朗读"), true, QStringLiteral("备注"));
        incoming.insert(QStringLiteral("originProjectId"), QUuid::createUuid().toString(QUuid::WithoutBraces)); QString error;
        const auto appended = VoiceDocument::appendStructured(document, incoming, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(appended.value(QStringLiteral("segments")).toArray()[0], document.value(QStringLiteral("segments")).toArray()[0]);
        const auto added = appended.value(QStringLiteral("segments")).toArray()[1].toObject(); QVERIFY(added.value(QStringLiteral("id")) != originalId);
        QCOMPARE(added.value(QStringLiteral("spoken")).toString(), QStringLiteral("外来手工朗读"));
        QCOMPARE(added.value(QStringLiteral("importOrigin")).toObject().value(QStringLiteral("segmentId")), originalId);
    }
    void editsProtectManualSpokenAndOnlyInvalidateEffectiveInputs()
    {
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。\n第三句。"));
        QVERIFY(VoiceDocument::validate(document)); const auto items = document.value(QStringLiteral("segments")).toArray(); QCOMPARE(items.size(), 3);
        const auto id = items[0].toObject().value(QStringLiteral("id")).toString();
        document = VoiceDocument::edit(document, id, QStringLiteral("正文"), QStringLiteral("手工朗读"), true, {});
        const auto before = document.value(QStringLiteral("segments")).toArray()[0].toObject();
        const auto key = VoiceDocument::inputKey(document, before);
        const auto updated = VoiceDocument::edit(document, id, QStringLiteral("另一个正文"), QStringLiteral("手工朗读"), true, QStringLiteral("未生效表演备注"));
        const auto first = updated.value(QStringLiteral("segments")).toArray()[0].toObject();
        QCOMPARE(VoiceDocument::inputKey(updated, first), key); QCOMPARE(first.value(QStringLiteral("inputVersion")), before.value(QStringLiteral("inputVersion")));
        QCOMPARE(updated.value(QStringLiteral("segments")).toArray()[1], items[1]);
        const auto split = VoiceDocument::split(updated, id, 2, nullptr); QVERIFY(VoiceDocument::validate(split));
        QCOMPARE(split.value(QStringLiteral("archivedSegments")).toArray()[0].toObject().value(QStringLiteral("spoken")).toString(), QStringLiteral("手工朗读"));
    }
    void resegmentationAndDirectedUpdatesPreserveUnrelatedAndManualData()
    {
        const auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。\n第三句。"));
        const auto items = document.value(QStringLiteral("segments")).toArray(); QString error;
        const auto paragraph = items[0].toObject().value(QStringLiteral("paragraphId")).toString();
        const auto changed = VoiceDocument::resegment(document, paragraph, QStringLiteral("重新断句！再一条。"));
        QCOMPARE(changed.value(QStringLiteral("segments")).toArray()[2], items[2]); QCOMPARE(changed.value(QStringLiteral("archivedSegments")).toArray().size(), 2);
        const auto project = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto incoming = QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("originProjectId"), project}, {QStringLiteral("segments"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), items[0].toObject().value(QStringLiteral("id"))}, {QStringLiteral("text"), QStringLiteral("JSON新正文")}, {QStringLiteral("spoken"), QStringLiteral("不能覆盖")}}}}};
        const auto id = items[0].toObject().value(QStringLiteral("id")).toString();
        auto manual = VoiceDocument::edit(document, id, QStringLiteral("旧正文"), QStringLiteral("手工修正"), true, {});
        QJsonArray differences; const auto result = VoiceDocument::targetedUpdate(manual, incoming, project, &differences, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value(QStringLiteral("segments")).toArray()[0].toObject().value(QStringLiteral("spoken")).toString(), QStringLiteral("手工修正"));
        QCOMPARE(result.value(QStringLiteral("segments")).toArray()[1], items[1]); QCOMPARE(differences.size(), 1);
        incoming.insert(QStringLiteral("originProjectId"), QUuid::createUuid().toString());
        QCOMPARE(VoiceDocument::targetedUpdate(manual, incoming, project, nullptr, &error), manual); QVERIFY(!error.isEmpty());
    }
    void takesRequireMatchingInputsAndExplicitCropPolicy()
    {
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("音频测试。"));
        auto items = document.value(QStringLiteral("segments")).toArray(); auto item = items[0].toObject(); const auto id = item.value(QStringLiteral("id")).toString();
        const auto key = QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex());
        QJsonObject take{{QStringLiteral("id"), QUuid::createUuid().toString()}, {QStringLiteral("inputKey"), key}, {QStringLiteral("sampleRate"), 24000}, {QStringLiteral("sampleCount"), QStringLiteral("48000")}};
        QString error; document = VoiceDocument::applyTake(document, id, take, {}, &error); QVERIFY(error.isEmpty());
        items = document.value(QStringLiteral("segments")).toArray(); item = items[0].toObject(); auto crop = item.value(QStringLiteral("selectedTake")).toObject();
        crop.insert(QStringLiteral("manualCrop"), true); crop.insert(QStringLiteral("trimStart"), QStringLiteral("2400")); crop.insert(QStringLiteral("trimEnd"), QStringLiteral("24000")); item.insert(QStringLiteral("selectedTake"), crop); items[0] = item; document.insert(QStringLiteral("segments"), items);
        auto shorter = take; shorter.insert(QStringLiteral("sampleCount"), QStringLiteral("12000"));
        QCOMPARE(VoiceDocument::applyTake(document, id, shorter, {}, &error), document); QVERIFY(error.contains(QStringLiteral("人工裁切")));
        error.clear(); QCOMPARE(VoiceDocument::applyTake(document, id, shorter, QStringLiteral("keep"), &error), document); QVERIFY(error.contains(QStringLiteral("超出")));
        error.clear(); const auto reset = VoiceDocument::applyTake(document, id, shorter, QStringLiteral("reset"), &error); QVERIFY(error.isEmpty()); QVERIFY(reset != document);
        take.insert(QStringLiteral("inputKey"), QString(64, u'0')); error.clear(); QCOMPARE(VoiceDocument::applyTake(document, id, take, {}, &error), document); QVERIFY(!error.isEmpty());
    }
    void sqliteStoresVoiceEntitiesOnceAndRollsBackStaleSaves()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("voice.sqlite")); ProjectStore store(path); QVERIFY(store.create());
        auto snapshot = *store.load(); const auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。"));
        snapshot.modules.insert(QStringLiteral("voiceStudio"), document); qint64 revision = -1; QVERIFY(store.save(snapshot, nullptr, &revision)); QCOMPARE(revision, 1);
        QCOMPARE(store.load()->modules.value(QStringLiteral("voiceStudio")).toObject(), document);
        QVERIFY(!store.save(snapshot)); QCOMPARE(store.load()->modules.value(QStringLiteral("voiceStudio")).toObject(), document);
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection); db.setDatabaseName(path); QVERIFY(db.open()); QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("SELECT modules,schema_version FROM project"))); QVERIFY(query.next()); QVERIFY(!query.value(0).toString().contains(QStringLiteral("第一句"))); QCOMPARE(query.value(1).toInt(), 5);
            QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM voice_segments"))); QVERIFY(query.next()); QCOMPARE(query.value(0).toInt(), 2);
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void migrationPreservesLegacyModulesAndRejectsBrokenReferences()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("upgrade.sqlite")); ProjectStore store(path); QVERIFY(store.create());
        auto snapshot = *store.load(); snapshot.modules.insert(QStringLiteral("voice"), QJsonObject{{QStringLiteral("legacy"), QStringLiteral("保留历史模块")}}); QVERIFY(store.save(snapshot));
        const auto job = QUuid::createUuid().toString(); QVERIFY(store.enqueue(job, {{QStringLiteral("input"), QStringLiteral("旧任务")}}));
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection); db.setDatabaseName(path); QVERIFY(db.open()); QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE voice_segments"))); QVERIFY(query.exec(QStringLiteral("DROP TABLE voice_paragraphs"))); QVERIFY(query.exec(QStringLiteral("DROP TABLE voice_document")));
            QVERIFY(query.exec(QStringLiteral("UPDATE project SET schema_version=4")));
        }
        QSqlDatabase::removeDatabase(connection);
        snapshot = *store.load(); const auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("迁移后的正文。")); snapshot.modules.insert(QStringLiteral("voiceStudio"), document); QVERIFY(store.save(snapshot));
        const auto migrated = *store.load(); QCOMPARE(migrated.modules.value(QStringLiteral("voice")), snapshot.modules.value(QStringLiteral("voice"))); QCOMPARE(store.jobs().size(), 1);
        auto invalid = document; auto items = invalid.value(QStringLiteral("segments")).toArray(); auto item = items[0].toObject(); item.insert(QStringLiteral("voiceVersionId"), QUuid::createUuid().toString()); items[0] = item; invalid.insert(QStringLiteral("segments"), items);
        snapshot = migrated; snapshot.modules.insert(QStringLiteral("voiceStudio"), invalid); QString error; QVERIFY(!store.save(snapshot, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(store.load()->modules, migrated.modules);
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection); db.setDatabaseName(path); QVERIFY(db.open()); QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("UPDATE voice_segments SET id='mismatched'")));
        }
        QSqlDatabase::removeDatabase(connection); error.clear(); QVERIFY(!store.load(&error)); QVERIFY(error.contains(QStringLiteral("关系引用")));
    }
};
QTEST_GUILESS_MAIN(VoiceDocumentTests)
#include "test_voice_document.moc"
