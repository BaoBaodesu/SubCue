#include "asr/dashscope_asr_service.h"

#include "alignment/normalizer.h"
#include "asr/audio_chunk_extractor.h"
#include "asr/audio_chunk_plan.h"
#include "common/logging.h"
#include "media/media_probe.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace subcue {
namespace {

QJsonObject sentenceFromEvent(const QJsonObject &event)
{
    const QJsonObject output = event.value(QStringLiteral("output")).toObject();
    const QJsonValue sentenceValue = output.value(QStringLiteral("sentence"));
    return sentenceValue.isObject() ? sentenceValue.toObject() : QJsonObject{};
}

QString responseErrorDetail(const QByteArray &body)
{
    const QJsonObject error = QJsonDocument::fromJson(body).object();
    const QString code = error.value(QStringLiteral("code")).toString();
    const QString message = error.value(QStringLiteral("message")).toString();
    if (!code.isEmpty() && !message.isEmpty()) return code + QStringLiteral(": ") + message;
    return message.isEmpty() ? code : message;
}

} // namespace

DashScopeAsrService::DashScopeAsrService(
    QString apiKey,
    QString model,
    QString region,
    IHttpClient *http)
    : DashScopeAsrService(std::move(apiKey), std::move(model), std::move(region), {}, http)
{
}

DashScopeAsrService::DashScopeAsrService(
    QString apiKey,
    QString model,
    QString region,
    QString apiHost,
    IHttpClient *http)
    : apiKey_(std::move(apiKey)),
      model_(std::move(model)),
      endpoint_(endpointForApiHost(std::move(apiHost), region))
{
    if (http) {
        http_ = http;
    } else {
        ownedHttp_ = std::make_unique<QtNetworkHttpClient>();
        http_ = ownedHttp_.get();
    }
}

QString DashScopeAsrService::providerId() const
{
    return QString::fromLatin1(kAsrProviderDashScope);
}

ProviderTestResult DashScopeAsrService::testConnection(const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    HttpRequest request;
    request.url = QUrl(endpoint_);
    request.method = QByteArrayLiteral("POST");
    request.headers = {
        {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + apiKey_.toUtf8()},
        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
    };
    request.body = buildPayload({});
    request.connectTimeoutMs = 15'000;
    request.transferTimeoutMs = 15'000;
    std::variant<HttpResponse, AppError> sent = http_->send(request, cancel);
    if (std::holds_alternative<AppError>(sent)) {
        return std::get<AppError>(std::move(sent));
    }
    const HttpResponse response = std::get<HttpResponse>(std::move(sent));
    // 空音频通常返回 400；它仍证明端点可达且凭据已通过认证。
    if (response.status != 200 && response.status != 400) {
        const QString detail = responseErrorDetail(response.body);
        return AppError(ErrorDomain::Asr, response.status,
            detail.isEmpty()
                ? QStringLiteral("云端 ASR 连接失败（HTTP %1）").arg(response.status)
                : QStringLiteral("云端 ASR 连接失败（HTTP %1）：%2").arg(response.status).arg(detail));
    }
    return ConnectionTestResult{{ModelDescriptor{model_, model_, 0, true}},
                                QDateTime::currentDateTimeUtc()};
}

QString DashScopeAsrService::endpointForRegion(QStringView region)
{
    if (region == QLatin1String("singapore")) {
        return QStringLiteral(
            "https://dashscope-intl.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation");
    }
    return QStringLiteral(
        "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation");
}

QString DashScopeAsrService::endpointForApiHost(QString apiHost, QStringView region)
{
    apiHost = apiHost.trimmed();
    if (apiHost.isEmpty()) return endpointForRegion(region);
    while (apiHost.endsWith(QLatin1Char('/'))) apiHost.chop(1);
    if (apiHost.endsWith(QLatin1String("/compatible-mode/v1"))) {
        apiHost.chop(QStringLiteral("/compatible-mode/v1").size());
    }
    if (apiHost.endsWith(QLatin1String("/api/v1"))) {
        apiHost += QStringLiteral("/services/aigc/multimodal-generation/generation");
    } else if (!apiHost.endsWith(QLatin1String("/api/v1/services/aigc/multimodal-generation/generation"))) {
        apiHost += QStringLiteral("/api/v1/services/aigc/multimodal-generation/generation");
    }
    const QUrl url(apiHost);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty()) return {};
    return url.toString(QUrl::FullyEncoded);
}

