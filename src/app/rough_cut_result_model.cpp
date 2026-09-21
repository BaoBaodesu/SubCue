#include "rough_cut_result_model.h"

#include <algorithm>

namespace subcue {
namespace {

QString decisionName(RoughCutDecision decision)
{
    switch (decision) {
    case RoughCutDecision::Keep: return QStringLiteral("KEEP");
    case RoughCutDecision::Cut: return QStringLiteral("CUT");
    case RoughCutDecision::Review: return QStringLiteral("REVIEW");
    }
    return QStringLiteral("REVIEW");
}

QString statusLabel(RoughCutDecision decision)
{
    switch (decision) {
    case RoughCutDecision::Keep: return QStringLiteral("保留");
    case RoughCutDecision::Cut: return QStringLiteral("剪除");
    case RoughCutDecision::Review: return QStringLiteral("复核");
    }
    return QStringLiteral("复核");
}

QString failureLabel(RoughCutFailureType type)
{
    switch (type) {
    case RoughCutFailureType::Retake: return QStringLiteral("重录");
    case RoughCutFailureType::Interrupted: return QStringLiteral("中断");
    case RoughCutFailureType::Duplicate: return QStringLiteral("重复");
    case RoughCutFailureType::WrongTake: return QStringLiteral("错 Take");
    case RoughCutFailureType::Filler: return QStringLiteral("垫词");
    case RoughCutFailureType::None: return {};
    }
    return {};
}

QString shortReason(const QString &reason)
{
    constexpr int kLimit = 36;
    if (reason.size() <= kLimit) return reason;
    return reason.left(kLimit) + QStringLiteral("…");
}

} // namespace

RoughCutResultModel::RoughCutResultModel(QObject *parent) : QAbstractListModel(parent) {}

int RoughCutResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : recording_.size();
}

QVariant RoughCutResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= recording_.size()) return {};
    const RecognizedPassage &passage = recording_.at(index.row());
    const RoughCutSegmentDecision &decision = decisions_.at(index.row());
    switch (role) {
    case StatusRole: return decisionName(decision.effectiveDecision());
    case TextRole: return passage.text;
    case ReasonRole: return decision.reason;
    case EvidenceRole: return decision.evidence;
    case StartMsRole: return sampleRate_ > 0 ? passage.startSample * 1000 / sampleRate_ : 0;
    case EndMsRole: return sampleRate_ > 0 ? passage.endSample * 1000 / sampleRate_ : 0;
    case UserOverrideRole: return decision.userDecision.has_value();
    case TakeGroupRole: return decision.takeGroupId;
    case BestTakeRole: return decision.bestTake;
    case ScoreRole: return decision.ruleScore;
    case FailureTypeRole: return roughCutFailureName(decision.failureType);
    case ModelProbabilityRole: return decision.modelProbability;
    case DecisionSourceRole: return decision.decisionSource;
    case ScriptRangeRole: return passage.scriptLineIndex < 0 ? QStringLiteral("文案外")
        : passage.scriptLineEndIndex > passage.scriptLineIndex
            ? QStringLiteral("文案第 %1–%2 行").arg(passage.scriptLineIndex + 1)
                .arg(passage.scriptLineEndIndex + 1)
            : QStringLiteral("文案第 %1 行").arg(passage.scriptLineIndex + 1);
    case ReplacementRole: return decision.replacementRecordingIndex < 0 ? QString()
        : QStringLiteral("由片段 %1 替代").arg(decision.replacementRecordingIndex + 1);
    case RecordingIndexRole: return index.row();
    case StatusLabelRole: return statusLabel(decision.effectiveDecision());
    case ShortReasonRole: return shortReason(decision.reason);
    case FailureLabelRole: return failureLabel(decision.failureType);
    case EvidenceTextRole: return decision.evidence.isEmpty()
        ? QStringLiteral("无") : decision.evidence.join(QStringLiteral("；"));
    case TechnicalDetailsRole: {
        QStringList parts;
        if (decision.failureType != RoughCutFailureType::None)
            parts.append(roughCutFailureName(decision.failureType));
        if (!decision.decisionSource.isEmpty()) parts.append(decision.decisionSource);
        if (decision.modelProbability >= 0)
            parts.append(QStringLiteral("置信度 %1%").arg(int(decision.modelProbability * 100)));
        return parts.join(QStringLiteral(" · "));
    }
    default: return {};
    }
}

