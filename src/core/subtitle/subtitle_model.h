#pragma once

#include "subtitle/subtitle_document.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QSet>

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


class SubtitleFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY filterChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY filterChanged)
public:
    explicit SubtitleFilterModel(QObject *parent = nullptr);
    QString searchText() const { return searchText_; }
    QString statusFilter() const { return statusFilter_; }
    void setSearchText(const QString &text);
    void setStatusFilter(const QString &status);
    void setOverlappingIds(const QSet<QString> &ids);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowForId(const QString &id) const;
signals:
    void countChanged();
    void filterChanged();
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
private:
    QString searchText_;
    QString statusFilter_ = QStringLiteral("ALL");
    QSet<QString> overlappingIds_;
};

} // namespace subcue
