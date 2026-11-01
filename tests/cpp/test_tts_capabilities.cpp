#include "tts/local_tts_provider.h"
#include "tts/tts_types.h"
#include <QtTest/QTest>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtCore/QUuid>
using namespace subcue;

namespace {
TtsCapabilities localCapabilities(LocalTtsProvider::Kind kind)
{
    return LocalTtsProvider(kind, QStringLiteral("python"), QStringLiteral("worker")).capabilities();
}

TtsRequest japaneseReferenceChineseTarget()
{
    TtsRequest request;
    request.projectId = request.segmentId = request.voiceVersionId = request.referenceAssetId = QUuid::createUuid().toString();
    request.projectGeneration = 1; request.inputVersion = 1;
    request.provider = QStringLiteral("gpt-local"); request.model = QStringLiteral("GPT-SoVITS-v2ProPlus"); request.modelVersion = QStringLiteral("sha256:weights");
    request.spokenText = QStringLiteral("目标文本使用中文。"); request.language = QStringLiteral("Chinese");
    request.referenceLanguage = QStringLiteral("Japanese"); request.referenceText = QStringLiteral("これは日本語の参考音声です。");
    request.referenceSha256 = QByteArray(32, 'a'); request.effectiveParameters = QJsonObject{{QStringLiteral("speedFactor"), 1.0}};
    return request;
}
} // namespace

class TtsCapabilitiesTests final : public QObject {
    Q_OBJECT
private slots:
    void gptAdvertisesChineseEnglishJapaneseForTargetAndReference()
    {
        const auto caps = localCapabilities(LocalTtsProvider::Kind::GptProPlus);
        const QStringList chineseEnglishJapanese{QStringLiteral("Chinese"), QStringLiteral("English"), QStringLiteral("Japanese")};
        QCOMPARE(caps.provider, QStringLiteral("gpt-local"));
        QCOMPARE(caps.languages, chineseEnglishJapanese);
        QCOMPARE(caps.referenceLanguages, chineseEnglishJapanese);
        QVERIFY(caps.referenceRequired);
        QVERIFY(caps.transcriptRequired);
        QVERIFY(!caps.cloud);
        QVERIFY(!caps.voiceIdRequired);
    }
    void japaneseReferenceWithChineseTargetValidatesAndRestores()
    {
        const auto caps = localCapabilities(LocalTtsProvider::Kind::GptProPlus);
        auto request = japaneseReferenceChineseTarget();
        QString error;
        QVERIFY2(request.validate(caps, &error), qPrintable(error));
        auto chineseReference = request; chineseReference.referenceLanguage = QStringLiteral("Chinese");
        QVERIFY(chineseReference.validate(caps, &error));
        // 参考语言进入不可变输入指纹；换参考语言不能让旧候选看起来仍然有效。
        QVERIFY(request.inputHash() != chineseReference.inputHash());
        const auto restored = TtsRequest::fromSnapshot(request.snapshot(), caps, &error);
        QVERIFY2(restored, qPrintable(error));
        QCOMPARE(restored->language, QStringLiteral("Chinese"));
        QCOMPARE(restored->referenceLanguage, QStringLiteral("Japanese"));
        QCOMPARE(restored->referenceText, request.referenceText);
        QCOMPARE(restored->inputHash(), request.inputHash());
    }
    void unsupportedReferenceOrTargetLanguageIsRejected()
    {
        const auto caps = localCapabilities(LocalTtsProvider::Kind::GptProPlus);
        auto request = japaneseReferenceChineseTarget();
        QString error;
        request.referenceLanguage = QStringLiteral("Korean");
        QVERIFY(!request.validate(caps, &error));
        QVERIFY(error.contains(QStringLiteral("参考语言")));
        // 目标语言仍只用 languages 校验，不受参考语言列表影响。
        auto koreanTarget = request; koreanTarget.referenceLanguage = QStringLiteral("Japanese"); koreanTarget.language = QStringLiteral("Korean");
        QVERIFY(!koreanTarget.validate(caps, &error));
        QVERIFY(error.contains(QStringLiteral("目标语言")));
        // 手改能力表后，参考语言列表优先于目标语言列表。
        auto restricted = caps; restricted.referenceLanguages = {QStringLiteral("Chinese"), QStringLiteral("English")};
        QVERIFY(!request.validate(restricted, &error));
        QVERIFY(error.contains(QStringLiteral("参考语言")));
        restricted.referenceLanguages.clear();
        QVERIFY(!request.validate(restricted, &error));
    }
    void enginesWithoutReferenceLanguagesFallBackToTargetLanguages()
    {
        const auto caps = localCapabilities(LocalTtsProvider::Kind::QwenBase);
        QVERIFY(caps.referenceLanguages.isEmpty());
        QVERIFY(caps.referenceRequired);
        QVERIFY(caps.transcriptRequired);
        auto request = japaneseReferenceChineseTarget();
        request.provider = caps.provider; request.model = caps.model; request.effectiveParameters = QJsonObject{{QStringLiteral("seed"), 7}};
        QString error;
        QVERIFY2(request.validate(caps, &error), qPrintable(error));
        auto chineseOnly = caps; chineseOnly.languages = {QStringLiteral("Chinese")};
        QVERIFY(!request.validate(chineseOnly, &error));
        QVERIFY(error.contains(QStringLiteral("参考语言")));
    }
};

QTEST_GUILESS_MAIN(TtsCapabilitiesTests)
#include "test_tts_capabilities.moc"
