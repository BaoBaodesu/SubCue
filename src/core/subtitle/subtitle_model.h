#pragma once

#include "subtitle/subtitle_document.h"

#include <QtCore/QAbstractListModel>

namespace subcue {

class SubtitleModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        SourceIndexRole,
        StartUsRole,
        EndUsRole,
        StartMsRole,
        EndMsRole,
        DurationMsRole,
        TextRole,
        TrackRole,
        ConfidenceRole,
        SourceRole,
        StatusRole,
        TimedRole,
        SkipReasonRole,
        StartWordIdRole,
        EndWordIdRole,
        MetadataRole,
        CandidateTextRole,
    };
    Q_ENUM(Role)

    explicit SubtitleModel(SubtitleDocument *document, QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    SubtitleDocument *document_;
};

} // namespace subcue
