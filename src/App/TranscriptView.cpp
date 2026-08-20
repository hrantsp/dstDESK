#include <QFrame>
#include <QHBoxLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStackedLayout>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include "TranscriptView.hpp"

namespace DST { namespace DESK { namespace App {
namespace {

// One accent per stream, so who is speaking is legible before any label is read.
QColor accentOf(Core::Stream::Value stream)
{
  switch (stream) { case Core::Stream::Mic : return QColor(0x2f, 0x6f, 0x4e);
                    case Core::Stream::Tab : return QColor(0x3a, 0x5a, 0x8c);
                    default                : return QColor(0x66, 0x66, 0x66); }
}

QString clockOf(double seconds)
{
  const int total = int(seconds);
  return QStringLiteral("%1:%2")
      .arg(total / 60, 2, 10, QLatin1Char('0'))
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// The columns, in one place, so measurement and painting cannot disagree about where
// the text starts.
constexpr int kMarginLeft   = 12;
constexpr int kMarginRight  = 12;
constexpr int kMarginTop    = 5;
constexpr int kMarginBottom = 5;
constexpr int kTimeWidth    = 44;
constexpr int kNameWidth    = 80;
constexpr int kGap          = 8;

int textWidthFor(int viewportWidth)
{
  const int width = viewportWidth - kMarginLeft - kMarginRight
                                  - kTimeWidth - kGap - kNameWidth - kGap;
  return std::max(width, 40); // never zero, or the layout loop never terminates
}

// Every occurrence of the search text, as ranges the layout can paint a background
// behind. Ranges rather than markup: the text is arbitrary speech, and building HTML
// out of it means escaping it correctly every time or silently swallowing a line that
// happens to contain a "<".
QList<QTextLayout::FormatRange> matchRanges(const QString& text, const QString& needle)
{
  auto ranges = QList<QTextLayout::FormatRange>{};
  if (needle.isEmpty()) return ranges;

  auto highlight = QTextCharFormat{};
  highlight.setBackground(QColor(0xff, 0xe5, 0x8a));

  int at = 0;
  while (at < text.size())
  {
    const int hit = text.indexOf(needle, at, Qt::CaseInsensitive);
    if (hit < 0) break;

    ranges.append(QTextLayout::FormatRange{ hit, int(needle.size()), highlight });
    at = hit + needle.size();
  }
  return ranges;
}

/// Lays `text` out at `width` and returns its height. Draws it too when a painter is
/// given, so the height a row was measured at is by construction the height it is drawn
/// at — the two cannot drift apart, which they do the moment they are separate code.
qreal layOutText(const QString& text, const QFont& font, int width, const QString& search,
                 QPainter* painter, const QPointF& origin, const QColor& colour)
{
  auto layout = QTextLayout(text, font);
  layout.setFormats(matchRanges(text, search));

  auto options = QTextOption(Qt::AlignLeft);
  options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  layout.setTextOption(options);

  qreal height = 0;
  layout.beginLayout();
  while (true)
  {
    auto line = layout.createLine();
    if (!line.isValid()) break;

    line.setLineWidth(width);
    line.setPosition(QPointF(0, height));
    height += line.height();
  }
  layout.endLayout();

  if (painter != nullptr)
  {
    painter->save();
    painter->setPen(colour);
    layout.draw(painter, origin);
    painter->restore();
  }
  return height;
}

} // namespace

// ── delegate ─────────────────────────────────────────────────────────────────

void UtteranceDelegate::setViewportWidth(int width)
{
  if (width_ == width) return;
  width_ = width;
  heights_.clear(); // every stored height was measured against the old width
}

void UtteranceDelegate::setSearch(const QString& needle)
{
  if (search_ == needle) return;

  // Marking a match does not reflow the text, so heights survive. Only the painting
  // changes, and the view repaints what is on screen.
  search_ = needle;
}

QSize UtteranceDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
  const int viewport = width_ > 0 ? width_ : option.rect.width();
  const int row      = index.data(TranscriptModel::SourceRowRole).toInt();

