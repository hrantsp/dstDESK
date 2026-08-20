#include "TranscriptModel.hpp"

namespace DST { namespace DESK { namespace App {

int TranscriptModel::rowCount(const QModelIndex& parent) const
{
  // A list model has rows only at the root; anything else must report none, or a view
  // will descend into children that do not exist.
  return parent.isValid() ? 0 : int(rows_.size());
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid() || index.row() < 0 || index.row() >= int(rows_.size())) return {};

  const auto& utterance = rows_[std::size_t(index.row())];

  switch (role)
  {
    case Qt::DisplayRole:
    case TextRole:       return QString::fromStdString(utterance.text);
    case StreamRole:     return int(utterance.stream);
    case StartRole:      return utterance.start;
    case ConfidenceRole: return utterance.confidence;
    case SourceRowRole:  return index.row();
    default:             return {};
  }
}

void TranscriptModel::append(const Core::Utterance& utterance)
{
  // The whole point of the rewrite: adding a row tells the view that one row appeared,
  // rather than handing it a rebuilt history to lay out again.
  const int at = int(rows_.size());
  beginInsertRows({}, at, at);
  rows_.push_back(utterance);
  endInsertRows();
}

void TranscriptModel::clear()
{
  if (rows_.empty()) return;

  beginResetModel();
  rows_.clear();
  endResetModel();
}

void TranscriptFilter::setStreamFilter(std::optional<Core::Stream::Value> stream)
{
  if (stream_ == stream) return;
  stream_ = stream;
  invalidateRowsFilter();
}

void TranscriptFilter::setSearch(const QString& needle)
{
  const auto trimmed = needle.trimmed();
  if (search_ == trimmed) return;
  search_ = trimmed;
  invalidateRowsFilter();
}

bool TranscriptFilter::filterAcceptsRow(int row, const QModelIndex& parent) const
{
  const auto index = sourceModel()->index(row, 0, parent);

  if (stream_.has_value() &&
      index.data(TranscriptModel::StreamRole).toInt() != int(*stream_))
    return false;

  if (!search_.isEmpty() &&
      !index.data(TranscriptModel::TextRole).toString().contains(search_, Qt::CaseInsensitive))
    return false;

  return true;
}

} } } // namespace DST::DESK::App