QVector<QJsonObject> DashScopeAsrService::parseEventPayloads(
    const QByteArray &body,
    QByteArrayView contentType)
{
    QVector<QJsonObject> events;
    if (contentType.contains("text/event-stream")) {
        const QList<QByteArray> lines = body.split('\n');
        for (QByteArray line : lines) {
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith("data:")) {
                const QJsonDocument document = QJsonDocument::fromJson(line.mid(5));
                if (document.isObject()) {
                    events.push_back(document.object());
                }
            }
        }
        return events;
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        events.push_back(document.object());
    }
    return events;
}

std::variant<QVector<TranscriptWord>, AppError> DashScopeAsrService::wordsFromEvents(
    const QVector<QJsonObject> &events)
{
    QMap<int, QJsonObject> sentences;
    for (const QJsonObject &event : events) {
        const QJsonObject sentence = sentenceFromEvent(event);
        const bool sentenceEnd = sentence.value(QStringLiteral("sentence_end")).toBool(true);
        const QJsonArray words = sentence.value(QStringLiteral("words")).toArray();
        if (sentenceEnd && !words.isEmpty()) {
            const int sentenceId = sentence.contains(QStringLiteral("sentence_id"))
                ? sentence.value(QStringLiteral("sentence_id")).toInt()
                : sentences.size() + 1;
            sentences.insert(sentenceId, sentence);
        }
    }

    QVector<TranscriptWord> words;
    const QList<int> ids = sentences.keys();
    for (int sentenceId : ids) {
        const QJsonArray rawWords = sentences.value(sentenceId).value(QStringLiteral("words")).toArray();
        for (const QJsonValue &rawValue : rawWords) {
            const QJsonObject raw = rawValue.toObject();
            const QString text = raw.value(QStringLiteral("text")).toString();
            if (text.trimmed().isEmpty()) {
                continue;
            }
            words.push_back(TranscriptWord{
                static_cast<qint64>(words.size() + 1),
                text,
                raw.value(QStringLiteral("begin_time")).toInteger(0),
                raw.value(QStringLiteral("end_time")).toInteger(0),
            });
        }
    }
    if (words.isEmpty()) {
        return AppError(ErrorDomain::Asr, static_cast<int>(AsrErrorCode::EmptyTranscript),
            QStringLiteral("语音识别未返回词级时间戳。"));
    }
    return words;
}

bool DashScopeAsrService::isOverlapDuplicate(
    const TranscriptWord &incoming,
    const QVector<TranscriptWord> &existing)
{
    const QString incomingText = Normalizer::normalizeText(incoming.text);
    if (incomingText.isEmpty()) {
        return true;
    }
    const qsizetype begin = std::max<qsizetype>(0, existing.size() - 40);
    for (qsizetype index = existing.size() - 1; index >= begin; --index) {
        const TranscriptWord &prior = existing.at(index);
        if (incoming.startMs - prior.endMs > 2'000) {
            break;
        }
        if (incomingText == Normalizer::normalizeText(prior.text)
            && std::llabs(incoming.startMs - prior.startMs) <= 600
            && std::llabs(incoming.endMs - prior.endMs) <= 900) {
            return true;
        }
    }
    return false;
}

QByteArray DashScopeAsrService::buildPayload(const QByteArray &flac) const
{
    QJsonObject inputAudio;
    inputAudio.insert(QStringLiteral("data"),
        QStringLiteral("data:audio/flac;base64,") + QString::fromLatin1(flac.toBase64()));
    QJsonObject content;
    content.insert(QStringLiteral("type"), QStringLiteral("input_audio"));
    content.insert(QStringLiteral("input_audio"), inputAudio);
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), QJsonArray{content});
    QJsonObject input;
    input.insert(QStringLiteral("messages"), QJsonArray{message});
    QJsonObject parameters;
    parameters.insert(QStringLiteral("format"), QStringLiteral("flac"));
    parameters.insert(QStringLiteral("sample_rate"), QStringLiteral("16000"));
    QJsonObject payload;
    payload.insert(QStringLiteral("model"), model_);
    payload.insert(QStringLiteral("input"), input);
    payload.insert(QStringLiteral("parameters"), parameters);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

