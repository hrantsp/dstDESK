// The transcript, in two zones.
//
// Committed text sits above and never changes once written; live text sits below and
// rewrites freely. The split is not decoration — the engine revises interim results,
// sometimes shortening them or changing their opening words, so anything shown before
// it is final has to be visibly provisional or the transcript appears to contradict
// itself. See decision 13 in dstOMNI/DESIGN.md.
//
// The committed zone is a model and a view, not a widget per line. The reason is
// measured rather than assumed, and is recorded in decision 24: one QWidget and three
// QLabels per utterance in a QVBoxLayout made every append re-solve heightForWidth over
// the whole history, so the cost of a line grew with the number before it, and clearing
// a search over a long meeting froze the window for seconds. A view renders the rows
// that are on screen and nothing else.

#ifndef DST_DESK_APP_TRANSCRIPTVIEW_HPP
#define DST_DESK_APP_TRANSCRIPTVIEW_HPP

#include <QHash>
#include <QLabel>
#include <QListView>
#include <QStyledItemDelegate>
#include <QWidget>
#include <array>
#include <optional>
#include "Core/Transcript.hpp"
#include "TranscriptModel.hpp"

namespace DST { namespace DESK { namespace App {

/// Draws one utterance: time, speaker, and word-wrapped text with search matches
/// marked. Layout and measurement share one code path, so the height a row is given is
/// the height it is drawn at.
class UtteranceDelegate : public QStyledItemDelegate
{
public:
  explicit UtteranceDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

  /// Text wraps to the viewport, so every stored height is only valid for one width.
  void setViewportWidth(int width);
  void setSearch(const QString& needle);

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void  paint(QPainter* painter, const QStyleOptionViewItem& option,
              const QModelIndex& index) const override;

private:
  // Laying text out is the expensive part and the answer never changes for a given row
  // and width, so it is done once. Keyed on the row in the source model, which survives
  // whatever filtering sits in between.
  mutable QHash<int, int> heights_;

  int     width_ = 0;
  QString search_;
};

/// A QListView that keeps the delegate informed of its width. Without this the delegate
/// measures against a stale width after every resize, and rows are given heights that
/// do not match the text drawn into them.
class TranscriptList : public QListView
{
  Q_OBJECT

public:
  TranscriptList(UtteranceDelegate* delegate, QWidget* parent = nullptr);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  UtteranceDelegate* delegate_ = nullptr;
};

class TranscriptView : public QWidget
{
  Q_OBJECT

public:
  explicit TranscriptView(QWidget* parent = nullptr);

  void append(const Core::Utterance& utterance);
  void setInterim(Core::Stream::Value stream, const QString& text);
  void clear();

  /// Shown while nothing is visible. Explaining why is more useful than a fixed line
  /// that becomes untrue on connection — or after a filter hides everything.
  void setPlaceholder(const QString& text);

  /// Nothing means every stream.
  void setStreamFilter(std::optional<Core::Stream::Value> stream);
  void setSearch(const QString& needle);

signals:
  void countsChanged(int shown, int total);

private:
  QWidget* buildLiveRow(Core::Stream::Value stream);
  void     updateCounts();
  void     scrollToEnd();

  TranscriptModel*   model_    = nullptr;
  TranscriptFilter*  filter_   = nullptr;
  UtteranceDelegate* delegate_ = nullptr;
  TranscriptList*    list_     = nullptr;

  QLabel* placeholder_ = nullptr;
  QString emptyText_;

  std::array<QLabel*, 2>  liveText_ = { nullptr, nullptr };
  std::array<QWidget*, 2> liveRow_  = { nullptr, nullptr };

  std::optional<Core::Stream::Value> streamFilter_;

  // True while the view is scrolled to the bottom. Following new text is right until
  // the reader scrolls up to re-read something, at which point yanking them back down
  // on every arriving utterance makes the transcript unusable.
  bool following_ = true;
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_TRANSCRIPTVIEW_HPP
