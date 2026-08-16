#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include "Core/Protocol.hpp"
#include "IO/WsServer.hpp"
#include "SelfTest.hpp"

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

  auto server = DST::DESK::IO::WsServer(std::move(cfg));
  if (!server.start()) return 1;

  return app.exec();
}