  const auto cached = heights_.constFind(row);
  if (cached != heights_.constEnd()) return QSize(viewport, *cached);

  const auto text   = index.data(TranscriptModel::TextRole).toString();
  const auto height = layOutText(text, option.font, textWidthFor(viewport), QString(),
                                 nullptr, {}, {});

  const int total = std::max(int(std::ceil(height)), option.fontMetrics.height())
                    + kMarginTop + kMarginBottom;
  heights_.insert(row, total);
  return QSize(viewport, total);
}

void UtteranceDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const
{
  painter->save();

  if (option.state & QStyle::State_Selected)
    painter->fillRect(option.rect, option.palette.highlight().color().lighter(180));

  const auto stream     = Core::Stream::Value(index.data(TranscriptModel::StreamRole).toInt());
  const auto start      = index.data(TranscriptModel::StartRole).toDouble();
  const auto text       = index.data(TranscriptModel::TextRole).toString();
  const auto confidence = index.data(TranscriptModel::ConfidenceRole).toDouble();

  const int left = option.rect.left() + kMarginLeft;
  const int top  = option.rect.top()  + kMarginTop;

  auto timeFont = option.font;
  timeFont.setFamily(QStringLiteral("monospace"));
  painter->setFont(timeFont);
  painter->setPen(QColor(0x8a, 0x8a, 0x8a));
  painter->drawText(QRect(left, top, kTimeWidth, option.fontMetrics.height()),
                    Qt::AlignRight | Qt::AlignTop, clockOf(start));

  auto nameFont = option.font;
  nameFont.setBold(true);
  painter->setFont(nameFont);
  painter->setPen(accentOf(stream));
  painter->drawText(QRect(left + kTimeWidth + kGap, top, kNameWidth,
                          option.fontMetrics.height()),
                    Qt::AlignLeft | Qt::AlignTop,
                    QString::fromLatin1(Core::Stream::label(stream)));

  // Low-confidence text is dimmed rather than hidden: a hedged guess is more useful
  // than a gap, but presenting it as certain would be a lie.
  const auto colour = (confidence > 0.0 && confidence < 0.6)
                          ? QColor(0x8a, 0x8a, 0x8a)
                          : option.palette.text().color();

  painter->setFont(option.font);
  layOutText(text, option.font, textWidthFor(option.rect.width()), search_, painter,
             QPointF(left + kTimeWidth + kGap + kNameWidth + kGap, top), colour);

  painter->restore();
}

// ── list ─────────────────────────────────────────────────────────────────────

TranscriptList::TranscriptList(UtteranceDelegate* delegate, QWidget* parent)
  : QListView(parent)
  , delegate_(delegate)
{
  setItemDelegate(delegate);
  setFrameShape(QFrame::NoFrame);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setUniformItemSizes(false);
  setWordWrap(true);
  setResizeMode(QListView::Adjust);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void TranscriptList::resizeEvent(QResizeEvent* event)
{
  QListView::resizeEvent(event);

  // Only when the width actually moved: a height-only resize changes nothing about how
  // the text wraps, and discarding every measurement for it would make dragging the
  // bottom edge as expensive as dragging the side.
  if (event->oldSize().width() == event->size().width()) return;

  delegate_->setViewportWidth(viewport()->width());
  scheduleDelayedItemsLayout();
}

// ── view ─────────────────────────────────────────────────────────────────────

TranscriptView::TranscriptView(QWidget* parent)
  : QWidget(parent)
{
  model_    = new TranscriptModel(this);
  filter_   = new TranscriptFilter(this);
  delegate_ = new UtteranceDelegate(this);

  filter_->setSourceModel(model_);

  list_ = new TranscriptList(delegate_, this);
  list_->setModel(filter_);

  emptyText_ = tr("Waiting for the extension to connect…");

  placeholder_ = new QLabel(emptyText_);
  placeholder_->setAlignment(Qt::AlignCenter);
  placeholder_->setWordWrap(true);
  placeholder_->setStyleSheet(QStringLiteral("color:#9a9a9a; padding:32px;"));

  // Stacked over the list rather than inside it. A placeholder that is a row of the
  // model would be filtered, sorted and selectable like real text.
  auto* stack = new QWidget;
  auto* stackLayout = new QStackedLayout(stack);
  stackLayout->setStackingMode(QStackedLayout::StackAll);
  stackLayout->setContentsMargins(0, 0, 0, 0);
  stackLayout->addWidget(placeholder_);
  stackLayout->addWidget(list_);

  connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value)
  {
    // Within a few pixels of the bottom still counts as following, so a stray wheel
    // notch does not silently stop the transcript from advancing.
    auto* bar = list_->verticalScrollBar();
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
  layout->addWidget(stack, 1);
  layout->addWidget(live, 0);

  // The counts line follows the models rather than being recomputed by whoever last
  // changed something, so a filter, a search and an arriving utterance all report it
  // the same way.
  //
  // Both models, not only the filter. An utterance that the current filter hides raises
  // the total and not the shown count, and the filter says nothing about it — so
  // listening to the filter alone leaves "12 of 40" reading 12 of 40 while the fortieth
  // line is already the sixtieth.
  for (auto* source : { static_cast<QAbstractItemModel*>(model_),
                        static_cast<QAbstractItemModel*>(filter_) })
  {
    connect(source, &QAbstractItemModel::rowsInserted,  this, &TranscriptView::updateCounts);
    connect(source, &QAbstractItemModel::rowsRemoved,   this, &TranscriptView::updateCounts);
    connect(source, &QAbstractItemModel::modelReset,    this, &TranscriptView::updateCounts);
    connect(source, &QAbstractItemModel::layoutChanged, this, &TranscriptView::updateCounts);
  }

  updateCounts(); // the empty state is a state, and the placeholder belongs to it
}

QWidget* TranscriptView::buildLiveRow(Core::Stream::Value stream)
{
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* name = new QLabel(QString::fromLatin1(Core::Stream::label(stream)));
  name->setFixedWidth(80);
  name->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(accentOf(stream).name()));

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
  // One row in, one row laid out. Nothing here touches the utterances already shown,
  // which is the whole difference from the widget-per-line version this replaced.
  model_->append(utterance);

  const bool shown = !streamFilter_.has_value() || *streamFilter_ == utterance.stream;
  if (shown && following_) scrollToEnd();
}

void TranscriptView::updateCounts()
{
  const int shown = filter_->rowCount();
  const int total = model_->rowCount();

  if (total == 0)
  {
    placeholder_->setText(emptyText_);
  }
  else if (shown == 0)
  {
    // A filter that hides everything looks identical to a broken app unless it says so.
    placeholder_->setText(tr("Nothing matches the current filter."));
  }

  placeholder_->setVisible(shown == 0);
  list_->setVisible(shown != 0);

  emit countsChanged(shown, total);
}

void TranscriptView::setStreamFilter(std::optional<Core::Stream::Value> stream)
{
  streamFilter_ = stream;
  filter_->setStreamFilter(stream);
  updateCounts();
}

void TranscriptView::setSearch(const QString& needle)
{
  delegate_->setSearch(needle.trimmed());
  filter_->setSearch(needle);
  list_->viewport()->update(); // the marks moved; the heights did not
  updateCounts();
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
  model_->clear();

  for (auto* label : liveText_) if (label != nullptr) label->clear();
  for (auto* row : liveRow_) if (row != nullptr) row->setVisible(false);

  following_ = true;
  updateCounts();
}

void TranscriptView::setPlaceholder(const QString& text)
{
  emptyText_ = text;
  if (model_->rowCount() == 0) placeholder_->setText(text);
}

void TranscriptView::scrollToEnd()
{
  // Queued: the new row has no geometry until the view has laid it out, so scrolling
  // now would stop short of the text that was just added.
  QTimer::singleShot(0, this, [this]
  {
    auto* bar = list_->verticalScrollBar();
    bar->setValue(bar->maximum());
  });
}

} } } // namespace DST::DESK::App
