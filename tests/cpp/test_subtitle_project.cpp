#include "project/project_serializer.h"
#include "settings/settings_manager.h"
#include "subtitle/ass_writer.h"
#include "subtitle/srt_parser.h"
#include "subtitle/srt_writer.h"
#include "subtitle/subtitle_command_manager.h"
#include "subtitle/subtitle_model.h"
#include "subtitle/timecode.h"
#include "subtitle/txt_importer.h"
#include "platform/windows/dpapi_credential_store.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dpapi.h>
#endif

using namespace subcue;

namespace {

Subtitle makeSubtitle(QString id, QString text, qint64 startMs, qint64 endMs)
{
    Subtitle subtitle;
    subtitle.id = std::move(id);
    subtitle.text = std::move(text);
    subtitle.start = MediaTime::fromMilliseconds(startMs);
    subtitle.end = MediaTime::fromMilliseconds(endMs);
    subtitle.confidence = 0.96;
    subtitle.source = QStringLiteral("asr");
    subtitle.status = QStringLiteral("MATCHED");
    return subtitle;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

class SubtitleProjectTests final : public QObject {
    Q_OBJECT

private slots:
    void timecodeGolden()
    {
        QCOMPARE(timecode::formatSrt(MediaTime::fromMilliseconds(2'080)),
                 QStringLiteral("00:00:02,080"));
        QCOMPARE(timecode::formatAss(MediaTime::fromMilliseconds(3'425)),
                 QStringLiteral("0:00:03.42"));
        QCOMPARE(timecode::formatAss(MediaTime::fromMilliseconds(3'435)),
                 QStringLiteral("0:00:03.44"));
        const auto parsed = timecode::parseSrt(QStringLiteral("123:59:59.999"));
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->milliseconds(), 446'399'999);
        QVERIFY(!timecode::parseSrt(QStringLiteral("00:60:00,000")).has_value());
    }

    void textImportCompatibility()
    {
        QCOMPARE(TxtImporter::parse(QStringLiteral("\ufeff第一条\r\n\r\n 第二条 \n")),
                 QStringList({QStringLiteral("第一条"), QStringLiteral("第二条")}));
        const QString content = QStringLiteral(
            "1\n00:00:01,000 --> 00:00:02,000\n第一行\n第二行\n\n"
            "2\n00:00:08,000 --> 00:00:09,000\n旧时间会被忽略\n");
        QCOMPARE(SrtParser::parseTextOnly(content),
                 QStringList({QStringLiteral("第一行\n第二行"), QStringLiteral("旧时间会被忽略")}));
    }

    void timedSrtRoundTrip()
    {
        const QList<Subtitle> source{
            makeSubtitle(QStringLiteral("stable-a"), QStringLiteral("第一行\n第二行"), 1'000, 2'000),
            makeSubtitle(QStringLiteral("stable-b"), QStringLiteral("DLC 100% ~ iPhone18Pro"), 2'080, 3'424),
        };
        const QString built = SrtWriter::build(source);
        QCOMPARE(built.toUtf8(), readFile(QFINDTESTDATA("../golden/sample.srt")));
        QString error;
        const auto parsed = SrtParser::parseTimed(built, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->size(), 2);
        QCOMPARE(parsed->at(0).start.milliseconds(), 1'000);
        QCOMPARE(parsed->at(0).end.milliseconds(), 2'000);
        QCOMPARE(parsed->at(0).text, QStringLiteral("第一行\n第二行"));
    }

    void assGolden()
    {
        const QList<Subtitle> source{
            makeSubtitle(QStringLiteral("stable-b"), QStringLiteral("原始字幕"), 2'080, 3'424),
        };
        QCOMPARE(AssWriter::build(source, 3840, 2160, SettingsManager::defaults()).toUtf8(),
                 readFile(QFINDTESTDATA("../golden/sample.ass")));
    }

    void modelAndCommands()
    {
        SubtitleDocument document;
        document.setSubtitles({
            makeSubtitle(QStringLiteral("a"), QStringLiteral("A"), 100, 800),
            makeSubtitle(QStringLiteral("b"), QStringLiteral("B"), 900, 1'500),
        });
        SubtitleModel model(&document);
        SubtitleCommandManager commands(&document);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.get(0).value(QStringLiteral("durationMs")).toLongLong(), 700);
        QVERIFY(model.get(0).value(QStringLiteral("timed")).toBool());
        QVERIFY(model.roleNames().values().contains(QByteArray("sourceIndex")));

        QVERIFY(commands.editText(QStringLiteral("a"), QStringLiteral("A1")));
        QCOMPARE(document.subtitle(QStringLiteral("a"))->text, QStringLiteral("A1"));
        commands.undo();
        QCOMPARE(document.subtitle(QStringLiteral("a"))->text, QStringLiteral("A"));
        commands.redo();
        QCOMPARE(document.subtitle(QStringLiteral("a"))->text, QStringLiteral("A1"));

        QVERIFY(commands.move(QStringLiteral("a"), MediaTime::fromMilliseconds(200),
                              MediaTime::fromMilliseconds(900)));
        QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 200);
        commands.undo();
        QCOMPARE(document.subtitle(QStringLiteral("a"))->start.milliseconds(), 100);
        commands.redo();
        QVERIFY(commands.trim(QStringLiteral("a"), MediaTime::fromMilliseconds(250),
                              MediaTime::fromMilliseconds(850)));
        QCOMPARE(document.subtitle(QStringLiteral("a"))->end.milliseconds(), 850);
        commands.undo();
        QCOMPARE(document.subtitle(QStringLiteral("a"))->end.milliseconds(), 900);
        commands.redo();
        QVERIFY(commands.split(QStringLiteral("a"), MediaTime::fromMilliseconds(500),
                               QStringLiteral("A-left"), QStringLiteral("A-right")));
        QCOMPARE(document.count(), 3);
        const QString splitId = document.subtitles().at(1).id;
        commands.undo();
        QCOMPARE(document.count(), 2);
        commands.redo();
        QCOMPARE(document.count(), 3);
        QVERIFY(commands.merge(QStringLiteral("a"), splitId, QStringLiteral("A merged")));
        QCOMPARE(document.count(), 2);
        QCOMPARE(document.subtitle(QStringLiteral("a"))->text, QStringLiteral("A merged"));
        commands.undo();
        QCOMPARE(document.count(), 3);
        commands.redo();
        QCOMPARE(document.count(), 2);
        commands.undo();
        commands.undo();
        QCOMPARE(document.count(), 2);

        Subtitle created = makeSubtitle(QStringLiteral("c"), QStringLiteral("C"), 1'600, 2'000);
        QVERIFY(commands.create(document.count(), created));
        QCOMPARE(document.count(), 3);
        commands.undo();
        QCOMPARE(document.count(), 2);
        commands.redo();
        QCOMPARE(document.count(), 3);
        QVERIFY(commands.remove(QStringLiteral("c")));
        QCOMPARE(document.count(), 2);
        commands.undo();
        QCOMPARE(document.count(), 3);
        commands.redo();
        QCOMPARE(document.count(), 2);
    }

    void settingsVersionOneMigration()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":1,"fontSize1080p":64,"appearanceMode":"light","apiKey":"secret"})");
        file.close();

        const SettingsManager manager(path);
        const QJsonObject loaded = manager.load();
        QCOMPARE(loaded.value(QStringLiteral("version")).toInt(), 3);
        QCOMPARE(loaded.value(QStringLiteral("fontSize1080p")).toInt(), 64);
        QCOMPARE(loaded.value(QStringLiteral("appearanceMode")).toString(), QStringLiteral("light"));
        QVERIFY(!loaded.contains(QStringLiteral("apiKey")));
        QVERIFY(!loaded.contains(QStringLiteral("aiAssistEnabled")));
        QVERIFY(!loaded.contains(QStringLiteral("omniReviewEnabled")));
        QString error;
        QVERIFY2(manager.save(loaded, &error), qPrintable(error));
        QVERIFY(!readFile(path).contains("secret"));
    }

    void settingsVersionThreeDropsLegacyAiProviders()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("settings.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":2,"aiAssistEnabled":true,"aiProviderId":"custom",)"
                   R"("aiProviders":[{"id":"custom","name":"Custom"}],)"
                   R"("omniReviewEnabled":true,"omniReviewModel":"qwen-plus",)"
                   R"("omniReviewBaseUrl":"https://example.invalid/v1",)"
                   R"("omniReviewReasoningEffort":"high"})");
        file.close();
        const SettingsManager manager(path);
        const QJsonObject loaded = manager.load();
        QCOMPARE(loaded.value(QStringLiteral("version")).toInt(), 3);
        QVERIFY(!loaded.contains(QStringLiteral("aiAssistEnabled")));
        QVERIFY(!loaded.contains(QStringLiteral("aiProviders")));
        QVERIFY(!loaded.contains(QStringLiteral("omniReviewEnabled")));
        QVERIFY(!loaded.contains(QStringLiteral("omniReviewModel")));
        const QJsonArray leftover = loaded.value(QStringLiteral("legacyAiCredentialIds")).toArray();
        QCOMPARE(leftover.size(), 1);
        QCOMPARE(leftover.at(0).toString(), QStringLiteral("custom"));
    }

    void projectRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("media")));
        const QString projectPath = root.filePath(QStringLiteral("episode.subcue"));
        Project source;
        source.mediaPath = root.filePath(QStringLiteral("media/episode.mp4"));
        source.mediaFingerprint = {{QStringLiteral("size"), 123456},
                                   {QStringLiteral("mtimeMs"), 987654},
                                   {QStringLiteral("edgeSha256"), QStringLiteral("abc")}};
        Subtitle subtitle = makeSubtitle(QStringLiteral("stable-project-id"),
                                         QStringLiteral("工程字幕"), 2'080, 3'424);
        subtitle.startWordId = 7;
        subtitle.endWordId = 9;
        subtitle.metadata = {{QStringLiteral("candidateText"), QStringLiteral("候选")},
                             {QStringLiteral("ambiguity"), 0.25}};
        Subtitle skipped = makeSubtitle(QStringLiteral("stable-skipped-id"),
                                        QStringLiteral("未找到音频"), 0, 0);
        skipped.status = QStringLiteral("SKIPPED_NO_AUDIO");
        source.tracks.append({QStringLiteral("main"), QStringLiteral("主字幕"), {subtitle, skipped},
                              {{QStringLiteral("language"), QStringLiteral("zh-CN")}}});
        source.metadata = {{QStringLiteral("title"), QStringLiteral("测试工程")}};

        QString error;
        QVERIFY2(ProjectSerializer::save(projectPath, source, &error), qPrintable(error));
        const QJsonObject json = QJsonDocument::fromJson(readFile(projectPath)).object();
        QCOMPARE(json.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(json.value(QStringLiteral("media")).toObject()
                     .value(QStringLiteral("path")).toString(),
                 QStringLiteral("media/episode.mp4"));

        const auto loaded = ProjectSerializer::load(projectPath, &error);
        QVERIFY2(loaded.has_value(), qPrintable(error));
        QCOMPARE(QDir::cleanPath(loaded->mediaPath), QDir::cleanPath(source.mediaPath));
        QCOMPARE(loaded->tracks.size(), 1);
        QCOMPARE(loaded->tracks.at(0).subtitles.size(), 2);
        QCOMPARE(loaded->tracks.at(0).subtitles.at(0).id, QStringLiteral("stable-project-id"));
        QCOMPARE(loaded->tracks.at(0).subtitles.at(0).start.microseconds(), 2'080'000);
        QCOMPARE(loaded->tracks.at(0).subtitles.at(0).startWordId, 7);
        QCOMPARE(loaded->tracks.at(0).subtitles.at(0).metadata
                     .value(QStringLiteral("candidateText")).toString(), QStringLiteral("候选"));
        QVERIFY(!loaded->tracks.at(0).subtitles.at(1).isTimed());
    }

    void dpapiRoundTrip()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("credentials.dat"));
        const DpapiCredentialStore store(path);
        QString error;
        QVERIFY2(store.save(QStringLiteral(" test-secret-key "), &error), qPrintable(error));
        QCOMPARE(store.load(&error), QStringLiteral("test-secret-key"));
        QVERIFY(!readFile(path).contains("test-secret-key"));
        QVERIFY(store.hasSavedKey());
        QVERIFY(store.save(QStringLiteral("SubCue/Test/secondary"), QStringLiteral("second"), &error));
        QCOMPARE(store.load(QStringLiteral("SubCue/Test/secondary"), &error), QStringLiteral("second"));
        QVERIFY(store.save(QStringLiteral("SubCue/Test/secondary"), QStringLiteral("replaced"), &error));
        QCOMPARE(store.load(QStringLiteral("SubCue/Test/secondary"), &error), QStringLiteral("replaced"));
        QVERIFY(store.remove(QStringLiteral("SubCue/Test/secondary"), &error));
        QVERIFY(!store.exists(QStringLiteral("SubCue/Test/secondary")));
        QVERIFY(store.remove(QStringLiteral("SubCue/ASR/dashscope"), &error));
#else
        QSKIP("DPAPI 仅支持 Windows");
#endif
    }

