#include "tts/audio_quality.h"
#include "tts/reference_samples.h"
#include "tts/voice_audio.h"
#include "tts/voice_document.h"
#include "project/project_store.h"
#include <QtCore/QUuid>
#include <QtCore/QFile>
#include <QtCore/QDataStream>
#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QScopeGuard>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif
#include <QtTest/QTest>
#include <cmath>
#include <limits>

using namespace subcue;
namespace {
bool writeWav(const QString &path, int count, bool floating, float level, bool invalid = false, bool truncated = false)
{
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian); stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const int bytes = floating ? 4 : 2;
    stream.writeRawData("RIFF", 4); stream << quint32(36 + count * bytes); stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(floating ? 3 : 1) << quint16(1) << quint32(24000) << quint32(24000 * bytes) << quint16(bytes) << quint16(bytes * 8);
    stream.writeRawData("data", 4); stream << quint32(count * bytes);
    for (int i = 0; i < (truncated ? count / 2 : count); ++i) {
        const float value = invalid && i == 100 ? std::numeric_limits<float>::quiet_NaN() : level * static_cast<float>(std::sin(i * 0.1));
        if (floating) stream << value; else stream << qint16(value * 32767);
    }
    return stream.status() == QDataStream::Ok;
}
}
class AudioQualityTests final : public QObject {
    Q_OBJECT
private slots:
    void exportFailurePreservesExistingOutput_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::newRow("cancel-after-first-clip") << QStringLiteral("cancel");
        QTest::newRow("later-take-changed") << QStringLiteral("changed");
#ifdef Q_OS_WIN
        QTest::newRow("output-locked-before-publication") << QStringLiteral("locked");
#endif
    }
    void exportFailurePreservesExistingOutput()
    {
        QFETCH(QString, failure);
        QTemporaryDir directory; QJsonArray clips;
        for (int i = 0; i < 2; ++i) {
            const auto source = directory.filePath(QStringLiteral("source-%1.wav").arg(i));
            QVERIFY(writeWav(source, 24000, true, 0.2f));
            const auto quality = AudioQuality::inspect(source); QVERIFY(quality);
            clips.append(QJsonObject{{QStringLiteral("id"), QString::number(i)}, {QStringLiteral("path"), source},
                {QStringLiteral("sha256"), QString::fromLatin1(quality->sha256.toHex())},
                {QStringLiteral("sourceRate"), 24000}, {QStringLiteral("sourceCount"), QStringLiteral("24000")},
                {QStringLiteral("start"), QStringLiteral("0")}, {QStringLiteral("end"), QStringLiteral("48000")},
                {QStringLiteral("position"), QString::number(i * 48000)}, {QStringLiteral("pause"), QStringLiteral("0")}, {QStringLiteral("gain"), 1.0}});
        }
        const auto output = directory.filePath(QStringLiteral("existing.wav"));
        QVERIFY(writeWav(output, 12000, true, 0.1f));
        const auto existing = AudioQuality::inspect(output); QVERIFY(existing);
        const auto files = QDir(directory.path()).entryList(QDir::Files | QDir::Hidden);
        std::atomic<bool> canceled{false}; int completed = 0; bool injected = false;
#ifdef Q_OS_WIN
        HANDLE locked = INVALID_HANDLE_VALUE;
        const auto unlock = qScopeGuard([&] { if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked); });
