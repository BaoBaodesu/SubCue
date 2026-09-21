#include "ai/ai_review_service.h"

#include "ai/ai_review_json.h"
#include "ai/ai_types.h"
#include "ai/openai_compatible_provider.h"
#include "ai/review_audio_encoder.h"
#include "alignment/alignment_engine.h"
#include "common/logging.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPair>

#include <algorithm>
#include <utility>

namespace subcue {
namespace {

QString nearbyScript(const QString &script, const QString &transcript)
{
    if (script.isEmpty() || transcript.isEmpty()) return {};
    const int index = script.indexOf(transcript.left(12));
    if (index < 0) return script.left(240);
    const int start = std::max(0, index - 80);
    return script.mid(start, 240);
}

QJsonObject subtitleUserPayload(
    const QVector<SubtitleOmniSegment> &segments,
    qint64 windowStartMs,
    const QString &scriptText)
{
    QJsonArray items;
    for (const SubtitleOmniSegment &segment : segments) {
        items.append(QJsonObject{
            {QStringLiteral("segment_id"), segment.segmentId},
            {QStringLiteral("text"), segment.text},
            {QStringLiteral("start_ms"), segment.startMs},
            {QStringLiteral("end_ms"), segment.endMs},
            {QStringLiteral("previous_text"), segment.previousText},
            {QStringLiteral("next_text"), segment.nextText},
        });
    }
    return {
        {QStringLiteral("task"), QStringLiteral("subtitle_review")},
        {QStringLiteral("window_start_ms"), windowStartMs},
        {QStringLiteral("script_reference"), scriptText},
        {QStringLiteral("segments"), items},
        {QStringLiteral("output"), QStringLiteral(
            "Call submit_subtitle_review. Return segment_id from the input. "
            "Do not invent timestamps. Recording audio is the highest-priority evidence.")},
    };
}

QJsonObject roughCutUserPayload(const RoughCutOmniCandidate &candidate, const QString &scriptText)
{
    return {
        {QStringLiteral("task"), QStringLiteral("roughcut_review")},
        {QStringLiteral("candidate"), QJsonObject{
            {QStringLiteral("candidate_id"), candidate.candidateId},
            {QStringLiteral("start_ms"), candidate.startMs},
            {QStringLiteral("end_ms"), candidate.endMs},
            {QStringLiteral("transcript"), candidate.transcript},
            {QStringLiteral("previous_context"), candidate.previousContext},
            {QStringLiteral("next_context"), candidate.nextContext},
            {QStringLiteral("reason_hint"), candidate.reasonHint},
            {QStringLiteral("possible_replacement_id"), candidate.replacementCandidateId},
        }},
        {QStringLiteral("script_reference"),
         candidate.scriptNearby.isEmpty() ? nearbyScript(scriptText, candidate.transcript)
                                          : candidate.scriptNearby},
        {QStringLiteral("output"), QStringLiteral(
            "Call submit_roughcut_review. Identify replacement takes first. "
            "Only CUT when a failed take or replacement take is clear. "
            "Do not emit free-form cut timestamps.")},
    };
}

[[nodiscard]] bool isFatalOmniError(const AppError &error) noexcept
{
    if (isAiCancelError(error)) return true;
    switch (error.code()) {
    case 401:
    case 403:
    case 408:
    case 429:
    case 504:
        return true;
    default:
        break;
    }
    const QString message = error.userMessage();
    return message.contains(QStringLiteral("认证失败"))
        || message.contains(QStringLiteral("模型权限"))
        || message.contains(QStringLiteral("过于频繁"))
        || message.contains(QStringLiteral("超时"));
}

} // namespace

AiReviewService::AiReviewService(
    OmniReviewSettings settings,
    QString apiKey,
    IHttpClient *http)
    : settings_(std::move(settings)),
      provider_(std::move(apiKey), settings_, http),
      lastModel_(settings_.model)
{
}

QString AiReviewService::lastModel() const
{
    return lastModel_.isEmpty() ? settings_.model : lastModel_;
}

void AiReviewService::recordChatResponse(const OmniChatResponse &response)
{
    addOmniUsage(&usage_, response.usage);
    if (!response.model.trimmed().isEmpty()) lastModel_ = response.model;
}

QString AiReviewService::subtitleSystemPrompt()
{
    return QStringLiteral(
        "你是字幕语义复核器，不是 ASR。录音是最高优先级证据，Word/文稿只是参考。"
        "不得因为录音与文稿不一致就认定录音错误。"
        "只允许 KEEP、REVIEW、REPLACE_TEXT。"
        "低置信度不得自动改字。"
        "必须返回输入中的 segment_id，禁止编造绝对时间。");
}

QString AiReviewService::roughCutSystemPrompt()
{
    return QStringLiteral(
        "你是自动粗剪语义复核器。本地算法只提供 Candidate，最终剪切点由本地 Safe Cut Boundary 决定。"
        "必须优先判断是否存在 replacement take。"
        "半句后重录时，第一遍是 FALSE_START/RETAKE，应 CUT；完整第二遍 KEEP。"
        "两句相似但语义不同不得误删。"
        "ASR 错但录音正确不得 CUT。"
        "无 replacement take 时错误判断应倾向 REVIEW。"
        "只允许 KEEP、REVIEW、CUT。"
        "禁止输出自由生成的毫秒时间戳。");
}

QString AiReviewService::wordMappingSystemPrompt()
{
    return OpenAICompatibleProvider::systemPrompt();
}

QVector<QPair<qint64, qint64>> AiReviewService::subtitleWindows(
    qint64 durationMs,
    const OmniReviewSettings &settings)
{
    QVector<QPair<qint64, qint64>> windows;
    if (durationMs <= 0) return windows;
    const qint64 span = std::max<qint64>(1,
        std::min<qint64>(settings.subtitleWindowMs, kOmniSubtitleWindowMaxMs));
    const qint64 overlap = std::clamp<qint64>(settings.subtitleOverlapMs, 0, span / 2);
    qint64 start = 0;
    while (start < durationMs) {
        const qint64 end = std::min(durationMs, start + span);
        windows.append({start, end});
        if (end >= durationMs) break;
        start = std::max<qint64>(end - overlap, start + 1);
    }
    return windows;
}

void AiReviewService::applySubtitleSuggestion(Subtitle *subtitle, const SubtitleOmniSuggestion &suggestion)
{
    if (!subtitle) return;
    const bool replace = suggestion.decision == SubtitleOmniDecision::ReplaceText
        && !suggestion.suggestedText.trimmed().isEmpty();
    subtitle->metadata.insert(QStringLiteral("omniIssueType"), subtitleIssueName(suggestion.issueType));
    subtitle->metadata.insert(QStringLiteral("omniSuggestedText"), suggestion.suggestedText);
    subtitle->metadata.insert(QStringLiteral("omniReason"), suggestion.reason);
    subtitle->metadata.insert(QStringLiteral("omniConfidence"), suggestion.confidence);
    subtitle->metadata.insert(QStringLiteral("omniDecision"), subtitleOmniDecisionName(suggestion.decision));
    subtitle->metadata.insert(QStringLiteral("omniReviewStatus"),
        suggestion.decision == SubtitleOmniDecision::Keep
            ? omniReviewStatusName(OmniReviewStatus::Normal)
            : (replace || suggestion.decision == SubtitleOmniDecision::ReplaceText
                ? omniReviewStatusName(OmniReviewStatus::Suggested)
                : omniReviewStatusName(OmniReviewStatus::Review)));
}

RoughCutDecision AiReviewService::gatedCutDecision(
    const RoughCutOmniSuggestion &suggestion,
    const OmniReviewSettings &settings)
{
    if (suggestion.decision == RoughCutDecision::Keep) {
        return suggestion.confidence < settings.reviewConfidence
            ? RoughCutDecision::Keep
            : suggestion.decision;
    }
    if (suggestion.confidence < settings.reviewConfidence) return RoughCutDecision::Keep;
    if (suggestion.decision == RoughCutDecision::Review
        || suggestion.reasonType == RoughCutOmniReasonType::Uncertain) {
        return RoughCutDecision::Review;
    }
    if (suggestion.decision != RoughCutDecision::Cut) return RoughCutDecision::Review;
    if (suggestion.confidence < settings.cutConfidence) return RoughCutDecision::Review;
    const bool hasReplacement = !suggestion.replacementCandidateId.trimmed().isEmpty();
    if (!hasReplacement && suggestion.reasonType != RoughCutOmniReasonType::NoiseTake) {
        return RoughCutDecision::Review;
    }
    return RoughCutDecision::Cut;
}

OmniCompleteResult AiReviewService::completeWithRetry(
    OmniChatRequest request,
    const std::atomic<bool> *cancel)
{
    OmniCompleteResult first = provider_.complete(request, cancel);
    if (std::holds_alternative<OmniChatResponse>(first)) {
        recordChatResponse(std::get<OmniChatResponse>(first));
        const OmniChatResponse &response = std::get<OmniChatResponse>(first);
        if (!response.arguments.isEmpty()) return first;
    }
    if (std::holds_alternative<AppError>(first)
        && isFatalOmniError(std::get<AppError>(first))) {
        return first;
    }
    qCWarning(subcueAiLog) << "omni_review retrying structured parse"
        << "provider=" << provider_.providerId()
        << "model=" << provider_.model();
    OmniCompleteResult retry = provider_.complete(request, cancel);
    if (std::holds_alternative<OmniChatResponse>(retry)) {
        recordChatResponse(std::get<OmniChatResponse>(retry));
    }
    return retry;
}

SubtitleOmniResult AiReviewService::reviewSubtitles(
    const SubtitleOmniRequest &request,
    const std::atomic<bool> *cancel,
    OmniReviewProgress progress)
{
    QHash<QString, SubtitleOmniSuggestion> byId;
    qint64 durationMs = 0;
    for (const SubtitleOmniSegment &segment : request.segments) {
        durationMs = std::max(durationMs, segment.endMs);
    }
    const auto windows = subtitleWindows(durationMs, settings_);
    int total = 0;
    for (const auto &window : windows) {
        for (const SubtitleOmniSegment &segment : request.segments) {
            if (segment.startMs >= window.second || segment.endMs <= window.first) continue;
            ++total;
            break;
        }
    }
    if (progress) {
        progress(0, std::max(1, total), QStringLiteral("正在准备 AI 复核…"));
    }
    int done = 0;
    for (const auto &window : windows) {
        if (aiCancelled(cancel)) return aiCancelledError();
        QVector<SubtitleOmniSegment> slice;
        for (const SubtitleOmniSegment &segment : request.segments) {
            if (segment.startMs >= window.second || segment.endMs <= window.first) continue;
            slice.append(segment);
        }
        if (slice.isEmpty()) continue;
        if (progress) {
            progress(done, total, QStringLiteral("正在复核窗口 %1/%2…").arg(done + 1).arg(total));
        }
        OmniChatRequest chat;
        chat.task = OmniTaskType::SubtitleReview;
        chat.systemPrompt = subtitleSystemPrompt();
        chat.userText = QString::fromUtf8(QJsonDocument(subtitleUserPayload(
            slice, window.first, request.scriptText)).toJson(QJsonDocument::Compact));
        chat.tools = QwenOmniProvider::subtitleToolSchema();
        chat.toolChoiceName = QStringLiteral("submit_subtitle_review");
        if (!request.mediaPath.isEmpty()) {
            MediaResult<OmniAudioClip> audio = ReviewAudioEncoder::encodeWindow(
                request.mediaPath, window.first, window.second, cancel);
            if (std::holds_alternative<OmniAudioClip>(audio)) {
                chat.audio = std::get<OmniAudioClip>(std::move(audio));
            } else if (std::holds_alternative<AppError>(audio)
                && isAiCancelError(std::get<AppError>(audio))) {
                return std::get<AppError>(std::move(audio));
            }
        }
        OmniCompleteResult completed = completeWithRetry(chat, cancel);
        if (std::holds_alternative<AppError>(completed)) {
            const AppError error = std::get<AppError>(std::move(completed));
            if (isFatalOmniError(error)) return error;
            for (const SubtitleOmniSegment &segment : slice) {
                if (byId.contains(segment.segmentId)) continue;
                SubtitleOmniSuggestion fallback;
                fallback.segmentId = segment.segmentId;
                fallback.originalText = segment.text;
                fallback.decision = SubtitleOmniDecision::Review;
                fallback.issueType = SubtitleIssueType::AsrUncertain;
                fallback.reason = QStringLiteral("AI Review 解析失败或超时，已标记复核");
                byId.insert(segment.segmentId, fallback);
            }
            ++done;
            if (progress) {
                progress(done, total, QStringLiteral("已完成窗口 %1/%2").arg(done).arg(total));
            }
            continue;
        }
        const OmniChatResponse response = std::get<OmniChatResponse>(std::move(completed));
        SubtitleOmniResult parsed = OmniReviewJson::parseSubtitleSuggestions(response.arguments);
        if (std::holds_alternative<AppError>(parsed)) {
            for (const SubtitleOmniSegment &segment : slice) {
                if (byId.contains(segment.segmentId)) continue;
                SubtitleOmniSuggestion fallback;
                fallback.segmentId = segment.segmentId;
                fallback.originalText = segment.text;
                fallback.decision = SubtitleOmniDecision::Review;
                fallback.reason = QStringLiteral("AI Review 返回无法解析，已标记复核");
                byId.insert(segment.segmentId, fallback);
            }
            ++done;
            if (progress) {
                progress(done, total, QStringLiteral("已完成窗口 %1/%2").arg(done).arg(total));
            }
            continue;
        }
        for (SubtitleOmniSuggestion suggestion : std::get<QVector<SubtitleOmniSuggestion>>(parsed)) {
            qCInfo(subcueAiLog) << "omni_review provider=" << provider_.providerId()
                << "model=" << provider_.model()
                << "task_type=subtitle"
                << "candidate_id=" << suggestion.segmentId
                << "decision=" << subtitleOmniDecisionName(suggestion.decision)
                << "confidence=" << suggestion.confidence
                << "request_duration=" << response.latencyMs
                << "token_usage=" << response.usage.totalTokens;
            byId.insert(suggestion.segmentId, std::move(suggestion));
        }
        ++done;
        if (progress) {
            progress(done, total, QStringLiteral("已完成窗口 %1/%2").arg(done).arg(total));
        }
    }
    QVector<SubtitleOmniSuggestion> suggestions;
    suggestions.reserve(byId.size());
    for (auto iterator = byId.cbegin(); iterator != byId.cend(); ++iterator) {
        suggestions.append(iterator.value());
    }
    return suggestions;
}

RoughCutOmniResult AiReviewService::reviewCandidates(
    const RoughCutOmniRequest &request,
    const std::atomic<bool> *cancel,
    OmniReviewProgress progress)
{
    QVector<RoughCutOmniSuggestion> results;
    results.reserve(request.candidates.size());
    const int total = request.candidates.size();
    if (progress) {
        progress(0, std::max(1, total), QStringLiteral("正在准备 AI 复核…"));
    }
    for (int index = 0; index < request.candidates.size(); ++index) {
        const RoughCutOmniCandidate &candidate = request.candidates.at(index);
        if (aiCancelled(cancel)) return aiCancelledError();
        if (progress) {
            progress(index, total, QStringLiteral("正在复核片段 %1/%2…").arg(index + 1).arg(total));
        }
        OmniChatRequest chat;
        chat.task = OmniTaskType::RoughCutReview;
        chat.systemPrompt = roughCutSystemPrompt();
        chat.userText = QString::fromUtf8(QJsonDocument(
            roughCutUserPayload(candidate, request.scriptText)).toJson(QJsonDocument::Compact));
        chat.tools = QwenOmniProvider::roughCutToolSchema();
        chat.toolChoiceName = QStringLiteral("submit_roughcut_review");
        if (!request.mediaPath.isEmpty()) {
            const qint64 start = std::max<qint64>(0, candidate.startMs - settings_.candidatePreMs);
            const qint64 end = candidate.endMs + settings_.candidatePostMs;
            MediaResult<OmniAudioClip> audio = ReviewAudioEncoder::encodeWindow(
                request.mediaPath, start, end, cancel);
            if (std::holds_alternative<OmniAudioClip>(audio)) {
                chat.audio = std::get<OmniAudioClip>(std::move(audio));
            } else if (std::holds_alternative<AppError>(audio)
                && isAiCancelError(std::get<AppError>(audio))) {
                return std::get<AppError>(std::move(audio));
            }
        }
        OmniCompleteResult completed = completeWithRetry(chat, cancel);
        auto fallback = [&](const QString &reason) {
            RoughCutOmniSuggestion suggestion;
            suggestion.candidateId = candidate.candidateId;
            suggestion.decision = RoughCutDecision::Review;
            suggestion.reasonType = RoughCutOmniReasonType::Uncertain;
            suggestion.reason = reason;
            qCWarning(subcueAiLog) << "omni_review provider=" << provider_.providerId()
                << "model=" << provider_.model()
                << "task_type=roughcut"
                << "candidate_id=" << candidate.candidateId
                << "decision=REVIEW"
                << "error=" << reason;
            results.append(std::move(suggestion));
        };
        if (std::holds_alternative<AppError>(completed)) {
            const AppError error = std::get<AppError>(std::move(completed));
            if (isFatalOmniError(error)) return error;
            qCWarning(subcueAiLog) << "omni_review provider=" << provider_.providerId()
                << "model=" << provider_.model()
                << "task_type=roughcut"
                << "candidate_id=" << candidate.candidateId
                << "error=" << error.userMessage();
            continue;
        }
        const OmniChatResponse response = std::get<OmniChatResponse>(std::move(completed));
        RoughCutOmniResult parsed = OmniReviewJson::parseRoughCutSuggestions(response.arguments);
        if (std::holds_alternative<AppError>(parsed)) {
            fallback(QStringLiteral("AI Review 返回无法解析，已标记复核"));
            continue;
        }
        QVector<RoughCutOmniSuggestion> parsedItems =
            std::get<QVector<RoughCutOmniSuggestion>>(std::move(parsed));
        if (parsedItems.isEmpty()) {
            fallback(QStringLiteral("AI Review 未返回候选结果，已标记复核"));
            continue;
        }
        RoughCutOmniSuggestion suggestion = parsedItems.takeFirst();
        if (suggestion.candidateId != candidate.candidateId) {
            suggestion.candidateId = candidate.candidateId;
        }
        suggestion.decision = gatedCutDecision(suggestion, settings_);
        qCInfo(subcueAiLog) << "omni_review provider=" << provider_.providerId()
            << "model=" << provider_.model()
            << "task_type=roughcut"
            << "candidate_id=" << suggestion.candidateId
            << "decision=" << roughCutDecisionName(suggestion.decision)
            << "confidence=" << suggestion.confidence
            << "request_duration=" << response.latencyMs
            << "token_usage=" << response.usage.totalTokens;
        results.append(std::move(suggestion));
    }
    return results;
}

WordMappingOmniResult AiReviewService::reviewWordMapping(
    const WordMappingOmniRequest &request,
    const std::atomic<bool> *cancel,
    OmniReviewProgress progress)
{
    QVector<Subtitle> subtitles = request.subtitles;
    if (request.words.isEmpty()) {
        if (progress) progress(1, 1, QStringLiteral("没有可复核的词级证据"));
        return subtitles;
    }
    int total = 0;
    for (const Subtitle &cue : subtitles) {
        if (AlignmentEngine::needsAiReview(cue.confidence, cue.ambiguity)) ++total;
    }
    if (progress) {
        progress(0, std::max(1, total),
            total > 0 ? QStringLiteral("正在准备时间映射复核…")
                      : QStringLiteral("没有需要复核的字幕"));
    }
    int done = 0;
    for (int index = 0; index < subtitles.size(); ++index) {
        if (aiCancelled(cancel)) return aiCancelledError();
        if (!AlignmentEngine::needsAiReview(subtitles.at(index).confidence, subtitles.at(index).ambiguity)) {
            continue;
        }
        if (progress) {
            progress(done, total, QStringLiteral("正在复核时间映射 %1/%2…").arg(done + 1).arg(total));
        }
        OmniChatRequest chat;
        chat.task = OmniTaskType::WordMappingReview;
        chat.systemPrompt = wordMappingSystemPrompt();
        chat.userText = OpenAICompatibleProvider::buildUserPrompt(subtitles, request.words, index);
        chat.tools = QwenOmniProvider::wordMappingToolSchema();
        chat.toolChoiceName = QStringLiteral("submit_word_mapping");
        OmniCompleteResult completed = completeWithRetry(chat, cancel);
        if (std::holds_alternative<AppError>(completed)) {
            const AppError error = std::get<AppError>(std::move(completed));
            if (isFatalOmniError(error)) return error;
            subtitles[index].metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("failed"));
            ++done;
            if (progress) {
                progress(done, total, QStringLiteral("已完成时间映射 %1/%2").arg(done).arg(total));
            }
            continue;
        }
        const OmniChatResponse response = std::get<OmniChatResponse>(std::move(completed));
        auto parsed = OmniReviewJson::parseWordMappings(response.arguments);
        if (std::holds_alternative<AppError>(parsed)) {
            subtitles[index].metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("invalid"));
            ++done;
            if (progress) {
                progress(done, total, QStringLiteral("已完成时间映射 %1/%2").arg(done).arg(total));
            }
            continue;
        }
        const QString expectedLine = QStringLiteral("L%1").arg(index + 1);
        QJsonObject mapping;
        for (const QJsonObject &item : std::get<QVector<QJsonObject>>(parsed)) {
            if (item.value(QStringLiteral("line_id")).toString() == expectedLine) {
                mapping = item;
                break;
            }
        }
        if (mapping.isEmpty()) {
            subtitles[index].metadata.insert(QStringLiteral("aiReviewStatus"), QStringLiteral("invalid"));
            ++done;
            if (progress) {
                progress(done, total, QStringLiteral("已完成时间映射 %1/%2").arg(done).arg(total));
            }
            continue;
        }
        const QString status = mapping.value(QStringLiteral("status")).toString();
        subtitles[index].metadata.insert(QStringLiteral("aiReviewStatus"), status);
        if (status == QLatin1String("matched")) {
            if (!OpenAICompatibleProvider::applyIfValid(
                    subtitles[index], index, mapping, subtitles, request.words)) {
                subtitles[index].metadata.insert(QStringLiteral("aiReviewStatus"),
                    QStringLiteral("rejectedLocally"));
                subtitles[index].skipReason = QStringLiteral("AI 映射未通过本地音频校验");
            }
        }
        ++done;
        if (progress) {
            progress(done, total, QStringLiteral("已完成时间映射 %1/%2").arg(done).arg(total));
        }
    }
    return subtitles;
}

} // namespace subcue