    void legacyCredentialMigration()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("credentials.dat"));
        QByteArray plain = QByteArrayLiteral("legacy-secret");
        DATA_BLOB input{static_cast<DWORD>(plain.size()),
                        reinterpret_cast<BYTE *>(plain.data())};
        DATA_BLOB output{};
        QVERIFY(CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(reinterpret_cast<const char *>(output.pbData), output.cbData),
                 static_cast<qint64>(output.cbData));
        file.close();
        LocalFree(output.pbData);

        DpapiCredentialStore store(path);
        QString error;
        QVERIFY2(store.migrateLegacyDashScope(&error), qPrintable(error));
        QCOMPARE(store.load(QStringLiteral("SubCue/ASR/dashscope"), &error),
                 QStringLiteral("legacy-secret"));
        QVERIFY(!QFile::exists(path));
        QVERIFY(store.remove(QStringLiteral("SubCue/ASR/dashscope"), &error));

        QFile invalid(path);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("not-dpapi");
        invalid.close();
        QVERIFY(!store.migrateLegacyDashScope(&error));
        QVERIFY(QFile::exists(path));
#else
        QSKIP("DPAPI 仅支持 Windows");
#endif
    }
};

QTEST_GUILESS_MAIN(SubtitleProjectTests)
#include "test_subtitle_project.moc"
