#pragma once

#include "roughcut/decision_engine.h"
#include "roughcut/script_matcher.h"
#include "roughcut/auxiliary_recognition.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QSortFilterProxyModel>

namespace subcue {

class RoughCutResultModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int keepCount READ keepCount NOTIFY countsChanged)
    Q_PROPERTY(int reviewCount READ reviewCount NOTIFY countsChanged)
    Q_PROPERTY(int cutCount READ cutCount NOTIFY countsChanged)

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
        ReplacementRole,
        RecordingIndexRole,
        StatusLabelRole,
        ShortReasonRole,
        FailureLabelRole,
        EvidenceTextRole,
        TechnicalDetailsRole
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
    void applyAuxiliaryResults(const QVector<RoughCutAuxiliaryResult> &results,
                               const QString &providerName);

    [[nodiscard]] int keepCount() const noexcept { return keepCount_; }
    [[nodiscard]] int reviewCount() const noexcept { return reviewCount_; }
    [[nodiscard]] int cutCount() const noexcept { return cutCount_; }
    [[nodiscard]] int nextReviewRow(int fromSourceRow) const;
    [[nodiscard]] int previousReviewRow(int fromSourceRow) const;

signals:
    void countsChanged();

private:
    void refreshProtectedDecisions();
    void recount();

    QVector<RecognizedPassage> recording_;
    QVector<RoughCutSegmentDecision> decisions_;
    QVector<RoughCutSegmentDecision> baseDecisions_;
    int sampleRate_ = 0;
    QString scriptText_;
    int keepCount_ = 0;
    int reviewCount_ = 0;
    int cutCount_ = 0;
};

class RoughCutResultFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY statusFilterChanged)

public:
    explicit RoughCutResultFilterModel(QObject *parent = nullptr);
    [[nodiscard]] QString statusFilter() const { return statusFilter_; }
    void setStatusFilter(const QString &value);

signals:
    void statusFilterChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString statusFilter_ = QStringLiteral("ALL");
};

} // namespace subcue
