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
    // An empty Origin means the client is not a browser, and is allowed.
    //
    // This looks like a hole and is not the one it looks like. A browser always sets
    // Origin and page script can neither suppress nor forge it, so this check is what
    // stands between a live session and any site the user happens to visit: a WebSocket
    // to loopback needs no CORS preflight, so without it any page could displace the
    // capture and stream its own audio in. A native process can omit the header, but a
    // process already running as this user does not need a socket to do harm — and
    // refusing it would break kobayashi-sim and the wire check, which are what make the
    // protocol testable without a browser at all.
    if (auth->origin().isEmpty())
    {
      auth->setAllowed(true);
      return;
    }

    const bool allowed = cfg_.allowedOrigins.contains(auth->origin());
    if (!allowed)
    {
      qWarning("Refused upgrade from origin '%s'", qUtf8Printable(auth->origin()));

      // A double-clicked application has no console, so this is the only place it can
      // say anything. Without it the window reads "waiting for the extension" while the
      // server is busy refusing that very extension — the most misleading state it can
      // be in, and one that looks like a network fault from either end.
      emit notice(tr("Refused a connection from %1 — that is not the configured origin. "
                     "If it is your extension, restart with:  --origin %1")
                      .arg(auth->origin()));
    }

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
  // HP:TODO: no token by default. The origin check above is what actually keeps web
  // pages out; a token would additionally authenticate native clients, and belongs with
  // a way to provision it that is better than typing the same secret into two places.
  if (cfg_.token.isEmpty())      qInfo("No token required (development mode)");
  for (const auto& origin : cfg_.allowedOrigins) qInfo("Accepting browser origin %s", qUtf8Printable(origin));
  return true;
}

std::uint16_t WsServer::port() const { return server_.serverPort(); }

void WsServer::updateConfig(const ServerConfig& cfg)
{
  const auto boundPort = cfg_.port;
  cfg_ = cfg;
  cfg_.port = boundPort;
}

bool WsServer::tokenMatches(const QString& offered) const
{
  // Constant time in the length of the configured token, so how far a guess got cannot
  // be read off the reply. Over loopback that signal is buried in noise and this is
  // close to ceremony — but it is a few lines, and comparing secrets with == is the
  // habit worth not having.
  const auto expected = cfg_.token.toUtf8();
  const auto actual   = offered.toUtf8();

  auto difference = static_cast<unsigned int>(expected.size() ^ actual.size());
  for (qsizetype ii = 0; ii < expected.size(); ++ii)
  {
    const auto lhs = static_cast<unsigned char>(expected[ii]);
    const auto rhs = ii < actual.size() ? static_cast<unsigned char>(actual[ii]) : 0u;
    difference |= static_cast<unsigned int>(lhs ^ rhs);
  }
  return difference == 0;
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
    // HP:TODO: an accepted client silently displaces a live session. Harmless while
    // the only accepted browser origin is the extension's own, and wrong if the origin
    // list is ever widened: the honest fix is to refuse the second client while a
    // session is recording, and to say so on the socket rather than in a log line.
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

  if (!cfg_.token.isEmpty() && !tokenMatches(msg.value(QStringLiteral("token")).toString()))
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
  session_->lastResult[slot] = QDateTime::currentDateTimeUtc();

  if (session_->stallWatch == nullptr)
  {
    session_->stallWatch = new QTimer(this);
    connect(session_->stallWatch, &QTimer::timeout, this, &WsServer::checkForStalls);
    session_->stallWatch->start(2000);
  }

  // Which session this client belongs to. A replaced session leaves its old clients
  // still connected and still finalising whatever they had buffered, and without this
  // check that tail lands in the *new* session's transcript — stamped with the old
  // client's time origin, so it appears as a duplicate several seconds adrift.
  auto* owner = session_.get();

  const auto mine = [this, owner] { return session_ && session_.get() == owner; };

  connect(client, &SttClient::finalResult, this,
          [this, mine, stream](double start, double end, QString text, double confidence)
  {
    if (!mine()) return;
    noteAlive(stream);
    session_->transcript.addFinal(stream, start, end, text.toStdString(), confidence);

    // A final supersedes whatever interim was showing for this stream, so the live row
    // has to be told. The merger drops its own copy, but without this the last interim
    // stays on screen for the rest of the session — including after it ends.
    emit interimChanged(stream, QString());

    drainTranscript();
  });

  connect(client, &SttClient::interimResult, this,
          [this, mine, stream](double start, QString text)
  {
    if (!mine()) return;
    noteAlive(stream);
    session_->transcript.setInterim(stream, start, text.toStdString());
    emit interimChanged(stream, text);
  });

  connect(client, &SttClient::finished, this, [this, mine, stream]
  {
    if (!mine()) return;

    // Now, and only now, can nothing further arrive for this stream. Releasing it
    // here also lets the watermark advance past it so the other stream can commit.
    session_->transcript.closeStream(stream);
    drainTranscript();

    if (session_->closing && --session_->sttAwaiting <= 0) finishSession();
  });

  connect(client, &SttClient::failed, this, [this, stream](QString reason)
  {
    // Surfaced rather than only logged: a stream whose transcription has died still
    // records audio perfectly, so nothing else about the session looks wrong.
    qWarning("Transcription failed for %s: %s", Core::Stream::label(stream),
             qUtf8Printable(reason));
    emit notice(QStringLiteral("Transcription stopped for %1: %2")
                    .arg(QString::fromLatin1(Core::Stream::label(stream)), reason));
  });

  // The engine counts from the first audio it receives, so this frame's position on
  // the shared capture clock is the offset for every time it later reports.
  client->start(double(firstSampleIndex) / double(Core::kSampleRate));
}

void WsServer::noteAlive(Core::Stream::Value stream)
{
  if (!session_) return;
  const auto slot = indexOf(stream);
  session_->lastResult[slot] = QDateTime::currentDateTimeUtc();
  session_->transcript.setStalled(stream, false);
}

void WsServer::checkForStalls()
{
  if (!session_) return;

  const auto now = QDateTime::currentDateTimeUtc();

  for (std::size_t slot = 0; slot < session_->stt.size(); ++slot)
  {
    if (session_->stt[slot] == nullptr || !session_->opened[slot]) continue;

    const auto quietFor = session_->lastResult[slot].msecsTo(now);

    if (quietFor > kStallAfterMs)
    {
      // Well beyond anything normal: even a silent stream is finalised every few
      // seconds, so total quiet for this long means the connection is not working.
      session_->transcript.setStalled(static_cast<Core::Stream::Value>(slot), true);
      drainTranscript();
    }
  }
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
  msg[QStringLiteral("server")]   = QStringLiteral("Kobayashi/" KOBAYASHI_VERSION);
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

  // Nothing further can arrive, so any interim still showing is now stale. It was
  // never committed text and must not be left looking like it was.
  emit interimChanged(Core::Stream::Mic, QString());
  emit interimChanged(Core::Stream::Tab, QString());

  reportSession();

  // Only for a session that actually began. A client that connects and never
  // handshakes would otherwise make the window announce the end of something it never
  // reported the start of.
  if (session_->helloDone) emit sessionEnded();

  // Parented to the server, so without this they would accumulate for its whole life,
  // each one still connected to the engine. Disconnected first because this may run
  // from inside one of their own signals.
  for (auto*& client : session_->stt)
  {
    if (client == nullptr) continue;
    client->disconnect(this);
    client->finish();
    client->deleteLater();
    client = nullptr;
  }

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
