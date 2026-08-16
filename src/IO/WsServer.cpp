#include <QDateTime>
#include <QTimer>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketCorsAuthenticator>
#include <QtGlobal>
#include "Core/Endian.hpp"
#include "WsServer.hpp"

namespace DST { namespace DESK { namespace IO {
namespace {

// Custom WebSocket close codes live in the 4000-4999 range reserved for
// applications, so they need casting out of the enum Qt models the standard ones as.
QWebSocketProtocol::CloseCode closeCodeOf(std::uint16_t code)         { return static_cast<QWebSocketProtocol::CloseCode>(code); }
std::size_t                       indexOf(Core::Stream::Value stream) { return static_cast<std::size_t>(stream); }

} // namespace

WsServer::WsServer(ServerConfig cfg, QObject* parent)
  : QObject(parent)
  , cfg_(std::move(cfg))
  , server_(QStringLiteral("dstDESK"), QWebSocketServer::NonSecureMode, this)
{
  connect(&server_, &QWebSocketServer::newConnection, this, &WsServer::onNewConnection);

  // Origin is settled during the HTTP upgrade, so a disallowed client gets a failed
  // handshake rather than an accepted socket that is closed a moment later. The
  // difference matters to the client: one is a clear connection error, the other
  // looks like an unexplained drop.
  connect(&server_, &QWebSocketServer::originAuthenticationRequired, this, [this](QWebSocketCorsAuthenticator* auth)
  {
    if (cfg_.allowedOrigins.isEmpty())
    {
      auth->setAllowed(true);
      return;
    }

    const bool allowed = cfg_.allowedOrigins.contains(auth->origin());
    if (!allowed) qWarning("Refused upgrade from origin '%s'", qUtf8Printable(auth->origin()));

    auth->setAllowed(allowed);
  });
}

WsServer::~WsServer()
{
  closeSession();
  server_.close();
}

bool WsServer::start()
{
  // Loopback only, never 0.0.0.0 — PROTOCOL.md §1.
  if (!server_.listen(QHostAddress::LocalHost, cfg_.port))
  {
    qWarning("Cannot listen on 127.0.0.1:%u — %s", cfg_.port, qUtf8Printable(server_.errorString()));
    return false;
  }

  qInfo("Listening on ws://127.0.0.1:%u", server_.serverPort());
  qInfo("Recording into %s", qUtf8Printable(QDir::toNativeSeparators(cfg_.outputDir)));
  if (cfg_.token.isEmpty())      qInfo("No token required (development mode)");
  if (cfg_.allowedOrigins.isEmpty()) qInfo("Any origin accepted (development mode)");
  return true;
}

std::uint16_t WsServer::port() const { return server_.serverPort(); }

void WsServer::updateConfig(const ServerConfig& cfg)
{
  const auto boundPort = cfg_.port;
  cfg_ = cfg;
  cfg_.port = boundPort;
}

const char* WsServer::streamKey(Core::Stream::Value stream)
{
  switch (stream) { case Core::Stream::Mic : return "mic";
                    case Core::Stream::Tab : return "meeting";
                    default                : return "unknown"; }
}

void WsServer::onNewConnection()
{
  auto* socket = server_.nextPendingConnection();
  if (socket == nullptr) return;

  // Origin was already settled during the upgrade — see the constructor.
  if (session_)
  {
    // A reloaded extension leaves the previous socket briefly alive. Refusing the
    // newcomer would make reconnection impossible, so the newcomer wins.
    qInfo("Replacing the existing session");
    closeSession();
  }

  session_ = std::make_unique<Session>();
  session_->socket = socket;

  connect(socket, &QWebSocket::textMessageReceived,   this, &WsServer::onTextMessage);
  connect(socket, &QWebSocket::binaryMessageReceived, this, &WsServer::onBinaryMessage);
  connect(socket, &QWebSocket::disconnected,          this, &WsServer::onDisconnected);

  qInfo("Connected from %s", qUtf8Printable(socket->origin().isEmpty()
                                            ? QStringLiteral("(no origin)")
                                            : socket->origin()));
}

void WsServer::onTextMessage(const QString& message)
{
  if (!session_) return;

  auto err = QJsonParseError{};
  const auto doc = QJsonDocument::fromJson(message.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    qWarning("Ignoring unparseable control message: %s", qUtf8Printable(err.errorString()));
    return;
  }

  const QJsonObject msg  = doc.object();
  const QString     type = msg.value(QStringLiteral("type")).toString();

  if (!session_->helloDone && type != QStringLiteral("hello"))
  {
    rejectWith("handshake-expected", Core::kCloseHandshakeExpected,
               QStringLiteral("first message was '%1'").arg(type));
    return;
  }

  if      (type == QStringLiteral("hello"))        handleHello(msg);
  else if (type == QStringLiteral("stream-open"))  handleStreamOpen(msg);
  else if (type == QStringLiteral("stream-close")) handleStreamClose(msg);
  else if (type == QStringLiteral("bye"))          handleBye();
  else
  {
    // Unknown types are ignored rather than fatal, so the protocol can gain
    // messages without a version bump — PROTOCOL.md §4.
    qInfo("Ignoring unknown control message '%s'", qUtf8Printable(type));
  }
}

void WsServer::handleHello(const QJsonObject& msg)
{
  const int protocol     = msg.value(QStringLiteral("protocol")).toInt(-1);
  const int sampleRate   = msg.value(QStringLiteral("sampleRate")).toInt(-1);
  const int frameSamples = msg.value(QStringLiteral("frameSamples")).toInt(-1);

  if (protocol != Core::kVersion)
  {
    rejectWith("protocol-version-mismatch", Core::kCloseProtocolVersionMismatch,
               QStringLiteral("server speaks %1, client offered %2")
                   .arg(Core::kVersion).arg(protocol));
    return;
  }

  if (!cfg_.token.isEmpty() && msg.value(QStringLiteral("token")).toString() != cfg_.token)
  {
    rejectWith("invalid-token", Core::kCloseInvalidToken, QStringLiteral("token mismatch"));
    return;
  }

  if (sampleRate != int(Core::kSampleRate) || frameSamples != int(Core::kFrameSamples))
  {
    rejectWith("unsupported-audio-format", Core::kCloseUnsupportedAudioFormat,
               QStringLiteral("expected %1 Hz / %2 samples, got %3 Hz / %4 samples")
                   .arg(Core::kSampleRate).arg(Core::kFrameSamples)
                   .arg(sampleRate).arg(frameSamples));
    return;
  }

  session_->helloDone         = true;
  session_->client            = msg.value(QStringLiteral("client")).toString();
  session_->contextEpochUtcMs = msg.value(QStringLiteral("contextEpochUtcMs")).toDouble();

  // One directory per session, so successive runs never overwrite each other.
  session_->dir = QDir(cfg_.outputDir).filePath(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
  QDir().mkpath(session_->dir);

  const QString who = session_->client.isEmpty() ? QStringLiteral("(unnamed client)")
                                                 : session_->client;
  qInfo("Handshake accepted from %s", qUtf8Printable(who));
  emit sessionStarted(who, session_->dir);
  sendReady();
}

void WsServer::handleStreamOpen(const QJsonObject& msg)
{
  const int raw = msg.value(QStringLiteral("stream")).toInt(-1);
  if (raw < 0 || !Core::Stream::isKnown(static_cast<std::uint8_t>(raw)))
  {
    qWarning("Ignoring stream-open for unknown stream %d", raw);
    return;
  }

  const auto stream = static_cast<Core::Stream::Value>(raw);
  const auto slot   = indexOf(stream);

  const QString path = QDir(session_->dir).filePath(QStringLiteral("%1.wav").arg(streamKey(stream)));

  // toStdU16String, not toStdString. std::filesystem::path reads a narrow string in
  // the platform's native encoding, which on Windows is the active code page rather
  // than UTF-8 — so a path with non-ASCII characters would silently resolve wrong
  // there while working perfectly on Linux and macOS. UTF-16 converts correctly on
  // both.
  if (!session_->recorders[slot].open(path.toStdU16String(), Core::kSampleRate))
  {
    qWarning("Cannot open %s for writing", qUtf8Printable(path));
    return;
  }

  session_->opened[slot] = true;
  session_->transcript.openStream(stream);
  qInfo("Stream %s open -> %s", Core::Stream::label(stream), qUtf8Printable(path));
  emit streamOpened(stream);
}

void WsServer::handleStreamClose(const QJsonObject& msg)
{
  const int raw = msg.value(QStringLiteral("stream")).toInt(-1);
  if (raw < 0 || !Core::Stream::isKnown(static_cast<std::uint8_t>(raw))) return;

  const auto slot = indexOf(static_cast<Core::Stream::Value>(raw));
  if (!session_->opened[slot]) return;

  session_->recorders[slot].close();
  session_->opened[slot] = false;
  // The merger is told the stream is closed only once the engine has finished with
  // it, not here. Closing it now would make addFinal discard the results still in
  // flight — which are precisely the last thing that was said.
  if (session_->stt[slot] != nullptr) session_->stt[slot]->finish();
  else                                session_->transcript.closeStream(static_cast<Core::Stream::Value>(raw));
  drainTranscript();
  qInfo("Stream %s closed (%s)",
        Core::Stream::label(static_cast<Core::Stream::Value>(raw)),
        qUtf8Printable(msg.value(QStringLiteral("reason")).toString(QStringLiteral("unspecified"))));
}

void WsServer::handleBye()
{
  qInfo("Client said goodbye");
  closeSession();
}

void WsServer::onBinaryMessage(const QByteArray& message)
{
  if (!session_ || !session_->helloDone) return;

  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(message.constData()),
      static_cast<std::size_t>(message.size()));

  const auto frame = Core::parseFrame(bytes);
  if (!frame)
  {
    rejectWith("malformed-frame", Core::kCloseMalformedFrame,
               QString::fromLatin1(Core::ParseError::what(frame.error)));
    return;
  }

  const auto slot = indexOf(frame.header.stream);
  // PROTOCOL.md §3: audio for an unopened stream is discarded, not fatal.
  if (!session_->opened[slot]) return;

  Core::samplesInto(frame, scratch_);
  session_->recorders[slot].accept(frame.header, scratch_);

  if (cfg_.transcribe)
  {
    if (session_->stt[slot] == nullptr) startTranscription(frame.header.stream, frame.header.sampleIndex);
    if (session_->stt[slot] != nullptr) session_->stt[slot]->sendAudio(frame.payload);
  }
}

void WsServer::startTranscription(Core::Stream::Value stream, std::uint32_t firstSampleIndex)
{
  const auto slot = indexOf(stream);

  auto* client = new SttClient(cfg_.stt, stream, this);
  session_->stt[slot] = client;

  connect(client, &SttClient::finalResult, this,
          [this, stream](double start, double end, QString text, double confidence)
  {
    if (!session_) return;
    session_->transcript.addFinal(stream, start, end, text.toStdString(), confidence);
    drainTranscript();
  });

  connect(client, &SttClient::interimResult, this, [this, stream](double start, QString text)
  {
    if (!session_) return;
    session_->transcript.setInterim(stream, start, text.toStdString());
    emit interimChanged(stream, text);
  });

  connect(client, &SttClient::finished, this, [this, stream]
  {
    if (!session_) return;

    // Now, and only now, can nothing further arrive for this stream. Releasing it
    // here also lets the watermark advance past it so the other stream can commit.
    session_->transcript.closeStream(stream);
    drainTranscript();

    if (session_->closing && --session_->sttAwaiting <= 0) finishSession();
  });

  connect(client, &SttClient::failed, this, [this, stream](QString reason)
  {
    qWarning("Transcription failed for %s: %s", Core::Stream::label(stream),
             qUtf8Printable(reason));
  });

  // The engine counts from the first audio it receives, so this frame's position on
  // the shared capture clock is the offset for every time it later reports.
  client->start(double(firstSampleIndex) / double(Core::kSampleRate));
}

void WsServer::emitUtterance(const Core::Utterance& utterance)
{
  emit utteranceCommitted(utterance);
}

void WsServer::drainTranscript()
{
  if (!session_) return;
  for (const auto& utterance : session_->transcript.takeCommitted()) emitUtterance(utterance);
}

void WsServer::sendReady()
{
  auto msg = QJsonObject{};
  msg[QStringLiteral("type")]     = QStringLiteral("ready");
  msg[QStringLiteral("protocol")] = int(Core::kVersion);
  msg[QStringLiteral("server")]   = QStringLiteral("dstDESK/0.1.0");
  session_->socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void WsServer::rejectWith(const char* code, std::uint16_t closeCode, const QString& detail)
{
  if (!session_ || session_->socket == nullptr) return;

  // The error is always sent before the close, so the failure is diagnosable from
  // the client side rather than appearing as an unexplained disconnect.
  auto msg = QJsonObject{};
  msg[QStringLiteral("type")]    = QStringLiteral("error");
  msg[QStringLiteral("code")]    = QString::fromLatin1(code);
  msg[QStringLiteral("message")] = detail;
  session_->socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));

