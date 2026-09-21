#include "ai/ai_review_service.h"
#include "ai/ai_review_settings.h"
#include "ai/ai_types.h"
#include "ai/review_audio_encoder.h"
#include "alignment/transcript.h"
#include "asr/http_client.h"
#include "common/app_error.h"
#include "settings/settings_manager.h"
#include "subtitle/subtitle.h"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>

#include <atomic>
#include <variant>

using namespace subcue;

namespace {

class FakeHttpClient final : public IHttpClient {
public:
    QVector<HttpResponse> responses;
    AppError error{ErrorDomain::Network, 0, QString()};
    bool fail = false;
    int sendCount = 0;
    HttpRequest lastRequest;

    std::variant<HttpResponse, AppError> send(
        const HttpRequest &request,
        const std::atomic<bool> *cancel) override
    {
        lastRequest = request;
        ++sendCount;
        if (aiCancelled(cancel)) return aiCancelledError();
        if (fail) return error;
        if (responses.isEmpty()) return HttpResponse{200, {}, QByteArrayLiteral("{}")};
        const int index = qMin(sendCount - 1, static_cast<int>(responses.size() - 1));
        return responses.at(index);
    }

    std::variant<qint64, AppError> download(
        const HttpRequest &, QFile *, qint64, const std::atomic<bool> *,
        const std::function<void(qint64, qint64)> &) override
    {
        return AppError(ErrorDomain::Network, 1, QStringLiteral("unused"));
    }
};

QByteArray jsonBody(const QJsonObject &arguments)
{
    const QJsonObject function{
        {QStringLiteral("name"), QStringLiteral("submit_word_mapping")},
        {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))},
    };
    const QJsonObject message{
        {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
            {QStringLiteral("function"), function}}}},
    };
    return QJsonDocument(QJsonObject{
        {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message}}}},
    }).toJson(QJsonDocument::Compact);
}

SubtitleOmniSegment segment(const QString &id, const QString &text, qint64 start, qint64 end)
{
    return {id, text, start, end, {}, {}};
}

} // namespace

class AiReviewServiceTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsKeepGlobalOffsetAndOverlap();
    void correctSubtitleStaysKeep();
    void missingTextSuggestsReplace();
    void invalidJsonBecomesReviewAfterRetry();
    void authFailureAbortsReview();
    void cancelStopsReview();
    void gatedCutRequiresReplacementExceptNoise();
    void similarButDifferentMeaningDoesNotAutoCut();
    void asrErrorWithoutAudioEvidenceDoesNotCut();
    void subtitleWindowIsHardCapped();
    void encodeWindowClampsRequestedSpan();
    void wordMappingAppliesValidRange();
    void wordMappingRejectsOutOfBounds();
};

void AiReviewServiceTests::windowsKeepGlobalOffsetAndOverlap()
{
    OmniReviewSettings settings;
    settings.subtitleWindowMs = 1000;
    settings.subtitleOverlapMs = 200;
    const auto windows = AiReviewService::subtitleWindows(2500, settings);
    QCOMPARE(windows.first().first, 0);
    QCOMPARE(windows.first().second, 1000);
    QVERIFY(windows.size() >= 2);
    QCOMPARE(windows.at(1).first, 800);
}

void AiReviewServiceTests::correctSubtitleStaysKeep()
{
    FakeHttpClient http;
    http.responses.append({200, {}, jsonBody(QJsonObject{{QStringLiteral("results"), QJsonArray{QJsonObject{
        {QStringLiteral("segment_id"), QStringLiteral("a")},
        {QStringLiteral("issue_type"), QStringLiteral("PUNCTUATION")},
        {QStringLiteral("confidence"), 0.99},
        {QStringLiteral("original_text"), QStringLiteral("你好")},
        {QStringLiteral("suggested_text"), QStringLiteral("你好")},
        {QStringLiteral("reason"), QStringLiteral("录音与字幕一致")},
        {QStringLiteral("decision"), QStringLiteral("KEEP")},
    }}}})});
    OmniReviewSettings settings;
    AiReviewService service(settings, QStringLiteral("key"), &http);
    SubtitleOmniRequest request;
    request.segments.append(segment(QStringLiteral("a"), QStringLiteral("你好"), 0, 1000));
    SubtitleOmniResult result = service.reviewSubtitles(request);
    QCOMPARE(std::get<QVector<SubtitleOmniSuggestion>>(result).at(0).decision, SubtitleOmniDecision::Keep);
}

