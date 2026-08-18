#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <QApplication>
#include <QIcon>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <csignal>
#include <memory>
#include "Core/Protocol.hpp"
#include "IO/WsServer.hpp"
#include "MainWindow.hpp"
#include "Settings.hpp"
#include "SelfTest.hpp"

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <cstdio>
#endif

namespace {

#ifdef Q_OS_WIN
/// Reconnects standard output to the console that started this process, if there is one.
///
/// The application is built for the windows subsystem, so no console is created for it
/// and --headless and --selftest would otherwise print into nothing. Attaching to the
/// parent's console restores them; a double-click has no parent console, the call fails
/// harmlessly, and the launch stays clean. Which is the whole point of the subsystem
/// choice: the console appears when it was asked for, and never otherwise.
void attachParentConsole()
{
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

  auto* stream = static_cast<FILE*>(nullptr);
  freopen_s(&stream, "CONOUT$", "w", stdout);
  freopen_s(&stream, "CONOUT$", "w", stderr);
  freopen_s(&stream, "CONIN$",  "r", stdin);
}
#endif

volatile std::sig_atomic_t gInterrupted = 0;

extern "C" void onSignal(int) { gInterrupted = 1; }

// Ctrl-C otherwise terminates the process without unwinding the stack, so no
// destructor runs and the recordings are left with an unpatched RIFF header — real
// audio on disk that no player will open.
//
// A signal handler may do almost nothing safely, so it only sets a flag; a timer in
// the event loop notices and asks the application to quit, which unwinds normally.
void installSignalHandlers()
{
  std::signal(SIGINT , onSignal);
  std::signal(SIGTERM, onSignal);

  auto* poll = new QTimer(QCoreApplication::instance());
  QObject::connect(poll, &QTimer::timeout, []
  {
    if (gInterrupted == 0) return;
    QTextStream(stdout) << "\nInterrupted — closing recordings." << Qt::endl;
    QCoreApplication::quit();
  });
  poll->start(100);
}

// Scanned before the application object exists, because the choice of object depends
// on it: QApplication needs a display, and --selftest must stay runnable over ssh and
// on a build machine that has none.
bool wantsHeadless(int argc, char** argv)
{
  for (int ii = 1; ii < argc; ++ii)
  {
    const auto arg = QString::fromLocal8Bit(argv[ii]);
    if (arg == QStringLiteral("--selftest") || arg == QStringLiteral("--headless")) return true;
  }
  return false;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
  // Before anything writes a byte, so the first line is not the one that is lost.
  attachParentConsole();
#endif

  const bool headless = wantsHeadless(argc, argv);

  auto app = headless
      ? std::unique_ptr<QCoreApplication>(new QCoreApplication(argc, argv))
      : std::unique_ptr<QCoreApplication>(new QApplication(argc, argv));
  QCoreApplication::setApplicationName   (QStringLiteral("kobayashi"));

  // Several sizes from one QIcon, so the window manager takes what it wants rather
  // than scaling a single bitmap badly. Set on the application, not the window: dialogs
  // inherit it, and the taskbar entry exists before any window does.
  //
  // Only when there is a GUI application to set it on. --selftest and --headless build a
  // QCoreApplication, and this static reaches for a QApplication that is not there — it
  // segfaults rather than failing politely, and it does so in the two modes that run on
  // machines with no display at all.
  if (!headless)
  {
    auto icon = QIcon{};
    for (const auto size : { 16, 32, 48, 64, 128, 256 })
      icon.addFile(QStringLiteral(":/kobayashi-%1.png").arg(size));
    QApplication::setWindowIcon(icon);
  }
  QCoreApplication::setApplicationVersion(QStringLiteral(KOBAYASHI_VERSION));

  // Stored settings are the defaults, so a double-clicked launch behaves like the
  // last configured one. An explicitly typed flag still overrides them.
  const auto stored = DST::DESK::App::Settings::load();

  auto parser = QCommandLineParser{};
  parser.setApplicationDescription(
      QStringLiteral("Receives microphone and meeting audio from the dstORCH browser "
                     "extension and records each stream separately."));
  parser.addHelpOption();
  parser.addVersionOption();

  const auto selfTestOption = QCommandLineOption(
      QStringLiteral("selftest"),
      QStringLiteral("Check that this machine can actually run the app — Qt, WebSockets, "
                     "TLS, the port and the output directory — then exit."));

  const auto portOption = QCommandLineOption(
      QStringLiteral("port"), QStringLiteral("Loopback port."),
      QStringLiteral("port"), QString::number(stored.port));

  const auto outputOption = QCommandLineOption(
      QStringLiteral("output"), QStringLiteral("Directory for recorded audio."),
      QStringLiteral("dir"), stored.outputDir);

  const auto tokenOption = QCommandLineOption(
      QStringLiteral("token"), QStringLiteral("Shared secret the client must present."),
      QStringLiteral("secret"), stored.token);

  const auto noTranscribeOption = QCommandLineOption(
      QStringLiteral("no-transcribe"),
      QStringLiteral("Record only, even when DEEPGRAM_API_KEY is set."));

  const auto modelOption = QCommandLineOption(
      QStringLiteral("model"), QStringLiteral("Transcription model."),
      QStringLiteral("name"), stored.model);

  const auto diarizeOption = QCommandLineOption(
      QStringLiteral("diarize"),
      QStringLiteral("Ask the engine to label speakers within the meeting stream. It "
                     "separates distinct voices well and similar ones poorly, so it "
                     "refines the transcript rather than defining it."));

  const auto originOption = QCommandLineOption(
      QStringLiteral("origin"),
      QStringLiteral("Allowed Origin header, repeatable. Any origin is accepted when "
                     "none is given."),
      QStringLiteral("url"));

  parser.addOption(selfTestOption);
  parser.addOption(portOption);
  parser.addOption(outputOption);
  parser.addOption(tokenOption);
  parser.addOption(originOption);
  parser.addOption(noTranscribeOption);
  parser.addOption(modelOption);
  const auto headlessOption = QCommandLineOption(
      QStringLiteral("headless"),
      QStringLiteral("Run without a window, printing the transcript to the console."));

  parser.addOption(diarizeOption);
  parser.addOption(headlessOption);
  parser.process(*app);

  bool       portOk = false;
  const auto port   = parser.value(portOption).toUInt(&portOk);

  if (!portOk || port == 0 || port > 65535)
  {
    QTextStream(stderr) << "Invalid --port: " << parser.value(portOption) << Qt::endl;
    return 2;
  }

  const QString outputDir = parser.value(outputOption);

  if (parser.isSet(selfTestOption))
    return DST::DESK::App::printSelfTest(
        DST::DESK::App::runSelfTest(static_cast<std::uint16_t>(port), outputDir));

  // The self-test runs before serving too. Its checks are the ones that would
  // otherwise fail silently mid-session rather than at startup.
  const auto report =
      DST::DESK::App::runSelfTest(static_cast<std::uint16_t>(port), outputDir);

  if (!report.ok())
  {
    QTextStream(stderr) << "Refusing to start — this machine is not ready:" << Qt::endl;
    DST::DESK::App::printSelfTest(report);
    return 1;
  }

  auto cfg = DST::DESK::IO::ServerConfig{};
  cfg.port           = static_cast<std::uint16_t>(port);
  cfg.outputDir      = outputDir;
  cfg.token          = parser.value(tokenOption);
  cfg.allowedOrigins = parser.values(originOption);

  // Transcription follows the key rather than a flag: a reviewer who sets
  // DEEPGRAM_API_KEY gets transcripts, and development without one still records.
  // The key is never a command-line argument, which would put it in shell history
  // and in the process list for every other user on the machine.
  cfg.stt.apiKey  = stored.apiKey;
  cfg.stt.model   = parser.value(modelOption);
  cfg.stt.diarize = parser.isSet(diarizeOption) || stored.diarize;
  cfg.transcribe  = !cfg.stt.apiKey.isEmpty() && !parser.isSet(noTranscribeOption);

  const QString keyHint =
      parser.isSet(noTranscribeOption)
          ? QStringLiteral("Transcription is off because --no-transcribe was given.")
          : QStringLiteral("No transcription key was found.\n\nSet DEEPGRAM_API_KEY, or "
                           "put it in\n%1\n\n    [deepgram]\n    apiKey=YOUR_KEY\n\n"
                           "A double-clicked application inherits no shell environment, "
                           "so the config file is what makes it work outside a terminal.")
                .arg(DST::DESK::App::Settings::path());

  if (cfg.transcribe)
    QTextStream(stdout) << "Transcribing with " << cfg.stt.model << Qt::endl;
  else
    QTextStream(stdout) << keyHint << Qt::endl;

  auto server = DST::DESK::IO::WsServer(std::move(cfg));
  if (!server.start()) return 1;

  auto window = std::unique_ptr<DST::DESK::App::MainWindow>{};

  if (headless)
  {
    // The console path consumes the same signals the window does, so there is one
    // pipeline rather than two that can drift apart.
    QObject::connect(&server, &DST::DESK::IO::WsServer::utteranceCommitted,
                     [](const DST::DESK::Core::Utterance& utterance)
    {
      qInfo("  %02d:%05.2f  %-10s %s", int(utterance.start) / 60,
            utterance.start - (int(utterance.start) / 60) * 60,
            DST::DESK::Core::Stream::label(utterance.stream), utterance.text.c_str());
    });
  }
  else
  {
    auto effective = stored;
    effective.port      = static_cast<std::uint16_t>(port);
    effective.token     = cfg.token;
    effective.outputDir = outputDir;
    effective.model     = cfg.stt.model;
    effective.diarize   = cfg.stt.diarize;

    window = std::make_unique<DST::DESK::App::MainWindow>(server, effective, cfg.transcribe, keyHint);
    window->show();
  }

  installSignalHandlers();
  return app->exec();
}
