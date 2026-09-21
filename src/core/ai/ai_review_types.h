#pragma once

#include "alignment/transcript.h"
#include "common/app_error.h"
#include "common/provider_types.h"
#include "roughcut/decision_engine.h"
#include "subtitle/subtitle.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>
#include <variant>

namespace subcue {

inline constexpr auto kOmniReviewProviderId = "qwen_omni";
inline constexpr auto kOmniReviewCredentialId = "SubCue/AIReview/qwen_omni";
inline constexpr auto kOmniReviewEnvKey = "DASHSCOPE_API_KEY";
inline constexpr auto kOmniReviewDefaultModel = "qwen3.8-omni-flash";
inline constexpr auto kOmniReviewDefaultBaseUrl =
    "https://dashscope.aliyuncs.com/compatible-mode/v1";
inline constexpr int kOmniSubtitleWindowDefaultMs = 90'000;
inline constexpr int kOmniSubtitleWindowMinMs = 15'000;
inline constexpr int kOmniSubtitleWindowMaxMs = 90'000;

enum class OmniReasoningEffort { None, Minimal, Low, Medium, High };

enum class OmniTaskType { ConnectionTest, SubtitleReview, RoughCutReview, WordMappingReview };

enum class AiKeySource { None, Ui, Saved, Environment };

enum class SubtitleIssueType {
    MissingText,
    WrongText,
    Duplicate,
    BadSplit,
    BadMerge,
    Punctuation,
    AsrUncertain,
    ScriptMismatch
};

enum class SubtitleOmniDecision { Keep, Review, ReplaceText };

enum class RoughCutOmniReasonType {
    Retake,
    FalseStart,
    Duplicate,
    Stumble,
    WrongTake,
    Interruption,
    NoiseTake,
    Uncertain
};

enum class OmniReviewStatus {
    Normal,
    Suggested,
    Review,
    Accepted,
    Ignored
};

struct OmniReviewSettings final {
    QString provider = QString::fromLatin1(kOmniReviewProviderId);
    QString model = QString::fromLatin1(kOmniReviewDefaultModel);
    QString baseUrl = QString::fromLatin1(kOmniReviewDefaultBaseUrl);
    OmniReasoningEffort reasoningEffort = OmniReasoningEffort::Low;
    int timeoutMs = 120'000;
    double cutConfidence = 0.90;
    double reviewConfidence = 0.65;
    int minTailMs = 150;
    int preferredTailMs = 250;
    int maxTailMs = 500;
    int handleMs = 150;
    int subtitleWindowMs = kOmniSubtitleWindowDefaultMs;
    int subtitleOverlapMs = 15'000;
    int candidatePreMs = 8'000;
    int candidatePostMs = 15'000;
};

struct OmniAudioClip final {
    QByteArray data;
    QString format;
    int sampleRate = 16'000;
    qint64 windowStartMs = 0;
    qint64 windowEndMs = 0;
};

struct OmniChatRequest final {
    OmniTaskType task = OmniTaskType::ConnectionTest;
    QString systemPrompt;
    QString userText;
    OmniAudioClip audio;
    QJsonArray tools;
    QString toolChoiceName;
};

struct OmniUsage final {
    qint64 promptTokens = -1;
    qint64 completionTokens = -1;
    qint64 totalTokens = -1;
};

struct OmniChatResponse final {
    QJsonObject arguments;
    QString rawContent;
    bool fromToolCall = false;
    qint64 latencyMs = 0;
    OmniUsage usage;
    QString model;
};

struct OmniConnectionTestResult final {
    QString model;
    qint64 latencyMs = 0;
    OmniUsage usage;
    QDateTime verifiedAtUtc;
};

struct SubtitleOmniSegment final {
    QString segmentId;
    QString text;
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString previousText;
    QString nextText;
};

struct SubtitleOmniRequest final {
    QString mediaPath;
    QString scriptText;
    QVector<SubtitleOmniSegment> segments;
};

struct SubtitleOmniSuggestion final {
    QString segmentId;
    SubtitleIssueType issueType = SubtitleIssueType::AsrUncertain;
    double confidence = 0.0;
    QString originalText;
    QString suggestedText;
    QString reason;
    SubtitleOmniDecision decision = SubtitleOmniDecision::Review;
};

struct RoughCutOmniCandidate final {
    QString candidateId;
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString transcript;
    QString previousContext;
    QString nextContext;
    QString scriptNearby;
    QString reasonHint;
    QString replacementCandidateId;
};

struct RoughCutOmniRequest final {
    QString mediaPath;
    QString scriptText;
    QVector<RoughCutOmniCandidate> candidates;
};

struct WordMappingOmniRequest final {
    QVector<Subtitle> subtitles;
    QVector<TranscriptWord> words;
};

struct RoughCutOmniSuggestion final {
    QString candidateId;
    RoughCutDecision decision = RoughCutDecision::Review;
    RoughCutOmniReasonType reasonType = RoughCutOmniReasonType::Uncertain;
    double confidence = 0.0;
    QString replacementCandidateId;
    QString reason;
};

using OmniCompleteResult = std::variant<OmniChatResponse, AppError>;
using OmniTestResult = std::variant<OmniConnectionTestResult, AppError>;
using SubtitleOmniResult = std::variant<QVector<SubtitleOmniSuggestion>, AppError>;
using RoughCutOmniResult = std::variant<QVector<RoughCutOmniSuggestion>, AppError>;
using WordMappingOmniResult = std::variant<QVector<Subtitle>, AppError>;

[[nodiscard]] inline QString omniReasoningName(OmniReasoningEffort effort)
{
    switch (effort) {
    case OmniReasoningEffort::None: return QStringLiteral("none");
    case OmniReasoningEffort::Minimal: return QStringLiteral("minimal");
    case OmniReasoningEffort::Low: return QStringLiteral("low");
    case OmniReasoningEffort::Medium: return QStringLiteral("medium");
    case OmniReasoningEffort::High: return QStringLiteral("high");
    }
    return QStringLiteral("low");
}

[[nodiscard]] inline OmniReasoningEffort omniReasoningFromName(QStringView name)
{
    if (name == QLatin1String("none")) return OmniReasoningEffort::None;
    if (name == QLatin1String("minimal")) return OmniReasoningEffort::Minimal;
    if (name == QLatin1String("medium")) return OmniReasoningEffort::Medium;
    if (name == QLatin1String("high")) return OmniReasoningEffort::High;
    return OmniReasoningEffort::Low;
}

[[nodiscard]] inline QString subtitleIssueName(SubtitleIssueType type)
{
    switch (type) {
    case SubtitleIssueType::MissingText: return QStringLiteral("MISSING_TEXT");
    case SubtitleIssueType::WrongText: return QStringLiteral("WRONG_TEXT");
    case SubtitleIssueType::Duplicate: return QStringLiteral("DUPLICATE");
    case SubtitleIssueType::BadSplit: return QStringLiteral("BAD_SPLIT");
    case SubtitleIssueType::BadMerge: return QStringLiteral("BAD_MERGE");
    case SubtitleIssueType::Punctuation: return QStringLiteral("PUNCTUATION");
    case SubtitleIssueType::AsrUncertain: return QStringLiteral("ASR_UNCERTAIN");
    case SubtitleIssueType::ScriptMismatch: return QStringLiteral("SCRIPT_MISMATCH");
    }
    return QStringLiteral("ASR_UNCERTAIN");
}

[[nodiscard]] inline std::optional<SubtitleIssueType> subtitleIssueFromName(QStringView name)
{
    if (name == QLatin1String("MISSING_TEXT")) return SubtitleIssueType::MissingText;
    if (name == QLatin1String("WRONG_TEXT")) return SubtitleIssueType::WrongText;
    if (name == QLatin1String("DUPLICATE")) return SubtitleIssueType::Duplicate;
    if (name == QLatin1String("BAD_SPLIT")) return SubtitleIssueType::BadSplit;
    if (name == QLatin1String("BAD_MERGE")) return SubtitleIssueType::BadMerge;
    if (name == QLatin1String("PUNCTUATION")) return SubtitleIssueType::Punctuation;
    if (name == QLatin1String("ASR_UNCERTAIN")) return SubtitleIssueType::AsrUncertain;
    if (name == QLatin1String("SCRIPT_MISMATCH")) return SubtitleIssueType::ScriptMismatch;
    return std::nullopt;
}

[[nodiscard]] inline QString subtitleOmniDecisionName(SubtitleOmniDecision decision)
{
    switch (decision) {
    case SubtitleOmniDecision::Keep: return QStringLiteral("KEEP");
    case SubtitleOmniDecision::Review: return QStringLiteral("REVIEW");
    case SubtitleOmniDecision::ReplaceText: return QStringLiteral("REPLACE_TEXT");
    }
    return QStringLiteral("REVIEW");
}

[[nodiscard]] inline std::optional<SubtitleOmniDecision> subtitleOmniDecisionFromName(QStringView name)
{
    if (name == QLatin1String("KEEP")) return SubtitleOmniDecision::Keep;
    if (name == QLatin1String("REVIEW")) return SubtitleOmniDecision::Review;
    if (name == QLatin1String("REPLACE_TEXT")) return SubtitleOmniDecision::ReplaceText;
    return std::nullopt;
}

[[nodiscard]] inline QString roughCutOmniReasonName(RoughCutOmniReasonType type)
{
    switch (type) {
    case RoughCutOmniReasonType::Retake: return QStringLiteral("RETAKE");
    case RoughCutOmniReasonType::FalseStart: return QStringLiteral("FALSE_START");
    case RoughCutOmniReasonType::Duplicate: return QStringLiteral("DUPLICATE");
    case RoughCutOmniReasonType::Stumble: return QStringLiteral("STUMBLE");
    case RoughCutOmniReasonType::WrongTake: return QStringLiteral("WRONG_TAKE");
    case RoughCutOmniReasonType::Interruption: return QStringLiteral("INTERRUPTION");
    case RoughCutOmniReasonType::NoiseTake: return QStringLiteral("NOISE_TAKE");
    case RoughCutOmniReasonType::Uncertain: return QStringLiteral("UNCERTAIN");
    }
    return QStringLiteral("UNCERTAIN");
}

[[nodiscard]] inline std::optional<RoughCutOmniReasonType> roughCutOmniReasonFromName(QStringView name)
{
    if (name == QLatin1String("RETAKE")) return RoughCutOmniReasonType::Retake;
    if (name == QLatin1String("FALSE_START")) return RoughCutOmniReasonType::FalseStart;
    if (name == QLatin1String("DUPLICATE")) return RoughCutOmniReasonType::Duplicate;
    if (name == QLatin1String("STUMBLE")) return RoughCutOmniReasonType::Stumble;
    if (name == QLatin1String("WRONG_TAKE")) return RoughCutOmniReasonType::WrongTake;
    if (name == QLatin1String("INTERRUPTION")) return RoughCutOmniReasonType::Interruption;
    if (name == QLatin1String("NOISE_TAKE")) return RoughCutOmniReasonType::NoiseTake;
    if (name == QLatin1String("UNCERTAIN")) return RoughCutOmniReasonType::Uncertain;
    return std::nullopt;
}

[[nodiscard]] inline QString omniReviewStatusName(OmniReviewStatus status)
{
    switch (status) {
    case OmniReviewStatus::Normal: return QStringLiteral("normal");
    case OmniReviewStatus::Suggested: return QStringLiteral("suggested");
    case OmniReviewStatus::Review: return QStringLiteral("review");
    case OmniReviewStatus::Accepted: return QStringLiteral("accepted");
    case OmniReviewStatus::Ignored: return QStringLiteral("ignored");
    }
    return QStringLiteral("normal");
}

[[nodiscard]] inline QString roughCutDecisionName(RoughCutDecision decision)
{
    switch (decision) {
    case RoughCutDecision::Keep: return QStringLiteral("KEEP");
    case RoughCutDecision::Cut: return QStringLiteral("CUT");
    case RoughCutDecision::Review: return QStringLiteral("REVIEW");
    }
    return QStringLiteral("REVIEW");
}

[[nodiscard]] inline std::optional<RoughCutDecision> roughCutDecisionFromName(QStringView name)
{
    if (name == QLatin1String("KEEP")) return RoughCutDecision::Keep;
    if (name == QLatin1String("CUT")) return RoughCutDecision::Cut;
    if (name == QLatin1String("REVIEW")) return RoughCutDecision::Review;
    return std::nullopt;
}

[[nodiscard]] inline bool omniConfidenceValid(double confidence) noexcept
{
    return confidence >= 0.0 && confidence <= 1.0;
}

[[nodiscard]] inline QString aiKeySourceLabel(AiKeySource source)
{
    switch (source) {
    case AiKeySource::Ui: return QStringLiteral("当前来源：本次输入");
    case AiKeySource::Saved: return QStringLiteral("当前来源：已保存密钥");
    case AiKeySource::Environment: return QStringLiteral("当前来源：环境变量 DASHSCOPE_API_KEY");
    case AiKeySource::None: return QStringLiteral("尚未配置");
    }
    return QStringLiteral("尚未配置");
}

} // namespace subcue
