#include <QFrame>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include "TranscriptView.hpp"

namespace DST { namespace DESK { namespace App {
namespace {

// One accent per stream, so who is speaking is legible before any label is read.
const char* accentOf(Core::Stream::Value stream)
{
  switch (stream) { case Core::Stream::Mic : return "#2f6f4e";
                    case Core::Stream::Tab : return "#3a5a8c";
                    default                : return "#666666"; }
}

QString clockOf(double seconds)
{
  const int total = int(seconds);
  return QStringLiteral("%1:%2")
      .arg(total / 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

QLabel* makeTimeLabel(const QString& text)
{
  auto* label = new QLabel(text);
  label->setStyleSheet(QStringLiteral("color:#8a8a8a; font-family:monospace;"));
  label->setAlignment(Qt::AlignRight | Qt::AlignTop);
  label->setFixedWidth(44);
  return label;
}

// Marks every occurrence of the search text. Escaped first, because a transcript is
// arbitrary speech and an unescaped "<" would silently swallow the rest of the line.
QString highlighted(const QString& plain, const QString& needle)
{
  if (needle.isEmpty()) return plain.toHtmlEscaped();

  QString out;
  int at = 0;

  while (at < plain.size())
  {
    const int hit = plain.indexOf(needle, at, Qt::CaseInsensitive);
    if (hit < 0)
    {
      out += plain.mid(at).toHtmlEscaped();
      break;
    }
    out += plain.mid(at, hit - at).toHtmlEscaped();
    out += QStringLiteral("<span style=\"background:#ffe58a;\">%1</span>")
               .arg(plain.mid(hit, needle.size()).toHtmlEscaped());
    at = hit + needle.size();
  }
  return out;
}

} // namespace

TranscriptView::TranscriptView(QWidget* parent)
  : QWidget(parent)
{
  committed_ = new QWidget;
  rows_      = new QVBoxLayout(committed_);
  rows_->setContentsMargins(12, 12, 12, 12);
  rows_->setSpacing(10);
  rows_->addStretch(1);

  emptyText_ = tr("Waiting for the extension to connect…");

  placeholder_ = new QLabel(emptyText_);
  placeholder_->setAlignment(Qt::AlignCenter);
  placeholder_->setWordWrap(true);
  placeholder_->setStyleSheet(QStringLiteral("color:#9a9a9a; padding:32px;"));
  rows_->insertWidget(0, placeholder_);

  scroll_ = new QScrollArea;
  scroll_->setWidget(committed_);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);

  connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value)
  {
    // Within a few pixels of the bottom still counts as following, so a stray wheel
    // notch does not silently stop the transcript from advancing.
    auto* bar = scroll_->verticalScrollBar();
    following_ = value >= bar->maximum() - 4;
  });