QHash<int, QByteArray> RoughCutResultModel::roleNames() const
{
    return {{StatusRole, "status"}, {TextRole, "text"}, {ReasonRole, "reason"},
            {EvidenceRole, "evidence"}, {StartMsRole, "startMs"}, {EndMsRole, "endMs"},
            {UserOverrideRole, "userOverride"}, {TakeGroupRole, "takeGroup"},
            {BestTakeRole, "bestTake"}, {ScoreRole, "score"},
            {FailureTypeRole, "failureType"}, {ModelProbabilityRole, "modelProbability"},
            {DecisionSourceRole, "decisionSource"},
            {ScriptRangeRole, "scriptRange"}, {ReplacementRole, "replacement"},
            {RecordingIndexRole, "recordingIndex"}, {StatusLabelRole, "statusLabel"},
            {ShortReasonRole, "shortReason"}, {FailureLabelRole, "failureLabel"},
            {EvidenceTextRole, "evidenceText"}, {TechnicalDetailsRole, "technicalDetails"}};
}

void RoughCutResultModel::reset(QVector<RecognizedPassage> recording,
                                QVector<RoughCutSegmentDecision> decisions, int sampleRate,
                                QString scriptText)
{
    beginResetModel();
    recording_ = std::move(recording);
    baseDecisions_ = std::move(decisions);
    sampleRate_ = sampleRate;
    scriptText_ = std::move(scriptText);
    refreshProtectedDecisions();
    endResetModel();
}

bool RoughCutResultModel::setUserDecision(int row, std::optional<RoughCutDecision> decision)
{
    if (row < 0 || row >= decisions_.size() || decisions_.at(row).userDecision == decision) return false;
    baseDecisions_[row].userDecision = decision;
    refreshProtectedDecisions();
    emit dataChanged(index(0), index(recording_.size() - 1),
        {StatusRole, UserOverrideRole, ReasonRole, EvidenceRole, ReplacementRole,
         StatusLabelRole, ShortReasonRole});
    return true;
}

void RoughCutResultModel::replaceBaseDecisions(QVector<RoughCutSegmentDecision> decisions, bool preserveUser)
{
    if (decisions.size() != baseDecisions_.size()) return;
    if (preserveUser) {
        for (int index = 0; index < decisions.size(); ++index) {
            if (baseDecisions_.at(index).userDecision)
                decisions[index].userDecision = baseDecisions_.at(index).userDecision;
        }
    }
    baseDecisions_ = std::move(decisions);
    refreshProtectedDecisions();
    if (!recording_.isEmpty()) {
        emit dataChanged(index(0), index(recording_.size() - 1),
            {StatusRole, ReasonRole, EvidenceRole, ReplacementRole, DecisionSourceRole,
             ModelProbabilityRole, UserOverrideRole, StatusLabelRole, ShortReasonRole,
             FailureLabelRole, EvidenceTextRole, TechnicalDetailsRole});
    }
}

void RoughCutResultModel::refreshProtectedDecisions()
{
    decisions_ = baseDecisions_;
    RoughCutDecisionEngine::protectCuts(recording_, &decisions_, scriptText_);
    recount();
}