AsrResult DashScopeAsrService::transcribeFlac(
    const QByteArray &flac,
    const std::atomic<bool> *cancel)
{
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    HttpRequest request;
    request.url = QUrl(endpoint_);
    request.method = QByteArrayLiteral("POST");
    request.headers = {
        {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + apiKey_.toUtf8()},
        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
        {QByteArrayLiteral("X-DashScope-SSE"), QByteArrayLiteral("enable")},
    };
    request.body = buildPayload(flac);
    request.connectTimeoutMs = 15'000;
    request.transferTimeoutMs = 360'000;

    std::variant<HttpResponse, AppError> sent = http_->send(request, cancel);
    if (std::holds_alternative<AppError>(sent)) {
        return std::get<AppError>(sent);
    }
    if (asrCancelled(cancel)) {
        return asrCancelledError();
    }
    const HttpResponse response = std::get<HttpResponse>(std::move(sent));
    if (response.status >= 400) {
        const QString detail = responseErrorDetail(response.body);
        return AppError(ErrorDomain::Asr, response.status,
            detail.isEmpty()
                ? QStringLiteral("语音识别请求失败（HTTP %1）").arg(response.status)
                : QStringLiteral("语音识别请求失败（HTTP %1）：%2").arg(response.status).arg(detail));
    }
    const QVector<QJsonObject> events = parseEventPayloads(response.body, response.contentType);
    std::variant<QVector<TranscriptWord>, AppError> parsed = wordsFromEvents(events);
    if (std::holds_alternative<AppError>(parsed)) {
        return std::get<AppError>(parsed);
    }
    Transcript transcript;
    transcript.words = std::get<QVector<TranscriptWord>>(std::move(parsed));
    return transcript;
}

AsrResult DashScopeAsrService::transcribePreparedChunks(
    const QVector<PreparedAudioChunk> &chunks,
    const std::atomic<bool> *cancel,
    const std::function<void(int, int)> &progress)
{
    QVector<TranscriptWord> merged;
    for (int index = 0; index < chunks.size(); ++index) {
        if (asrCancelled(cancel)) {
            return asrCancelledError();
        }
        if (progress) {
            progress(index, chunks.size());
        }
        AsrResult local = transcribeFlac(chunks.at(index).flac, cancel);
        if (std::holds_alternative<AppError>(local)) {
            return local;
        }
        const QVector<TranscriptWord> words = std::get<Transcript>(std::move(local)).words;
        if (progress) progress(index + 1, chunks.size());
        for (TranscriptWord word : words) {
            word.startMs += chunks.at(index).window.startMs;
            word.endMs += chunks.at(index).window.startMs;
            if (!isOverlapDuplicate(word, merged)) {
                merged.push_back(std::move(word));
            }
        }
    }
    Transcript transcript;
    transcript.words = std::move(merged);
    transcript.sortAndReindex();
    return transcript;
}

AsrResult DashScopeAsrService::transcribe(const AsrRequest &request)
{
    if (asrCancelled(request.cancel)) {
        return asrCancelledError();
    }
    const ProbeResult probed = MediaProbe::probe(request.mediaPath);
    if (std::holds_alternative<AppError>(probed)) {
        return std::get<AppError>(probed);
    }
    const MediaInfo &info = std::get<MediaInfo>(probed);
    const QVector<AudioChunkWindow> windows = AudioChunkPlanner::plan(info.duration.seconds());
    QVector<PreparedAudioChunk> chunks;
    chunks.reserve(windows.size());
    for (const AudioChunkWindow &window : windows) {
        if (asrCancelled(request.cancel)) {
            return asrCancelledError();
        }
        MediaResult<PreparedAudioChunk> extracted =
            AudioChunkExtractor::extract(request.mediaPath, window, request.cancel, true);
        if (std::holds_alternative<AppError>(extracted)) {
            return std::get<AppError>(extracted);
        }
        chunks.push_back(std::get<PreparedAudioChunk>(std::move(extracted)));
    }
    qCInfo(subcueAsrLog) << "dashscope chunks" << chunks.size();
    return transcribePreparedChunks(chunks, request.cancel, request.progress);
}

} // namespace subcue
