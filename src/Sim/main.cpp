#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include "Core/Protocol.hpp"
#include "Simulate.hpp"

int main(int argc, char** argv)
{
  auto app = QCoreApplication(argc, argv);
  QCoreApplication::setApplicationName   (QStringLiteral("dstsim"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  auto parser = QCommandLineParser{};
  parser.setApplicationDescription(
      QStringLiteral("Stands in for the dstORCH browser extension: connects to a "
                     "running dstdesk, speaks the real protocol, and sends generated "
                     "tones on both streams.\n\n"
                     "The microphone stream carries 440 Hz and the meeting stream "
                     "660 Hz, so the recordings can be checked by ear — one clean "
                     "tone per file means capture and routing are correct."));
  parser.addHelpOption();
  parser.addVersionOption();

  const auto portOption = QCommandLineOption(
      QStringLiteral("port"), QStringLiteral("Port dstdesk is listening on."),
      QStringLiteral("port"), QString::number(DST::DESK::Core::kDefaultPort));

  const auto tokenOption = QCommandLineOption(
      QStringLiteral("token"), QStringLiteral("Shared secret to present in the handshake."),
      QStringLiteral("secret"), QString());

  const auto secondsOption = QCommandLineOption(
      QStringLiteral("seconds"),
      QStringLiteral("Seconds of audio to send. Rounded down to whole 32 ms frames."),
      QStringLiteral("n"), QStringLiteral("5"));

  const auto gapOption = QCommandLineOption(
      QStringLiteral("gap"),
      QStringLiteral("Drop a run of meeting frames midway, so gap padding can be "
                     "observed rather than assumed."));

  const auto micOption = QCommandLineOption(
      QStringLiteral("mic"),
      QStringLiteral("WAV to send on the microphone stream (16 kHz mono 16-bit). "
                     "Without it, a 440 Hz tone is sent."),
      QStringLiteral("file"));

  const auto meetingOption = QCommandLineOption(
      QStringLiteral("meeting"),
      QStringLiteral("WAV to send on the meeting stream. Without it, a 660 Hz tone."),
      QStringLiteral("file"));

  const auto offsetOption = QCommandLineOption(
      QStringLiteral("offset"),
      QStringLiteral("Seconds to delay the meeting stream, so the two overlap the way "
                     "a conversation does."),
      QStringLiteral("n"), QStringLiteral("0"));

  parser.addOption(micOption);
  parser.addOption(meetingOption);
  parser.addOption(offsetOption);
  parser.addOption(portOption);
  parser.addOption(tokenOption);
  parser.addOption(secondsOption);
  parser.addOption(gapOption);
  parser.process(app);

  bool       portOk = false;
  const auto port   = parser.value(portOption).toUInt(&portOk);

  if (!portOk || port == 0 || port > 65535)
  {
    QTextStream(stderr) << "Invalid --port: " << parser.value(portOption) << Qt::endl;
    return 2;
  }

  auto cfg = DST::DESK::Sim::SimulateConfig{};
  cfg.port      = static_cast<std::uint16_t>(port);
  cfg.token     = parser.value(tokenOption);
  cfg.seconds   = parser.value(secondsOption).toDouble();
  cfg.injectGap = parser.isSet(gapOption);
  cfg.micFile   = parser.value(micOption);
  cfg.tabFile   = parser.value(meetingOption);
  cfg.tabOffset = parser.value(offsetOption).toDouble();

  if (cfg.seconds <= 0.0)
  {
    QTextStream(stderr) << "Invalid --seconds: " << parser.value(secondsOption) << Qt::endl;
    return 2;
  }

  return DST::DESK::Sim::runSimulation(cfg);
}