void AiReviewServiceTests::missingTextSuggestsReplace()
{
    FakeHttpClient http;
    http.responses.append({200, {}, jsonBody(QJsonObject{{QStringLiteral("results"), QJsonArray{QJsonObject{
        {QStringLiteral("segment_id"), QStringLiteral("a")},
        {QStringLiteral("issue_type"), QStringLiteral("MISSING_TEXT")},
        {QStringLiteral("confidence"), 0.94},
        {QStringLiteral("original_text"), QStringLiteral("你好")},
        {QStringLiteral("suggested_text"), QStringLiteral("你好世界")},
        {QStringLiteral("reason"), QStringLiteral("录音多了世界")},
        {QStringLiteral("decision"), QStringLiteral("REPLACE_TEXT")},
    }}}})});
    OmniReviewSettings settings;
    AiReviewService service(settings, QStringLiteral("key"), &http);
    SubtitleOmniRequest request;
    request.segments.append(segment(QStringLiteral("a"), QStringLiteral("你好"), 0, 1000));
    const auto suggestions = std::get<QVector<SubtitleOmniSuggestion>>(service.reviewSubtitles(request));
    QCOMPARE(suggestions.at(0).decision, SubtitleOmniDecision::ReplaceText);
    Subtitle cue;
    cue.id = QStringLiteral("a");
    cue.text = QStringLiteral("你好");
    AiReviewService::applySubtitleSuggestion(&cue, suggestions.at(0));
    QCOMPARE(cue.metadata.value(QStringLiteral("omniReviewStatus")).toString(), QStringLiteral("suggested"));
    QCOMPARE(cue.text, QStringLiteral("你好"));
}

void AiReviewServiceTests::invalidJsonBecomesReviewAfterRetry()
{
    FakeHttpClient http;
    http.responses.append({200, {}, QByteArrayLiteral("not-json")});
    http.responses.append({200, {}, QByteArrayLiteral("still-bad")});
    OmniReviewSettings settings;
    AiReviewService service(settings, QStringLiteral("key"), &http);
    SubtitleOmniRequest request;
    request.segments.append(segment(QStringLiteral("a"), QStringLiteral("你好"), 0, 1000));
    const auto suggestions = std::get<QVector<SubtitleOmniSuggestion>>(service.reviewSubtitles(request));
    QCOMPARE(http.sendCount, 2);
    QCOMPARE(suggestions.at(0).decision, SubtitleOmniDecision::Review);
}

void AiReviewServiceTests::cancelStopsReview()
{
    FakeHttpClient http;
    OmniReviewSettings settings;
    AiReviewService service(settings, QStringLiteral("key"), &http);
    std::atomic<bool> cancel{true};
    SubtitleOmniRequest request;
    request.segments.append(segment(QStringLiteral("a"), QStringLiteral("你好"), 0, 1000));
    SubtitleOmniResult result = service.reviewSubtitles(request, &cancel);
    QVERIFY(std::holds_alternative<AppError>(result));
    QVERIFY(isAiCancelError(std::get<AppError>(result)));
}

