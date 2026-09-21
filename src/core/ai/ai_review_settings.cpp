#include "ai/ai_review_settings.h"

#include <QtCore/QProcessEnvironment>
#include <algorithm>

namespace subcue {
namespace {

int clampPositive(int value, int fallback, int minimum, int maximum)
{
    if (value <= 0) return fallback;
    return std::clamp(value, minimum, maximum);
}

double clampUnit(double value, double fallback)
{
    if (value < 0.0 || value > 1.0) return fallback;
    return value;
}

} // namespace

OmniReviewSettings OmniReviewSettingsStore::fromJson(const QJsonObject &settings)
{
    OmniReviewSettings result;
    result.provider = QString::fromLatin1(kOmniReviewProviderId);
    result.model = QString::fromLatin1(kOmniReviewDefaultModel);
    result.baseUrl = QString::fromLatin1(kOmniReviewDefaultBaseUrl);
    result.reasoningEffort = OmniReasoningEffort::Low;
    result.timeoutMs = clampPositive(
        settings.value(QStringLiteral("omniReviewTimeoutMs")).toInt(), 120'000, 5'000, 600'000);
    result.cutConfidence = clampUnit(
        settings.value(QStringLiteral("omniReviewCutConfidence")).toDouble(), 0.90);
    result.reviewConfidence = clampUnit(
        settings.value(QStringLiteral("omniReviewReviewConfidence")).toDouble(), 0.65);
    if (result.reviewConfidence > result.cutConfidence)
        result.reviewConfidence = result.cutConfidence;
    result.minTailMs = clampPositive(
        settings.value(QStringLiteral("omniReviewMinTailMs")).toInt(), 150, 0, 2'000);
    result.preferredTailMs = clampPositive(
        settings.value(QStringLiteral("omniReviewPreferredTailMs")).toInt(), 250, result.minTailMs, 5'000);
    result.maxTailMs = clampPositive(
        settings.value(QStringLiteral("omniReviewMaxTailMs")).toInt(), 500, result.preferredTailMs, 10'000);
    result.handleMs = clampPositive(
        settings.value(QStringLiteral("omniReviewHandleMs")).toInt(), 150, 0, 2'000);
    result.subtitleWindowMs = clampPositive(
        settings.value(QStringLiteral("omniReviewWindowMs")).toInt(),
        kOmniSubtitleWindowDefaultMs, kOmniSubtitleWindowMinMs, kOmniSubtitleWindowMaxMs);
    result.subtitleOverlapMs = clampPositive(
        settings.value(QStringLiteral("omniReviewOverlapMs")).toInt(), 15'000, 0, 120'000);
    result.candidatePreMs = clampPositive(
        settings.value(QStringLiteral("omniReviewCandidatePreMs")).toInt(), 8'000, 1'000, 30'000);
    result.candidatePostMs = clampPositive(
        settings.value(QStringLiteral("omniReviewCandidatePostMs")).toInt(), 15'000, 1'000, 40'000);
    return result;
}

QJsonObject OmniReviewSettingsStore::toJsonPatch(const OmniReviewSettings &settings)
{
    return {
        {QStringLiteral("omniReviewTimeoutMs"), settings.timeoutMs},
        {QStringLiteral("omniReviewCutConfidence"), settings.cutConfidence},
        {QStringLiteral("omniReviewReviewConfidence"), settings.reviewConfidence},
        {QStringLiteral("omniReviewMinTailMs"), settings.minTailMs},
        {QStringLiteral("omniReviewPreferredTailMs"), settings.preferredTailMs},
        {QStringLiteral("omniReviewMaxTailMs"), settings.maxTailMs},
        {QStringLiteral("omniReviewHandleMs"), settings.handleMs},
        {QStringLiteral("omniReviewWindowMs"), settings.subtitleWindowMs},
        {QStringLiteral("omniReviewOverlapMs"), settings.subtitleOverlapMs},
        {QStringLiteral("omniReviewCandidatePreMs"), settings.candidatePreMs},
        {QStringLiteral("omniReviewCandidatePostMs"), settings.candidatePostMs},
    };
}

QString OmniReviewSettingsStore::resolveApiKey(const QString &uiKey, const ICredentialStore *store)
{
    switch (resolveApiKeySource(uiKey, store)) {
    case AiKeySource::Ui: return uiKey.trimmed();
    case AiKeySource::Saved: return store ? store->load(QString::fromLatin1(kOmniReviewCredentialId)) : QString();
    case AiKeySource::Environment:
        return QProcessEnvironment::systemEnvironment()
            .value(QString::fromLatin1(kOmniReviewEnvKey))
            .trimmed();
    case AiKeySource::None: return {};
    }
    return {};
}

AiKeySource OmniReviewSettingsStore::resolveApiKeySource(
    const QString &uiKey, const ICredentialStore *store, bool ignoreSaved)
{
    if (!uiKey.trimmed().isEmpty()) return AiKeySource::Ui;
    if (!ignoreSaved && store) {
        const QString saved = store->load(QString::fromLatin1(kOmniReviewCredentialId)).trimmed();
        if (!saved.isEmpty()) return AiKeySource::Saved;
    }
    const QString env = QProcessEnvironment::systemEnvironment()
        .value(QString::fromLatin1(kOmniReviewEnvKey))
        .trimmed();
    if (!env.isEmpty()) return AiKeySource::Environment;
    return AiKeySource::None;
}

QString OmniReviewSettingsStore::resolveApiKeyLabel(
    const QString &uiKey, const ICredentialStore *store, bool ignoreSaved)
{
    return aiKeySourceLabel(resolveApiKeySource(uiKey, store, ignoreSaved));
}

} // namespace subcue
