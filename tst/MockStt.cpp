// A stand-in for the transcription service that can be made to misbehave on purpose.
//
// It exists because the reconnect logic in SttClient cannot be believed without it. That
// code only runs when a connection dies unexpectedly, which the live service politely
// declines to do on request — so before this, the hardest part of the application to get
// right was the part with no test, and three separate mistakes in it were found only by
// pointing the application at something that drops sockets on purpose.
//
// It speaks enough of the engine's streaming shape to drive the client: one final result
// per second of audio received, timed from the audio *this connection* has received,
// which is the property the client's clock mapping has to survive.
//
//   kobayashi-mockstt --port 9001 --drop-after 3        first connection dies at 3 s of audio
//   kobayashi-mockstt --port 9001 --drop-after 1 --drop-all   every connection dies
//
// Point the application at it with `kobayashi --stt-endpoint ws://127.0.0.1:9001`.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QWebSocket>
#include <QWebSocketServer>
#include "Core/Protocol.hpp"

namespace {

using DST::DESK::Core::kSampleRate;

/// One connection's state. The engine times from the audio it has received, so both
/// counters belong to the connection rather than to the server.
struct Peer
{
  int     index    = 0;
  qint64  received = 0;   // bytes of PCM16
  double  emitted  = 0.0; // seconds already reported as final
};

QString resultMessage(double start, double duration, const QString& text)
{
  auto words = QJsonArray{};
  words.append(QJsonObject{ { QStringLiteral("confidence"), 0.9 } });

  const auto alternative = QJsonObject{
    { QStringLiteral("transcript"), text },
    { QStringLiteral("words"),      words } };

  const auto message = QJsonObject{
    { QStringLiteral("type"),     QStringLiteral("Results") },
    { QStringLiteral("start"),    start },
    { QStringLiteral("duration"), duration },
    { QStringLiteral("is_final"), true },
    { QStringLiteral("channel"),  QJsonObject{
        { QStringLiteral("alternatives"), QJsonArray{ alternative } } } } };

  return QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

} // namespace

int main(int argc, char** argv)
{
  auto app = QCoreApplication(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("kobayashi-mockstt"));

  auto parser = QCommandLineParser{};
  parser.setApplicationDescription(
      QStringLiteral("A transcription service that drops connections on purpose, so the "
                     "client's reconnection can be tested."));
  parser.addHelpOption();

  const auto portOption = QCommandLineOption(
      QStringLiteral("port"), QStringLiteral("Port to listen on."),
      QStringLiteral("port"), QStringLiteral("9001"));

  // Measured in audio rather than in wall time, so a slow machine does not move the
  // point at which the connection dies and the check stays deterministic.
  const auto dropOption = QCommandLineOption(
      QStringLiteral("drop-after"),
      QStringLiteral("Abort the connection once this many seconds of audio have been "
                     "received. 0 never drops."),
      QStringLiteral("seconds"), QStringLiteral("0"));

  const auto dropAllOption = QCommandLineOption(
      QStringLiteral("drop-all"),
      QStringLiteral("Drop every connection, not only the first. Exercises the retry "
                     "budget and the refusal at the end of it."));

  parser.addOption(portOption);
  parser.addOption(dropOption);
  parser.addOption(dropAllOption);
  parser.process(app);

  const auto port      = static_cast<quint16>(parser.value(portOption).toUShort());
  const auto dropAfter = parser.value(dropOption).toDouble();
  const bool dropAll   = parser.isSet(dropAllOption);

  auto out    = QTextStream(stdout);
  auto server = QWebSocketServer(QStringLiteral("kobayashi-mockstt"),
                                 QWebSocketServer::NonSecureMode);

  if (!server.listen(QHostAddress::LocalHost, port))
  {
    QTextStream(stderr) << "cannot listen on 127.0.0.1:" << port << " — "
                        << server.errorString() << Qt::endl;
    return 1;
  }

  // Printed so a caller can wait for readiness rather than sleeping a guessed interval.
  out << "listening " << server.serverPort() << Qt::endl;
  out.flush();

  int connections = 0;

  QObject::connect(&server, &QWebSocketServer::newConnection, [&]
  {
    auto* socket = server.nextPendingConnection();
    auto* peer   = new Peer{ ++connections, 0, 0.0 };
    socket->setProperty("peer", QVariant::fromValue(reinterpret_cast<quintptr>(peer)));

    out << "connection " << peer->index << " opened" << Qt::endl;
    out.flush();

    QObject::connect(socket, &QWebSocket::disconnected, socket, [socket, peer]
    {
      delete peer;
      socket->deleteLater();
    });

    QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket,
                     [socket, peer, dropAfter, dropAll, &out](const QByteArray& data)
    {
      peer->received += data.size();
      const double have = double(peer->received) / 2.0 / double(kSampleRate);

      if (dropAfter > 0.0 && (dropAll || peer->index == 1) && have >= dropAfter)
      {
        // abort(), not close(): a clean close is the case the client already handles.
        // What has to be survived is the connection vanishing without a word.
        out << "connection " << peer->index << " dropped at " << have << " s of audio"
            << Qt::endl;
        out.flush();
        socket->abort();
        return;
      }

      while (have - peer->emitted >= 1.0)
      {
        socket->sendTextMessage(resultMessage(
            peer->emitted, 1.0,
            QStringLiteral("line %1 on connection %2").arg(int(peer->emitted)).arg(peer->index)));
        peer->emitted += 1.0;
      }
    });

    QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                     [socket, peer, &out](const QString& text)
    {
      if (!text.contains(QStringLiteral("CloseStream"))) return;
      out << "connection " << peer->index << " asked to close" << Qt::endl;
      out.flush();
      socket->close();
    });
  });

  return app.exec();
}
