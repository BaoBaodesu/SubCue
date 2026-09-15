#include "rough_cut_result_model.h"

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
    default: return {};
    }
}

QHash<int, QByteArray> RoughCutResultModel::roleNames() const
{
    return {{StatusRole, "status"}, {TextRole, "text"}, {ReasonRole, "reason"},
            {EvidenceRole, "evidence"}, {StartMsRole, "startMs"}, {EndMsRole, "endMs"},
            {UserOverrideRole, "userOverride"}};
}

void RoughCutResultModel::reset(QVector<RecognizedPassage> recording,
                                QVector<RoughCutSegmentDecision> decisions, int sampleRate)
{
    beginResetModel();
    recording_ = std::move(recording);
    decisions_ = std::move(decisions);
    sampleRate_ = sampleRate;
    endResetModel();
}

bool RoughCutResultModel::setUserDecision(int row, std::optional<RoughCutDecision> decision)
{
    if (row < 0 || row >= decisions_.size() || decisions_.at(row).userDecision == decision) return false;
    decisions_[row].userDecision = decision;
    emit dataChanged(index(row), index(row), {StatusRole, UserOverrideRole});
    return true;
}

void RoughCutResultModel::applyAuxiliaryResult(RoughCutAuxiliaryResult result)
{
    if (result.recordingIndex < 0 || result.recordingIndex >= decisions_.size()) return;
    RoughCutSegmentDecision &decision = decisions_[result.recordingIndex];
    decision.autoDecision = RoughCutAuxiliaryRecognition::reconcile(decision.autoDecision, &result);
    decision.evidence.append(QStringLiteral("Fun-ASR：%1").arg(
        result.funAsrFailed ? QStringLiteral("失败") : result.funAsrText));
    decision.evidence.append(QStringLiteral("Whisper：%1").arg(
        result.whisperFailed ? QStringLiteral("失败") : result.whisperText));
    if (result.conflict) decision.reason = QStringLiteral("辅助识别冲突或失败，保留复核");
    const QModelIndex changed = index(result.recordingIndex);
    emit dataChanged(changed, changed, {StatusRole, ReasonRole, EvidenceRole});
}

} // namespace subcue
