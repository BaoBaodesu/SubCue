#pragma once

#include "roughcut/decision_engine.h"
#include "roughcut/script_matcher.h"
#include "roughcut/auxiliary_recognition.h"

#include <QtCore/QAbstractListModel>

namespace subcue {

class RoughCutResultModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        StatusRole = Qt::UserRole + 1,
        TextRole,
        ReasonRole,
        EvidenceRole,
        StartMsRole,
        EndMsRole,
        UserOverrideRole,
        TakeGroupRole,
        BestTakeRole,
        ScoreRole,
        FailureTypeRole,
        ModelProbabilityRole,
        DecisionSourceRole,
        ScriptRangeRole,
        ReplacementRole
    };

    explicit RoughCutResultModel(QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void reset(QVector<RecognizedPassage> recording,
               QVector<RoughCutSegmentDecision> decisions, int sampleRate,
               QString scriptText = {});
    [[nodiscard]] const QVector<RecognizedPassage> &recording() const noexcept { return recording_; }
    [[nodiscard]] const QVector<RoughCutSegmentDecision> &decisions() const noexcept { return decisions_; }
    [[nodiscard]] const QVector<RoughCutSegmentDecision> &baseDecisions() const noexcept { return baseDecisions_; }
    [[nodiscard]] bool setUserDecision(int row, std::optional<RoughCutDecision> decision);
    void replaceBaseDecisions(QVector<RoughCutSegmentDecision> decisions, bool preserveUser = true);
    void applyAuxiliaryResult(RoughCutAuxiliaryResult result, const QString &providerName);

private:
    void refreshProtectedDecisions();

    QVector<RecognizedPassage> recording_;
    QVector<RoughCutSegmentDecision> decisions_;
    QVector<RoughCutSegmentDecision> baseDecisions_;
    int sampleRate_ = 0;
    QString scriptText_;
};

} // namespace subcue