  qWarning("Rejecting session: %s — %s", code, qUtf8Printable(detail));
  session_->socket->close(closeCodeOf(closeCode), QString::fromLatin1(code));
}

void WsServer::reportSession() const
{
  if (!session_) return;

  for (std::size_t slot = 0; slot < session_->recorders.size(); ++slot)
  {
    const auto& rec   = session_->recorders[slot];
    const auto& stats = rec.stats();
    if (!stats.started) continue;

    qInfo("  %-10s %6llu frames  %8.2f s  %u gaps (%llu padded samples)  %u rejected",
          streamKey(static_cast<Core::Stream::Value>(slot)),
          static_cast<unsigned long long>(stats.frames),
          rec.durationSeconds(),
          stats.gaps,
          static_cast<unsigned long long>(stats.paddedSamples),
          stats.rejected);
  }
}

void WsServer::closeSession()
{
  if (!session_ || session_->closing) return;
  session_->closing = true;

  for (auto& rec : session_->recorders) rec.close();

  // Ask each engine connection to finalise, then wait. Results for the last few
  // seconds of speech arrive after the request, so flushing now would discard them.
  session_->sttAwaiting = 0;
  for (auto* client : session_->stt)
  {
    if (client == nullptr || client->isFinished()) continue;
    ++session_->sttAwaiting;
    client->finish();
  }

  if (session_->sttAwaiting == 0)
  {
    finishSession();
    return;
  }

  // A connection that never answers must not strand the session forever.
  QTimer::singleShot(5000, this, [this] { finishSession(); });
}

void WsServer::finishSession()
{
  if (!session_) return;

  // Whatever the watermark never reached: with one stream ahead of the other at the
  // end, this is usually the last thing that was said.
  for (const auto& utterance : session_->transcript.flush()) emitUtterance(utterance);

  reportSession();
  emit sessionEnded();

  if (session_->socket != nullptr)
  {
    session_->socket->disconnect(this);
    session_->socket->close();
    session_->socket->deleteLater();
  }
  session_.reset();
}

void WsServer::onDisconnected()
{
  qInfo("Client disconnected");
  closeSession();
}

} } } // namespace DST::DESK::IO
