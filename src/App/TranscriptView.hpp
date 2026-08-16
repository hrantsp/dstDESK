// The transcript, in two zones.
//
// Committed text sits above and never changes once written; live text sits below and
// rewrites freely. The split is not decoration — the engine revises interim results,
// sometimes shortening them or changing their opening words, so anything shown before
// it is final has to be visibly provisional or the transcript appears to contradict
// itself. See decision 13 in dstOMNI/DESIGN.md.

#ifndef DST_DESK_APP_TRANSCRIPTVIEW_HPP
#define DST_DESK_APP_TRANSCRIPTVIEW_HPP

#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <array>
#include <optional>
#include <vector>
#include "Core/Transcript.hpp"

namespace DST { namespace DESK { namespace App {

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
  struct Row
  {
    QWidget*            widget = nullptr;
    QLabel*             text   = nullptr;
    Core::Stream::Value stream = Core::Stream::Mic;
    QString             plain;
  };

  QWidget* buildLiveRow(Core::Stream::Value stream);
  void     applyFilters();
  bool     matches(const Row& row) const;
  void     renderText(const Row& row) const;
  void     scrollToEnd();

  QScrollArea* scroll_      = nullptr;
  QWidget*     committed_   = nullptr;
  QVBoxLayout* rows_        = nullptr;
  QLabel*      placeholder_ = nullptr;

  std::vector<Row> entries_;

  std::optional<Core::Stream::Value> streamFilter_;
  QString                            search_;
  QString                            emptyText_;

  std::array<QLabel*, 2>  liveText_ = { nullptr, nullptr };
  std::array<QWidget*, 2> liveRow_  = { nullptr, nullptr };

  // True while the view is scrolled to the bottom. Following new text is right until
  // the reader scrolls up to re-read something, at which point yanking them back down
  // on every arriving utterance makes the transcript unusable.
  bool following_ = true;
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_TRANSCRIPTVIEW_HPP
