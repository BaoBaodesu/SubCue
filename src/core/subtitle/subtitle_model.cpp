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

} // namespace subcue
