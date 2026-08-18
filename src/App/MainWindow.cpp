#include <QHBoxLayout>
#include <QKeySequence>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWidget>
#include "MainWindow.hpp"
#include "SettingsDialog.hpp"

namespace DST { namespace DESK { namespace App {

MainWindow::MainWindow(IO::WsServer& server, const Settings& settings, bool transcribing,
                       const QString& keyHint, QWidget* parent)
  : QMainWindow(parent)
  , server_(server)
  , settings_(settings)
  , transcribing_(transcribing)
  , keyHint_(keyHint)
{
  setWindowTitle(tr("Kobayashi"));
  resize(820, 600);

  transcript_ = new TranscriptView;

  auto* central = new QWidget;
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(buildStatusBar(transcribing, keyHint), 0);
  layout->addWidget(buildControlBar(), 0);
  layout->addWidget(transcript_, 1);
  setCentralWidget(central);

  applyPlaceholder(false);

  connect(&server_, &IO::WsServer::utteranceCommitted, transcript_, &TranscriptView::append);
  connect(&server_, &IO::WsServer::interimChanged,     transcript_, &TranscriptView::setInterim);

  connect(transcript_, &TranscriptView::countsChanged, this, [this](int shown, int total)
  {
    counts_->setText(shown == total ? tr("%n line(s)", nullptr, total)
                                    : tr("%1 of %2").arg(shown).arg(total));
  });

  connect(&server_, &IO::WsServer::sessionStarted, this,
          [this](const QString& client, const QString& directory)
  {
    transcript_->clear();
    setStatus(tr("Capturing — %1").arg(client), QStringLiteral("#c0392b"));
    setWindowTitle(tr("Kobayashi — %1").arg(directory));
    applyPlaceholder(true);
  });

  connect(&server_, &IO::WsServer::notice, this, [this](const QString& text)
  {
    // Kept, not just flashed: a refusal happens before anyone is looking at the window,
    // and the placeholder is where they look afterwards. The status line gets the first
    // sentence only — it is one line, and the placeholder carries the whole of it.
    notice_ = text;
    const auto stop = text.indexOf(QStringLiteral(" — "));
    setStatus(stop > 0 ? text.left(stop) : text, QStringLiteral("#c0392b"));
    applyPlaceholder(false);
  });

  connect(&server_, &IO::WsServer::sessionEnded, this, [this]
  {
    // Deliberately leaves the transcript on screen: a session ending is exactly when
    // someone wants to read back what was said.
    setStatus(tr("Session ended"), QStringLiteral("#b0b0b0"));
  });

  auto* find = new QShortcut(QKeySequence::Find, this);
  connect(find, &QShortcut::activated, this, [this] { search_->setFocus(); search_->selectAll(); });
}

QWidget* MainWindow::buildStatusBar(bool transcribing, const QString& keyHint)
{
  dot_ = new QLabel(QStringLiteral("●"));
  dot_->setStyleSheet(QStringLiteral("color:#b0b0b0; font-size:15px;"));

  status_ = new QLabel(tr("Listening on port %1").arg(server_.port()));
  status_->setStyleSheet(QStringLiteral("font-weight:600;"));

  // Whether transcription is on stays visible for the whole session. It used to be
  // replaced by the output directory once capture began, which is precisely when a
  // user is asking why no text is appearing.
  detail_ = new QLabel(transcribing ? tr("Transcribing") : tr("Recording only — no API key"));
  detail_->setStyleSheet(transcribing ? QStringLiteral("color:#8a8a8a;")
                                      : QStringLiteral("color:#c0392b; font-weight:600;"));
  if (!transcribing) detail_->setToolTip(keyHint);

  auto* bar = new QWidget;
  auto* layout = new QHBoxLayout(bar);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  layout->addWidget(dot_);
  layout->addWidget(status_);
  layout->addStretch(1);
  layout->addWidget(detail_);
  // Scoped by object name. An unscoped rule cascades into every descendant, which
  // repaints things Qt styles for itself — a combo box popup inheriting a flat white
  // background loses its hover highlight and renders white on white.
  bar->setObjectName(QStringLiteral("statusBar"));
  bar->setStyleSheet(QStringLiteral(
      "QWidget#statusBar { background:#fafafa; border-bottom:1px solid #e0e0e0; }"));
  return bar;
}

QWidget* MainWindow::buildControlBar()
{
  filter_ = new QComboBox;
  filter_->addItem(tr("Both streams"));
  filter_->addItem(QString::fromLatin1(Core::Stream::label(Core::Stream::Mic)));
  filter_->addItem(QString::fromLatin1(Core::Stream::label(Core::Stream::Tab)));

  connect(filter_, &QComboBox::currentIndexChanged, this, [this](int index)
  {
    if (index == 1)      transcript_->setStreamFilter(Core::Stream::Mic);
    else if (index == 2) transcript_->setStreamFilter(Core::Stream::Tab);
    else                 transcript_->setStreamFilter(std::nullopt);
  });

  search_ = new QLineEdit;
  search_->setPlaceholderText(tr("Find in transcript…"));
  search_->setClearButtonEnabled(true);
  connect(search_, &QLineEdit::textChanged, transcript_, &TranscriptView::setSearch);

  counts_ = new QLabel(tr("0 lines"));
  counts_->setStyleSheet(QStringLiteral("color:#8a8a8a;"));

  auto* settings = new QPushButton(tr("Settings…"));
  connect(settings, &QPushButton::clicked, this, &MainWindow::openSettings);

  auto* bar = new QWidget;
  auto* layout = new QHBoxLayout(bar);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  layout->addWidget(filter_, 0);
  layout->addWidget(search_, 1);
  layout->addWidget(counts_, 0);
  layout->addWidget(settings, 0);
  bar->setObjectName(QStringLiteral("controlBar"));
  bar->setStyleSheet(QStringLiteral(
      "QWidget#controlBar { background:#ffffff; border-bottom:1px solid #ececec; }"));
  return bar;
}

void MainWindow::openSettings()
{
  auto dialog = SettingsDialog(settings_, server_.port(), this);
  if (dialog.exec() != QDialog::Accepted) return;

  settings_ = dialog.result();
  settings_.save();

  // Applied to the next session rather than the running one: the recorders and the
  // transcription connections were built from the values that were current when the
  // session opened, and swapping them underneath would give a session two identities.
  auto cfg = IO::ServerConfig{};
  cfg.port           = server_.port();
  cfg.outputDir      = settings_.outputDir;
  cfg.token          = settings_.token;
  cfg.stt.apiKey     = settings_.apiKey;
  cfg.stt.model      = settings_.model;
  cfg.stt.diarize    = settings_.diarize;
  cfg.transcribe     = !settings_.apiKey.isEmpty();
  server_.updateConfig(cfg);

  transcribing_ = cfg.transcribe;
  detail_->setText(transcribing_ ? tr("Transcribing") : tr("Recording only — no API key"));
  detail_->setStyleSheet(transcribing_ ? QStringLiteral("color:#8a8a8a;")
                                       : QStringLiteral("color:#c0392b; font-weight:600;"));
  applyPlaceholder(false);
}

void MainWindow::applyPlaceholder(bool connected)
{
  if (transcribing_)
  {
    transcript_->setPlaceholder(connected ? tr("Connected. Nothing has been said yet.")
                                          : waitingText());
    return;
  }

  transcript_->setPlaceholder(
      connected ? tr("Connected, and recording audio — but not transcribing.\n\n%1").arg(keyHint_)
                : tr("%1\n\n%2").arg(waitingText(), keyHint_));
}

QString MainWindow::waitingText() const
{
  // Names the port, because the commonest reason nothing connects is that the two halves
  // are looking at different ones — and the number is otherwise only in a console line
  // that a double-clicked application never shows.
  const auto waiting = tr("Waiting for Verbal on port %1…").arg(server_.port());
  return notice_.isEmpty() ? waiting : tr("%1\n\n%2").arg(notice_, waiting);
}

void MainWindow::setStatus(const QString& text, const QString& colour)
{
  // Elided, because a QLabel's size hint is the whole of its text and nothing in a
  // horizontal bar constrains it: one long status — a refused origin naming a flag, or a
  // notice carrying an engine's error string — otherwise widens the window to fit the
  // line, and the user has to drag it back. The full text stays in the tooltip, and for
  // notices it is in the placeholder too, which does wrap.
  constexpr auto kStatusWidth = 520;
  const auto elided = status_->fontMetrics().elidedText(text, Qt::ElideRight, kStatusWidth);
  status_->setText(elided);
  status_->setToolTip(elided == text ? QString() : text);
  dot_->setStyleSheet(QStringLiteral("color:%1; font-size:15px;").arg(colour));
}

} } } // namespace DST::DESK::App
