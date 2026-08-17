#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include "SettingsDialog.hpp"

namespace DST { namespace DESK { namespace App {

SettingsDialog::SettingsDialog(const Settings& current, std::uint16_t boundPort, QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Settings"));
  setMinimumWidth(460);

  port_ = new QSpinBox;
  port_->setRange(1, 65535);
  port_->setValue(current.port);

  token_ = new QLineEdit(current.token);
  token_->setPlaceholderText(tr("none"));

  outputDir_ = new QLineEdit(current.outputDir);
  auto* browse = new QPushButton(tr("Browse…"));
  connect(browse, &QPushButton::clicked, this, [this]
  {
    const auto chosen = QFileDialog::getExistingDirectory(this, tr("Recordings folder"),
                                                          outputDir_->text());
    if (!chosen.isEmpty()) outputDir_->setText(chosen);
  });

  auto* outputRow = new QWidget;
  auto* outputLayout = new QHBoxLayout(outputRow);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  outputLayout->addWidget(outputDir_, 1);
  outputLayout->addWidget(browse, 0);

  apiKey_ = new QLineEdit(current.apiKey);
  apiKey_->setEchoMode(QLineEdit::Password);
  apiKey_->setPlaceholderText(tr("none — recording only"));

  model_ = new QLineEdit(current.model);
  diarize_ = new QCheckBox(tr("Label speakers within the meeting stream"));
  diarize_->setChecked(current.diarize);
  diarize_->setToolTip(tr("Separates distinct voices well and similar ones poorly, so it "
                          "refines the transcript rather than defining it."));

  auto* form = new QFormLayout;
  form->addRow(tr("Port"), port_);
  form->addRow(tr("Shared secret"), token_);
  form->addRow(tr("Recordings"), outputRow);
  form->addRow(tr("Deepgram key"), apiKey_);
  form->addRow(tr("Model"), model_);
  form->addRow(QString(), diarize_);

  auto* note = new QLabel;
  note->setWordWrap(true);
  note->setStyleSheet(QStringLiteral("color:#8a8a8a;"));

  // Two different kinds of "later", and conflating them would mislead.
  QString text = tr("Saved to %1").arg(Settings::path());
  if (port_->value() != boundPort)
    text += tr("\n\nThe port is bound at startup — restart to listen on a different one.");
  else
    text += tr("\n\nChanges apply to the next capture session.");

  if (Settings::keyFromEnvironment())
    text += tr("\n\nDEEPGRAM_API_KEY is set in the environment and takes precedence; the "
               "key typed here will not be saved.");

  note->setText(text);

  connect(port_, &QSpinBox::valueChanged, this, [this, note, boundPort](int value)
  {
    QString text = tr("Saved to %1").arg(Settings::path());
    text += value != boundPort
                ? tr("\n\nThe port is bound at startup — restart to listen on a different one.")
                : tr("\n\nChanges apply to the next capture session.");
    if (Settings::keyFromEnvironment())
      text += tr("\n\nDEEPGRAM_API_KEY is set in the environment and takes precedence; the "
                 "key typed here will not be saved.");
    note->setText(text);
  });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addSpacing(6);
  layout->addWidget(note);
  layout->addSpacing(6);
  layout->addWidget(buttons);
}

Settings SettingsDialog::result() const
{
  auto out = Settings{};
  out.port      = static_cast<std::uint16_t>(port_->value());
  out.token     = token_->text().trimmed();
  out.outputDir = outputDir_->text().trimmed();
  out.apiKey    = apiKey_->text().trimmed();
  // A blank field means "whatever the default is", not "no model". Saving the blank
  // writes a key that overrides the default on every later load, and the engine rejects
  // an empty model with a message about account permissions — nowhere near the cause.
  out.model     = model_->text().trimmed();
  if (out.model.isEmpty()) out.model = Settings::defaultModel();
  out.diarize   = diarize_->isChecked();
  return out;
}

} } } // namespace DST::DESK::App