void AiReviewServiceTests::gatedCutRequiresReplacementExceptNoise()
{
    OmniReviewSettings settings;
    RoughCutOmniSuggestion cut;
    cut.decision = RoughCutDecision::Cut;
    cut.confidence = 0.95;
    cut.reasonType = RoughCutOmniReasonType::FalseStart;
    QCOMPARE(AiReviewService::gatedCutDecision(cut, settings), RoughCutDecision::Review);
    cut.replacementCandidateId = QStringLiteral("2");
    QCOMPARE(AiReviewService::gatedCutDecision(cut, settings), RoughCutDecision::Cut);
    RoughCutOmniSuggestion noise = cut;
    noise.replacementCandidateId.clear();
    noise.reasonType = RoughCutOmniReasonType::NoiseTake;
    QCOMPARE(AiReviewService::gatedCutDecision(noise, settings), RoughCutDecision::Cut);
}

void AiReviewServiceTests::similarButDifferentMeaningDoesNotAutoCut()
{
    OmniReviewSettings settings;
    RoughCutOmniSuggestion suggestion;
    suggestion.decision = RoughCutDecision::Cut;
    suggestion.confidence = 0.8;
    suggestion.reasonType = RoughCutOmniReasonType::Duplicate;
    suggestion.replacementCandidateId = QStringLiteral("2");
    QCOMPARE(AiReviewService::gatedCutDecision(suggestion, settings), RoughCutDecision::Review);
}

void AiReviewServiceTests::asrErrorWithoutAudioEvidenceDoesNotCut()
{
    OmniReviewSettings settings;
    RoughCutOmniSuggestion suggestion;
    suggestion.decision = RoughCutDecision::Cut;
    suggestion.confidence = 0.99;
    suggestion.reasonType = RoughCutOmniReasonType::Uncertain;
    suggestion.replacementCandidateId = QStringLiteral("2");
    QCOMPARE(AiReviewService::gatedCutDecision(suggestion, settings), RoughCutDecision::Review);
}

