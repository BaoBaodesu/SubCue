#include "subtitle/subtitle_model.h"

namespace subcue {

SubtitleModel::SubtitleModel(SubtitleDocument *document, QObject *parent)
    : QAbstractListModel(parent), document_(document)
{
    Q_ASSERT(document_);
    connect(document_, &SubtitleDocument::aboutToReset, this, [this] { beginResetModel(); });
    connect(document_, &SubtitleDocument::reset, this, [this] {
        endResetModel();
        emit countChanged();
    });
    connect(document_, &SubtitleDocument::aboutToInsert, this,
            [this](int index) { beginInsertRows(QModelIndex(), index, index); });
    connect(document_, &SubtitleDocument::inserted, this, [this] {
        endInsertRows();
        emit countChanged();
    });
    connect(document_, &SubtitleDocument::aboutToRemove, this,
            [this](int index) { beginRemoveRows(QModelIndex(), index, index); });
    connect(document_, &SubtitleDocument::removed, this, [this] {
        endRemoveRows();
        emit countChanged();
    });
    connect(document_, &SubtitleDocument::changed, this, [this](int index) {
        emit dataChanged(this->index(index), this->index(index));
    });
}

int SubtitleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : document_->count();
}

QVariant SubtitleModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= document_->count()) return {};
    const Subtitle &subtitle = document_->subtitles().at(index.row());
    switch (role) {
    case IdRole: return subtitle.id;
    case SourceIndexRole: return index.row() + 1;
    case StartUsRole: return subtitle.start.microseconds();
    case EndUsRole: return subtitle.end.microseconds();
    case StartMsRole: return subtitle.start.milliseconds();
    case EndMsRole: return subtitle.end.milliseconds();
    case DurationMsRole: return (subtitle.end.microseconds() - subtitle.start.microseconds()) / 1000;
    case TextRole: return subtitle.text;
    case TrackRole: return subtitle.track;
    case ConfidenceRole: return subtitle.confidence;
    case SourceRole: return subtitle.source;
    case StatusRole: return subtitle.status;
    case TimedRole: return subtitle.isTimed();
    case SkipReasonRole: return subtitle.skipReason;
    case StartWordIdRole: return subtitle.startWordId;
    case EndWordIdRole: return subtitle.endWordId;
    case CandidateTextRole: return subtitle.candidateText;
    case MetadataRole: return subtitle.metadata.toVariantMap();
    default: return {};
    }
}

QHash<int, QByteArray> SubtitleModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {SourceIndexRole, "sourceIndex"},
        {StartUsRole, "startUs"},
        {EndUsRole, "endUs"},
        {StartMsRole, "startMs"},
        {EndMsRole, "endMs"},
        {DurationMsRole, "durationMs"},
        {TextRole, "text"},
        {TrackRole, "track"},
        {ConfidenceRole, "confidence"},
        {SourceRole, "source"},
        {StatusRole, "status"},
        {TimedRole, "timed"},
        {SkipReasonRole, "skipReason"},
        {StartWordIdRole, "startWordId"},
        {EndWordIdRole, "endWordId"},
        {MetadataRole, "metadata"},
        {CandidateTextRole, "candidateText"},
    };
}

QVariantMap SubtitleModel::get(int row) const
{
    if (row < 0 || row >= rowCount()) return {};
    QVariantMap result;
    const QModelIndex modelIndex = index(row);
    const auto roles = roleNames();
    for (auto iterator = roles.cbegin(); iterator != roles.cend(); ++iterator) {
        result.insert(QString::fromUtf8(iterator.value()), data(modelIndex, iterator.key()));
    }
    return result;
}


SubtitleFilterModel::SubtitleFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::modelReset, this, &SubtitleFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsInserted, this, &SubtitleFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &SubtitleFilterModel::countChanged);
}

void SubtitleFilterModel::setSearchText(const QString &text)
{
    if (searchText_ == text) return;
    searchText_ = text;
    invalidateRowsFilter();
    emit filterChanged();
}

void SubtitleFilterModel::setStatusFilter(const QString &status)
{
    if (statusFilter_ == status) return;
    statusFilter_ = status;
    invalidateRowsFilter();
    emit filterChanged();
}

void SubtitleFilterModel::setOverlappingIds(const QSet<QString> &ids)
{
    if (overlappingIds_ == ids) return;
    overlappingIds_ = ids;
    if (statusFilter_ == QStringLiteral("OVERLAP")) invalidateRowsFilter();
}

bool SubtitleFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &parent) const
{
    const QModelIndex row = sourceModel()->index(sourceRow, 0, parent);
    const QString status = row.data(SubtitleModel::StatusRole).toString();
    const bool timed = row.data(SubtitleModel::TimedRole).toBool();
    if (statusFilter_ == QStringLiteral("REVIEW") && (!timed ||
        (status != QStringLiteral("REVIEW") && status != QStringLiteral("LOW_CONFIDENCE")))) return false;
    if (statusFilter_ == QStringLiteral("UNTIMED") && timed) return false;
    if (statusFilter_ == QStringLiteral("OVERLAP") &&
        !overlappingIds_.contains(row.data(SubtitleModel::IdRole).toString())) return false;
    return searchText_.isEmpty() || row.data(SubtitleModel::TextRole).toString().contains(searchText_, Qt::CaseInsensitive);
}

QVariantMap SubtitleFilterModel::get(int row) const
{
    const QModelIndex source = mapToSource(index(row, 0));
    const auto *model = qobject_cast<const SubtitleModel *>(sourceModel());
    return source.isValid() && model ? model->get(source.row()) : QVariantMap{};
}

int SubtitleFilterModel::rowForId(const QString &id) const
{
    for (int row = 0; row < rowCount(); ++row)
        if (index(row, 0).data(SubtitleModel::IdRole).toString() == id) return row;
    return -1;
}

} // namespace subcue