  auto* live = new QWidget;
  auto* liveLayout = new QVBoxLayout(live);
  liveLayout->setContentsMargins(12, 8, 12, 10);
  liveLayout->setSpacing(4);
  liveLayout->addWidget(buildLiveRow(Core::Stream::Mic));
  liveLayout->addWidget(buildLiveRow(Core::Stream::Tab));
  // Scoped, so the rule cannot leak into the labels inside it.
  live->setObjectName(QStringLiteral("liveArea"));
  live->setStyleSheet(QStringLiteral(
      "QWidget#liveArea { background:#f6f6f6; border-top:1px solid #e0e0e0; }"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(scroll_, 1);
  layout->addWidget(live, 0);
}

QWidget* TranscriptView::buildLiveRow(Core::Stream::Value stream)
{
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* name = new QLabel(QString::fromLatin1(Core::Stream::label(stream)));
  name->setFixedWidth(80);
  name->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(QLatin1String(accentOf(stream))));

  auto* text = new QLabel;
  text->setWordWrap(true);
  // Italic and grey: this text is a guess and may still change.
  text->setStyleSheet(QStringLiteral("color:#7a7a7a; font-style:italic;"));

  layout->addWidget(name, 0, Qt::AlignTop);
  layout->addWidget(text, 1);

  liveText_[std::size_t(stream)] = text;
  liveRow_[std::size_t(stream)]  = row;
  row->setVisible(false);

  return row;
}

void TranscriptView::append(const Core::Utterance& utterance)
{
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* name = new QLabel(QString::fromLatin1(Core::Stream::label(utterance.stream)));
  name->setFixedWidth(80);
  name->setStyleSheet(QStringLiteral("color:%1; font-weight:600;")
                          .arg(QLatin1String(accentOf(utterance.stream))));

  auto* text = new QLabel;
  text->setWordWrap(true);
  text->setTextInteractionFlags(Qt::TextSelectableByMouse);

  // Low-confidence text is dimmed rather than hidden: a hedged guess is more useful
  // than a gap, but presenting it as certain would be a lie.
  if (utterance.confidence > 0.0 && utterance.confidence < 0.6)
    text->setStyleSheet(QStringLiteral("color:#8a8a8a;"));

  layout->addWidget(makeTimeLabel(clockOf(utterance.start)), 0, Qt::AlignTop);
  layout->addWidget(name, 0, Qt::AlignTop);
  layout->addWidget(text, 1);

  rows_->insertWidget(rows_->count() - 1, row);

  const auto entry = Row{ row, text, utterance.stream, QString::fromStdString(utterance.text) };
  entries_.push_back(entry);
  renderText(entry);

  const bool shown = matches(entry);
  row->setVisible(shown);
  placeholder_->setVisible(false);

  applyFilters();
  if (shown && following_) scrollToEnd();
}

bool TranscriptView::matches(const Row& row) const
{
  if (streamFilter_.has_value() && row.stream != *streamFilter_) return false;
  if (!search_.isEmpty() && !row.plain.contains(search_, Qt::CaseInsensitive)) return false;
  return true;
}

void TranscriptView::renderText(const Row& row) const
{
  // Rich text only while searching: a plain label is cheaper and cannot be tripped up
  // by punctuation in speech.
  if (search_.isEmpty()) row.text->setText(row.plain);
  else                   row.text->setText(highlighted(row.plain, search_));
}

void TranscriptView::applyFilters()
{
  int shown = 0;

  for (const auto& entry : entries_)
  {
    const bool visible = matches(entry);
    entry.widget->setVisible(visible);
    renderText(entry);
    if (visible) ++shown;
  }

  const int total = int(entries_.size());

  if (total == 0)
  {
    placeholder_->setText(emptyText_);
    placeholder_->setVisible(true);
  }
  else if (shown == 0)
  {
    // A filter that hides everything looks identical to a broken app unless it says so.
    placeholder_->setText(tr("Nothing matches the current filter."));
    placeholder_->setVisible(true);
  }
  else
  {
    placeholder_->setVisible(false);
  }

  emit countsChanged(shown, total);
}

void TranscriptView::setStreamFilter(std::optional<Core::Stream::Value> stream)
{
  streamFilter_ = stream;
  applyFilters();
}

void TranscriptView::setSearch(const QString& needle)
{
  search_ = needle.trimmed();
  applyFilters();
}

void TranscriptView::setInterim(Core::Stream::Value stream, const QString& text)
{
  const auto slot = std::size_t(stream);
  if (liveText_[slot] == nullptr) return;

  // Interim text follows the stream filter too, so filtering to one side does not
  // leave the other still murmuring at the bottom of the window.
  const bool allowed = !streamFilter_.has_value() || *streamFilter_ == stream;

  liveText_[slot]->setText(text);
  liveRow_[slot]->setVisible(allowed && !text.isEmpty());
}

void TranscriptView::clear()
{
  for (const auto& entry : entries_)
  {
    rows_->removeWidget(entry.widget);
    entry.widget->deleteLater();
  }
  entries_.clear();

  placeholder_->setText(emptyText_);
  placeholder_->setVisible(true);

  for (auto* label : liveText_) if (label != nullptr) label->clear();
  for (auto* row : liveRow_) if (row != nullptr) row->setVisible(false);

  following_ = true;
  emit countsChanged(0, 0);
}

void TranscriptView::setPlaceholder(const QString& text)
{
  emptyText_ = text;
  if (entries_.empty()) placeholder_->setText(text);
}

void TranscriptView::scrollToEnd()
{
  // Queued: the new row has no geometry until the layout has run, so scrolling now
  // would stop short of the text that was just added.
  QTimer::singleShot(0, this, [this]
  {
    auto* bar = scroll_->verticalScrollBar();
    bar->setValue(bar->maximum());
  });
}

} } } // namespace DST::DESK::App