void AiReviewServiceTests::subtitleWindowIsHardCapped()
{
    QCOMPARE(SettingsManager::defaults().value(QStringLiteral("omniReviewWindowMs")).toInt(),
             kOmniSubtitleWindowDefaultMs);
    OmniReviewSettings settings = OmniReviewSettingsStore::fromJson({});
    QCOMPARE(settings.subtitleWindowMs, kOmniSubtitleWindowDefaultMs);
    settings = OmniReviewSettingsStore::fromJson({
        {QStringLiteral("omniReviewWindowMs"), 480000},
    });
    QCOMPARE(settings.subtitleWindowMs, kOmniSubtitleWindowMaxMs);
    settings = OmniReviewSettingsStore::fromJson({
        {QStringLiteral("omniReviewWindowMs"), 1'200'000},
    });
    QCOMPARE(settings.subtitleWindowMs, kOmniSubtitleWindowMaxMs);
    settings.subtitleWindowMs = 480'000;
    const auto windows = AiReviewService::subtitleWindows(600'000, settings);
    QVERIFY(!windows.isEmpty());
    QCOMPARE(windows.first().second - windows.first().first, kOmniSubtitleWindowMaxMs);
}

void AiReviewServiceTests::encodeWindowClampsRequestedSpan()
{
#ifndef SUBCUE_TEST_MEDIA_DIR
    QSKIP("Test media directory is not configured");
#else
    const QString path = QDir(QString::fromUtf8(SUBCUE_TEST_MEDIA_DIR)).filePath(
        QStringLiteral("audio.wav"));
    MediaResult<OmniAudioClip> encoded = ReviewAudioEncoder::encodeWindow(path, 0, 480'000);
    QVERIFY2(std::holds_alternative<OmniAudioClip>(encoded),
        qPrintable(std::holds_alternative<AppError>(encoded)
            ? std::get<AppError>(encoded).userMessage() : QString()));
    const OmniAudioClip clip = std::get<OmniAudioClip>(encoded);
    QVERIFY(clip.windowEndMs - clip.windowStartMs <= kOmniSubtitleWindowMaxMs);
    QVERIFY(!clip.data.isEmpty());
#endif
}

void AiReviewServiceTests::authFailureAbortsReview()
{
    FakeHttpClient http;
    http.responses.append({401, {}, QByteArrayLiteral("{\"error\":\"unauthorized\"}")});
    AiReviewService service({}, QStringLiteral("key"), &http);
    SubtitleOmniRequest request;
    request.segments.append(segment(QStringLiteral("a"), QStringLiteral("你好"), 0, 1000));
    SubtitleOmniResult result = service.reviewSubtitles(request);
    QVERIFY(std::holds_alternative<AppError>(result));
    QCOMPARE(std::get<AppError>(result).userMessage(), QStringLiteral("认证失败：API Key 无效或已过期"));
}

void AiReviewServiceTests::wordMappingAppliesValidRange()
{
    FakeHttpClient http;
    http.responses.append({200, {}, jsonBody(QJsonObject{{QStringLiteral("mappings"), QJsonArray{QJsonObject{
        {QStringLiteral("line_id"), QStringLiteral("L1")},
        {QStringLiteral("status"), QStringLiteral("matched")},
        {QStringLiteral("start_word_id"), QStringLiteral("W1")},
        {QStringLiteral("end_word_id"), QStringLiteral("W1")},
        {QStringLiteral("confidence"), 0.95},
    }}}})});
    WordMappingOmniRequest request;
    request.words = {{1, QStringLiteral("Hello"), 0, 400}, {2, QStringLiteral("world"), 400, 800}};
    Subtitle cue;
    cue.id = QStringLiteral("s1");
    cue.text = QStringLiteral("Hello");
    cue.confidence = 0.2;
    cue.ambiguity = 1.0;
    request.subtitles.append(cue);
    AiReviewService service({}, QStringLiteral("key"), &http);
    WordMappingOmniResult result = service.reviewWordMapping(request);
    QVERIFY(std::holds_alternative<QVector<Subtitle>>(result));
    const Subtitle mapped = std::get<QVector<Subtitle>>(result).at(0);
    QCOMPARE(mapped.startWordId, 1);
    QCOMPARE(mapped.endWordId, 1);
    QCOMPARE(mapped.start.milliseconds(), 0);
    QCOMPARE(mapped.end.milliseconds(), 400);
}

void AiReviewServiceTests::wordMappingRejectsOutOfBounds()
{
    FakeHttpClient http;
    http.responses.append({200, {}, jsonBody(QJsonObject{{QStringLiteral("mappings"), QJsonArray{QJsonObject{
        {QStringLiteral("line_id"), QStringLiteral("L1")},
        {QStringLiteral("status"), QStringLiteral("matched")},
        {QStringLiteral("start_word_id"), QStringLiteral("W1")},
        {QStringLiteral("end_word_id"), QStringLiteral("W99")},
        {QStringLiteral("confidence"), 0.99},
    }}}})});
    WordMappingOmniRequest request;
    request.words = {{1, QStringLiteral("Hello"), 0, 400}};
    Subtitle cue;
    cue.id = QStringLiteral("s1");
    cue.text = QStringLiteral("你好");
    cue.confidence = 0.2;
    cue.start = MediaTime::fromMilliseconds(10);
    cue.end = MediaTime::fromMilliseconds(20);
    request.subtitles.append(cue);
    AiReviewService service({}, QStringLiteral("key"), &http);
    WordMappingOmniResult result = service.reviewWordMapping(request);
    const Subtitle mapped = std::get<QVector<Subtitle>>(result).at(0);
    QCOMPARE(mapped.start.milliseconds(), 10);
    QCOMPARE(mapped.end.milliseconds(), 20);
    QCOMPARE(mapped.metadata.value(QStringLiteral("aiReviewStatus")).toString(),
             QStringLiteral("rejectedLocally"));
}

QTEST_MAIN(AiReviewServiceTests)
#include "test_ai_review_service.moc"