#endif
        QString error;
        const auto rows = VoiceAudio::render(clips, output, false, &error, &canceled, [&](int done, int) {
            completed = done;
            if (done != 1) return;
            // 通过真实文件状态和取消标志注入故障，不给产品代码增加测试开关。
            if (failure == QStringLiteral("cancel")) { canceled = true; injected = true; }
            else if (failure == QStringLiteral("changed")) {
                injected = writeWav(clips[1].toObject().value(QStringLiteral("path")).toString(), 24000, true, 0.3f);
            }
#ifdef Q_OS_WIN
            else {
                locked = CreateFileW(reinterpret_cast<LPCWSTR>(output.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                injected = locked != INVALID_HANDLE_VALUE;
            }
#endif
        });
        QVERIFY(injected); QVERIFY(rows.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(completed, failure == QStringLiteral("locked") ? 2 : 1);
        if (failure == QStringLiteral("cancel")) QVERIFY(error.contains(QStringLiteral("取消")));
        if (failure == QStringLiteral("changed")) QVERIFY(error.contains(QStringLiteral("变化")));
#ifdef Q_OS_WIN
        if (locked != INVALID_HANDLE_VALUE) { CloseHandle(locked); locked = INVALID_HANDLE_VALUE; }
#endif
        const auto preserved = AudioQuality::inspect(output); QVERIFY(preserved);
        QCOMPARE(preserved->sha256, existing->sha256);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), files);
        // 排除故障后复用同一编排重试，不能遗留占用或把失败当成完成。
        QVERIFY(writeWav(clips[1].toObject().value(QStringLiteral("path")).toString(), 24000, true, 0.2f));
        canceled = false; error.clear();
        QCOMPARE(VoiceAudio::render(clips, output, false, &error).size(), 2);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto result = AudioQuality::inspect(output); QVERIFY(result); QCOMPARE(result->sampleCount, qint64(96000));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), files);
    }
    void voiceArrangementStreamsCropPauseGainAndPcm24WithoutTouchingTakes()
    {
        QTemporaryDir directory; const auto source = directory.filePath(QStringLiteral("source.wav")); QVERIFY(writeWav(source, 24000, true, 0.2f));
        const auto original = AudioQuality::inspect(source); QVERIFY(original); ProjectStore store(directory.filePath(QStringLiteral("project.sqlite"))); QVERIFY(store.create());
        auto document = VoiceDocument::appendText(VoiceDocument::empty(), QStringLiteral("第一句。第二句。")); QString error;
        const auto initial = document.value(QStringLiteral("segments")).toArray();
        for (const auto &value : initial) {
            const auto item = value.toObject(); const auto job = QUuid::createUuid().toString(), take = QUuid::createUuid().toString();
            QVERIFY(store.enqueue(job, {{QStringLiteral("segmentId"), item.value(QStringLiteral("id"))}})); QVERIFY(store.finish(job, take, source));
            document = VoiceDocument::applyTake(document, item.value(QStringLiteral("id")).toString(), {{QStringLiteral("id"), take}, {QStringLiteral("sampleRate"), 24000}, {QStringLiteral("sampleCount"), QStringLiteral("24000")},
                {QStringLiteral("sha256"), QString::fromLatin1(original->sha256.toHex())}, {QStringLiteral("inputKey"), QString::fromLatin1(VoiceDocument::inputKey(document, item).toHex())}}, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
        }
        auto items = document.value(QStringLiteral("segments")).toArray(); auto first = items[0].toObject(); auto take = first.value(QStringLiteral("selectedTake")).toObject(); take.insert(QStringLiteral("trimEnd"), QStringLiteral("12000")); take.insert(QStringLiteral("manualCrop"), true); first.insert(QStringLiteral("selectedTake"), take); first.insert(QStringLiteral("gain"), 0.5); first.insert(QStringLiteral("pauseMs"), 300); items[0] = first;
        auto second = items[1].toObject(); second.insert(QStringLiteral("pauseMs"), 0); items[1] = second; document.insert(QStringLiteral("segments"), items);
        auto snapshot = *store.load(); snapshot.modules.insert(QStringLiteral("voiceStudio"), document); QVERIFY(store.save(snapshot));
        const auto clips = VoiceAudio::plan(directory.filePath(QStringLiteral("project.sqlite")), document, {}, false, &error); QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(clips.size(), 2);
        const auto output = directory.filePath(QStringLiteral("output.wav")); const auto rows = VoiceAudio::render(clips, output, false, &error); QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(rows.size(), 2); QCOMPARE(rows[0].toObject().value(QStringLiteral("waveform")).toArray().size(), 256);
        const auto result = AudioQuality::inspect(output); QVERIFY(result); QCOMPARE(result->sampleRate, 48000); QCOMPARE(result->channels, 1); QCOMPARE(result->sampleCount, 86400);
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); const auto header = file.read(44); QCOMPARE(quint8(header[34]), quint8(24)); QCOMPARE(quint8(header[20]), quint8(1)); file.close();
        std::atomic<bool> canceled{true}; error.clear(); QVERIFY(VoiceAudio::render(clips, output, false, &error, &canceled).isEmpty()); QCOMPARE(AudioQuality::inspect(output)->sha256, result->sha256);
        error.clear(); QVERIFY(!VoiceAudio::render(clips, directory.filePath(QStringLiteral("normalized.wav")), true, &error).isEmpty()); QVERIFY(AudioQuality::inspect(directory.filePath(QStringLiteral("normalized.wav")))->peak < original->peak);
        QCOMPARE(AudioQuality::inspect(source)->sha256, original->sha256);
        const auto id = first.value(QStringLiteral("id")).toString(); const auto edited = VoiceDocument::edit(document, id, QStringLiteral("改稿"), {}, false, {}); error.clear(); QString problem;
        QVERIFY(VoiceAudio::plan(directory.filePath(QStringLiteral("project.sqlite")), edited, {}, false, &error, &problem).isEmpty()); QCOMPARE(problem, id); QVERIFY(error.contains(QStringLiteral("过期")));
        error.clear(); QVERIFY(!VoiceAudio::plan(directory.filePath(QStringLiteral("project.sqlite")), edited, {}, true, &error).isEmpty());
    }
    void referenceSplittingDeduplicatesAndPreservesSources()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("original.wav"));
        QVERIFY(writeWav(path, 24000 * 25, false, 0.2f));
        const auto original = AudioQuality::inspect(path); QVERIFY(original);
        QString error;
        const auto samples = ReferenceSamples::prepare({path, path}, directory.filePath(QStringLiteral("samples")), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error)); QVERIFY(!samples.isEmpty()); QVERIFY(samples.size() <= 3);
        for (const auto &value : samples) {
            const auto sample = value.toObject();
            QVERIFY(sample.value(QStringLiteral("endSeconds")).toDouble() - sample.value(QStringLiteral("startSeconds")).toDouble() <= 12.001);
            const auto quality = AudioQuality::inspect(sample.value(QStringLiteral("referencePath")).toString()); QVERIFY(quality);
            QCOMPARE(quality->sampleRate, 24000); QCOMPARE(quality->channels, 1);
            QCOMPARE(sample.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(original->sha256.toHex()));
        }
        QCOMPARE(AudioQuality::inspect(path)->sha256, original->sha256);
        std::atomic<bool> cancel{true};
        QVERIFY(ReferenceSamples::prepare({path}, directory.path(), &error, &cancel).isEmpty()); QVERIFY(error.contains(QStringLiteral("取消")));
    }
    void referenceDerivationHonorsCropAndRejectsInvalidRanges()
    {
        QTemporaryDir directory; const QString source = directory.filePath(QStringLiteral("source.wav"));
        const QString target = directory.filePath(QStringLiteral("derived.wav"));
        QVERIFY(writeWav(source, 24000 * 3, true, 0.2f));
        const auto original = AudioQuality::inspect(source); QVERIFY(original);
        QString error;
        QVERIFY2(ReferenceSamples::derive(source, target, 0.5, 1.5, true, true, &error), qPrintable(error));
        const auto derived = AudioQuality::inspect(target); QVERIFY(derived); QCOMPARE(derived->sampleCount, 24000);
        QVERIFY(derived->peak > 0.1); QCOMPARE(AudioQuality::inspect(source)->sha256, original->sha256);
        QVERIFY(!ReferenceSamples::derive(source, source, 0, 1, false, false, &error));
        QVERIFY(!ReferenceSamples::derive(source, target, 2, 1, false, false, &error));
        QVERIFY(!ReferenceSamples::derive(source, target, 0, 31, false, false, &error));
        std::atomic<bool> cancel{true}; QVERIFY(!ReferenceSamples::derive(source, target, 0, 1, true, true, &error, &cancel));
    }
    void validNativeSamplesAndBoundedFrames()
    {
        QTemporaryDir directory;
        const QString shortPath = directory.filePath(QStringLiteral("short.wav"));
        const QString longPath = directory.filePath(QStringLiteral("long.wav"));
        QVERIFY(writeWav(shortPath, 24000, false, 0.2f));
        QVERIFY(writeWav(longPath, 2400000, false, 0.2f));
        QString error;
        const auto first = AudioQuality::inspect(shortPath, &error);
        QVERIFY2(first, qPrintable(error)); QCOMPARE(first->sampleCount, 24000); QCOMPARE(first->sampleRate, 24000); QCOMPARE(first->channels, 1);
        QVERIFY(first->peak > 0.19 && first->peak < 0.21); QVERIFY(first->rms > 0.13 && first->rms < 0.15);
        QVERIFY(first->warnings.isEmpty());
        QCOMPARE(first->sha256.size(), 32);
        QCOMPARE(first->toJson().value(QStringLiteral("sha256")).toString().size(), 64);
        const auto second = AudioQuality::inspect(longPath, &error);
        QVERIFY2(second, qPrintable(error)); QCOMPARE(second->sampleCount, 2400000);
        QCOMPARE(second->maxFrameBytes, first->maxFrameBytes);
        QVERIFY(second->maxFrameBytes <= 4 * 1024 * 1024);
        QCOMPARE(first->toJson().value(QStringLiteral("sampleCount")).toString(), QStringLiteral("24000"));
    }
    void floatingSamplesAndQualityWarnings()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("float.wav"));
        QVERIFY(writeWav(path, 24000, true, 1.2f));
        const auto clipped = AudioQuality::inspect(path); QVERIFY(clipped); QVERIFY(clipped->clippedFraction > 0); QVERIFY(!clipped->warnings.isEmpty());
        QVERIFY(writeWav(path, 24000, true, 0));
        const auto silent = AudioQuality::inspect(path); QVERIFY(silent); QCOMPARE(silent->silentFraction, 1.0); QVERIFY(!silent->warnings.isEmpty());
        QVERIFY(writeWav(path, 24000, true, 0.2f, true));
        QString error; QVERIFY(!AudioQuality::inspect(path, &error)); QVERIFY(error.contains(QStringLiteral("非有限")));
    }
    void rejectsDamageEmptyNetworkAndCancellation()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("bad.wav"));
        QVERIFY(writeWav(path, 24000, false, 0.2f, false, true));
        QString error; QVERIFY(!AudioQuality::inspect(path, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(writeWav(path, 0, false, 0)); QVERIFY(!AudioQuality::inspect(path));
        QVERIFY(!AudioQuality::inspect(QStringLiteral("https://invalid.example/audio.wav")));
        std::atomic<bool> cancel{true}; QVERIFY(!AudioQuality::inspect(path, &error, &cancel)); QVERIFY(error.contains(QStringLiteral("取消")));
    }
};
QTEST_GUILESS_MAIN(AudioQualityTests)
#include "test_audio_quality.moc"
