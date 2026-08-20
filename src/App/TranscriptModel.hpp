// The committed transcript, as data rather than as widgets.
//
// This exists because the previous view held the transcript in its own widgets: one
// QWidget, one layout and three QLabels per utterance, in a QVBoxLayout. Every append
// re-laid out every row, and word-wrapped labels make that layout solve heightForWidth
// over the whole history — so the cost of adding line n grew with n, and any operation
// that made hidden rows visible again paid for all of them at once. Clearing a search
// over an hour-long meeting took seconds.
//
// A model over a plain vector fixes it at the root: the view renders the rows on screen
// and nothing else, filtering is a proxy rather than a walk over widgets, and the stored
// type stays Core::Utterance — the same Qt-free struct the merger produces, not a copy
// of it reshaped for display.

#ifndef DST_DESK_APP_TRANSCRIPTMODEL_HPP
#define DST_DESK_APP_TRANSCRIPTMODEL_HPP

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <vector>
#include "Core/Transcript.hpp"

namespace DST { namespace DESK { namespace App {

class TranscriptModel : public QAbstractListModel
{
  Q_OBJECT

public:
  enum Role
  {
    TextRole = Qt::UserRole + 1,
    StreamRole,
    StartRole,
    ConfidenceRole,

    // The row this utterance occupies in *this* model, so a delegate can key a cache on
    // it without knowing which proxies sit in between.
    SourceRowRole,
  };

  explicit TranscriptModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

  int      rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;

  void append(const Core::Utterance& utterance);
  void clear();

private:
  std::vector<Core::Utterance> rows_;
};

/// Stream filter and free-text search, applied without touching a widget.
class TranscriptFilter : public QSortFilterProxyModel
{
  Q_OBJECT

public:
  explicit TranscriptFilter(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

  /// Nothing means every stream.
  void setStreamFilter(std::optional<Core::Stream::Value> stream);
  void setSearch(const QString& needle);

  const QString& search() const { return search_; }

protected:
  bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private:
  std::optional<Core::Stream::Value> stream_;
  QString                            search_;
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_TRANSCRIPTMODEL_HPP
