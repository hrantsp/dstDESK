// The application window: a status strip, a control strip, and the transcript.
//
// It owns no pipeline logic. Everything it shows arrives as a signal from the server,
// so the pipeline behaves identically whether or not a window exists — which is what
// keeps the headless path honest rather than a second implementation.

#ifndef DST_DESK_APP_MAINWINDOW_HPP
#define DST_DESK_APP_MAINWINDOW_HPP

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include "IO/WsServer.hpp"
#include "Settings.hpp"
#include "TranscriptView.hpp"

namespace DST { namespace DESK { namespace App {

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  MainWindow(IO::WsServer& server, const Settings& settings, bool transcribing,
             const QString& keyHint, QWidget* parent = nullptr);

private:
  QWidget* buildStatusBar(bool transcribing, const QString& keyHint);
  QWidget* buildControlBar();
  void     openSettings();
  void     setStatus(const QString& text, const QString& colour);
  void     applyPlaceholder(bool connected);
  QString  waitingText() const;

  IO::WsServer&   server_;
  Settings        settings_;
  TranscriptView* transcript_ = nullptr;
  QString         notice_;

  QLabel*    dot_     = nullptr;
  QLabel*    status_  = nullptr;
  QLabel*    detail_  = nullptr;
  QLabel*    counts_  = nullptr;
  QComboBox* filter_  = nullptr;
  QLineEdit* search_  = nullptr;

  bool    transcribing_ = false;
  QString keyHint_;
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_MAINWINDOW_HPP
