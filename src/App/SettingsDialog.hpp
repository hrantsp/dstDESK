// Settings, reachable without a terminal.
//
// The dialog is honest about when a change takes effect: the listening port is bound
// at startup and cannot move under a live connection, while everything else is read
// when the next session begins. Saying so is better than appearing to accept a change
// that quietly does nothing.

#ifndef DST_DESK_APP_SETTINGSDIALOG_HPP
#define DST_DESK_APP_SETTINGSDIALOG_HPP

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include "Settings.hpp"

namespace DST { namespace DESK { namespace App {

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  SettingsDialog(const Settings& current, std::uint16_t boundPort, QWidget* parent = nullptr);

  Settings result() const;

private:
  QSpinBox*  port_      = nullptr;
  QLineEdit* token_     = nullptr;
  QLineEdit* outputDir_ = nullptr;
  QLineEdit* apiKey_    = nullptr;
  QLineEdit* model_     = nullptr;
  QCheckBox* diarize_   = nullptr;
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_SETTINGSDIALOG_HPP
