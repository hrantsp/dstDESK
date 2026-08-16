#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <QProcessEnvironment>
#include <QTimer>
#include <csignal>
#include "Core/Protocol.hpp"
#include "IO/WsServer.hpp"
#include "SelfTest.hpp"

namespace {

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

} // namespace

int main(int argc, char** argv)
{
  auto app = QCoreApplication(argc, argv);
  QCoreApplication::setApplicationName   (QStringLiteral("dstdesk"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

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
      QStringLiteral("port"), QString::number(DST::DESK::Core::kDefaultPort));

  const auto outputOption = QCommandLineOption(
      QStringLiteral("output"), QStringLiteral("Directory for recorded audio."),
      QStringLiteral("dir"), QDir::current().filePath(QStringLiteral("out")));

  const auto tokenOption = QCommandLineOption(
      QStringLiteral("token"), QStringLiteral("Shared secret the client must present."),
      QStringLiteral("secret"), QString());

  const auto noTranscribeOption = QCommandLineOption(
      QStringLiteral("no-transcribe"),
      QStringLiteral("Record only, even when DEEPGRAM_API_KEY is set."));

  const auto modelOption = QCommandLineOption(
      QStringLiteral("model"), QStringLiteral("Transcription model."),
      QStringLiteral("name"), QStringLiteral("nova-3"));

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
  parser.addOption(diarizeOption);
  parser.process(app);

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
  cfg.stt.apiKey  = QProcessEnvironment::systemEnvironment().value(QStringLiteral("DEEPGRAM_API_KEY"));
  cfg.stt.model   = parser.value(modelOption);
  cfg.stt.diarize = parser.isSet(diarizeOption);
  cfg.transcribe  = !cfg.stt.apiKey.isEmpty() && !parser.isSet(noTranscribeOption);

  if (cfg.transcribe)
    QTextStream(stdout) << "Transcribing with " << cfg.stt.model << Qt::endl;
  else if (parser.isSet(noTranscribeOption))
    QTextStream(stdout) << "Recording only (--no-transcribe)." << Qt::endl;
  else
    QTextStream(stdout) << "Recording only — set DEEPGRAM_API_KEY to transcribe." << Qt::endl;

  auto server = DST::DESK::IO::WsServer(std::move(cfg));
  if (!server.start()) return 1;

  installSignalHandlers();
  return app.exec();
}
