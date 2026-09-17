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
        ScoreRole
    };

    explicit RoughCutResultModel(QObject *parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void reset(QVector<RecognizedPassage> recording,
               QVector<RoughCutSegmentDecision> decisions, int sampleRate);
    [[nodiscard]] const QVector<RecognizedPassage> &recording() const noexcept { return recording_; }
    [[nodiscard]] const QVector<RoughCutSegmentDecision> &decisions() const noexcept { return decisions_; }
    [[nodiscard]] bool setUserDecision(int row, std::optional<RoughCutDecision> decision);
    void applyAuxiliaryResult(RoughCutAuxiliaryResult result, const QString &providerName);

private:
    QVector<RecognizedPassage> recording_;
    QVector<RoughCutSegmentDecision> decisions_;
    int sampleRate_ = 0;
};

} // namespace subcue