void RoughCutResultModel::recount()
{
    int keep = 0;
    int review = 0;
    int cut = 0;
    for (const RoughCutSegmentDecision &decision : decisions_) {
        switch (decision.effectiveDecision()) {
        case RoughCutDecision::Keep: ++keep; break;
        case RoughCutDecision::Cut: ++cut; break;
        case RoughCutDecision::Review: ++review; break;
        }
    }
    if (keep == keepCount_ && review == reviewCount_ && cut == cutCount_) return;
    keepCount_ = keep;
    reviewCount_ = review;
    cutCount_ = cut;
    emit countsChanged();
}

int RoughCutResultModel::nextReviewRow(int fromSourceRow) const
{
    if (decisions_.isEmpty()) return -1;
    const int start = std::max(-1, fromSourceRow);
    for (int row = start + 1; row < decisions_.size(); ++row) {
        if (decisions_.at(row).effectiveDecision() == RoughCutDecision::Review) return row;
    }
    for (int row = 0; row <= start && row < decisions_.size(); ++row) {
        if (decisions_.at(row).effectiveDecision() == RoughCutDecision::Review) return row;
    }
    return -1;
}

int RoughCutResultModel::previousReviewRow(int fromSourceRow) const
{
    if (decisions_.isEmpty()) return -1;
    const int start = fromSourceRow < 0 ? decisions_.size() : fromSourceRow;
    for (int row = start - 1; row >= 0; --row) {
        if (decisions_.at(row).effectiveDecision() == RoughCutDecision::Review) return row;
    }
    for (int row = decisions_.size() - 1; row >= start && row >= 0; --row) {
        if (decisions_.at(row).effectiveDecision() == RoughCutDecision::Review) return row;
    }
    return -1;
}

RoughCutResultFilterModel::RoughCutResultFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void RoughCutResultFilterModel::setStatusFilter(const QString &value)
{
    const QString next = value.isEmpty() ? QStringLiteral("ALL") : value;
    if (statusFilter_ == next) return;
    statusFilter_ = next;
    invalidateFilter();
    emit statusFilterChanged();
}

bool RoughCutResultFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (statusFilter_ == QStringLiteral("ALL") || sourceModel() == nullptr) return true;
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return sourceModel()->data(index, RoughCutResultModel::StatusRole).toString() == statusFilter_;
}

void RoughCutResultModel::applyAuxiliaryResult(
    RoughCutAuxiliaryResult result, const QString &providerName)
{
    applyAuxiliaryResults({std::move(result)}, providerName);
}

void RoughCutResultModel::applyAuxiliaryResults(
    const QVector<RoughCutAuxiliaryResult> &results, const QString &providerName)
{
    if (results.isEmpty() || baseDecisions_.isEmpty()) return;
    bool changed = false;
    for (RoughCutAuxiliaryResult result : results) {
        if (result.recordingIndex < 0 || result.recordingIndex >= baseDecisions_.size()) continue;
        RoughCutSegmentDecision &decision = baseDecisions_[result.recordingIndex];
        if (decision.failureType != RoughCutFailureType::None)
            result.agreementDecision = RoughCutDecision::Cut;
        else
            result.agreementDecision = RoughCutDecision::Keep;
        decision.autoDecision = RoughCutAuxiliaryRecognition::reconcile(decision.autoDecision, &result);
        if (!result.conflict) decision.decisionSource = QStringLiteral("rule+review-asr");
        decision.evidence.append(QStringLiteral("%1：%2").arg(providerName,
            result.funAsrFailed ? QStringLiteral("失败") : result.funAsrText));
        if (result.conflict) decision.reason = QStringLiteral("辅助识别冲突或失败，保留复核");
        changed = true;
    }
    if (!changed) return;
    refreshProtectedDecisions();
    if (!recording_.isEmpty())
        emit dataChanged(index(0), index(recording_.size() - 1),
            {StatusRole, ReasonRole, EvidenceRole, ReplacementRole, DecisionSourceRole,
             StatusLabelRole, ShortReasonRole, FailureLabelRole, EvidenceTextRole,
             TechnicalDetailsRole});
}

} // namespace subcue
