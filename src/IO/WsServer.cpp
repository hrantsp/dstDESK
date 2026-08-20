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
  if (cfg_.record) qInfo("Recording into %s", qUtf8Printable(QDir::toNativeSeparators(cfg_.outputDir)));
  else             qInfo("Not recording audio (--no-record); frame accounting is kept");
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

QString WsServer::sessionDirectory() const
{
  // Named for the second it began in, which is what makes a recording findable — and
  // which is not unique. Two sessions inside one second share the name, and the second
  // one's stream-open truncates the first one's mic.wav while the summary still reports
  // the frames it wrote: a clean session over audio that no longer exists.
  //
  // Reachable without trying: the extension's first reconnect delay is 500 ms, so a
  // socket that blips shortly after capture starts reconnects inside the same second.
  // Same failure the per-stream `-2` suffix already exists to prevent, one level up.
  const auto base = QDir(cfg_.outputDir);
  const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

  if (!base.exists(stamp)) return base.filePath(stamp);

  // Bounded rather than while(true): if something else is creating these as fast as we
  // can test them, failing to record is better than spinning. 1000 sessions in one
  // second is not a case worth serving.
  for (int nth = 2; nth < 1000; ++nth)
  {
    const auto candidate = QStringLiteral("%1-%2").arg(stamp).arg(nth);
    if (!base.exists(candidate)) return base.filePath(candidate);
  }
  return base.filePath(stamp);
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

    // closeSession() returns with the session still alive whenever it is waiting for a
    // transcription connection to finalise, and the replacement is about to overwrite
    // session_ — which would destroy the Session while its socket, its engine clients
    // and its stall timer were all still connected to this server. The old socket then
    // delivered audio into the *new* session's recorder and transcript, and its
    // eventual `disconnected` tore that session down; the clients and the timer leaked,
    // because the handler that frees them checks the session id and finds a different
    // one. Measured at about 14 kB and one live 2 s timer per displacement.
    //
    // So a displaced session is retired here and now. It loses whatever the engine had
    // not yet finalised, which is a few words of a session the user has already
    // replaced — and the window clears the transcript for the new one in any case.
    if (session_) finishSession();
  }

  session_ = std::make_unique<Session>();
  session_->id     = ++nextSessionId_;
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

  // Everything that reaches closeSession() is terminal — bye, a disconnect, a
  // replacement — and the recorders are shut at that moment. Acting on control messages
  // afterwards means a stream-open during teardown, which creates a recording nothing
  // will ever close properly and an `opened` flag the summary has already reported on.
  if (session_->closing) return;

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

  // A second hello is a client fault — PROTOCOL.md §3 says hello is the first message on
  // the connection. Acting on it would open a second session directory while the streams
  // opened under the first went on recording into the old one, so the summary would
  // describe a session whose files are somewhere else.
  if (session_->helloDone)
  {
    qWarning("Ignoring a second hello on a connection that has already handshaken");
    return;
  }

  session_->helloDone         = true;
  session_->client            = msg.value(QStringLiteral("client")).toString();
  session_->contextEpochUtcMs = msg.value(QStringLiteral("contextEpochUtcMs")).toDouble();

  // One directory per session, so successive runs never overwrite each other.
  session_->dir = sessionDirectory();
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

  // A second open for a stream that is already open is a client fault, not an
  // instruction to begin again. Acting on it would truncate the recording in progress
  // and reset the accounting that describes it, so the session summary would report a
  // clean run over audio that had just been destroyed.
  if (session_->opened[slot])
  {
    qWarning("Ignoring stream-open for %s, which is already open", Core::Stream::label(stream));
    return;
  }

  // PROTOCOL.md §3 allows a stream to be closed and opened again on one connection.
  // Each open gets its own file: the second one wrote over the first for as long as
  // the name was fixed, and reported the loss as a clean session.
  const int     nth  = ++session_->opens[slot];
  const QString name = nth == 1 ? QStringLiteral("%1.wav").arg(streamKey(stream))
                                : QStringLiteral("%1-%2.wav").arg(streamKey(stream)).arg(nth);
  const QString path = QDir(session_->dir).filePath(name);

  // toStdU16String, not toStdString. std::filesystem::path reads a narrow string in
  // the platform's native encoding, which on Windows is the active code page rather
  // than UTF-8 — so a path with non-ASCII characters would silently resolve wrong
  // there while working perfectly on Linux and macOS. UTF-16 converts correctly on
  // both.
  if (!cfg_.record)
  {
    session_->recorders[slot].openCounting(Core::kSampleRate);
  }
  else if (!session_->recorders[slot].open(path.toStdU16String(), Core::kSampleRate))
  {
    // Leaving the stream unopened discards its audio *and* skips transcription for it,
    // so the window would read "Capturing" while doing neither. On a console this was a
    // warning nobody sees; a double-clicked application had nowhere to put it at all.
    qWarning("Cannot open %s for writing", qUtf8Printable(path));
    emit notice(tr("Cannot write %1 — the %2 stream is not being captured. "
                   "Choose a writable folder in Settings, or start with --no-record.")
                    .arg(QDir::toNativeSeparators(path),
                         QString::fromLatin1(Core::Stream::label(stream))));
    return;
  }

  session_->opened[slot]   = true;
  session_->reported[slot] = false;
  session_->sawFrame[slot] = false;
  session_->transcript.openStream(stream);

  // The watchdog's clock for this stream starts here, not when a transcription
  // connection is made. An open stream holds the merge watermark down, and a stream that
  // never sends a frame never gets a connection — so it held it down for the whole call
  // and nothing was ever committed while the *other* stream was being transcribed
  // perfectly. Everything then arrived at once in the end-of-session flush.
  session_->lastResult[slot] = QDateTime::currentDateTimeUtc();
  armStallWatch();
  qInfo("Stream %s open -> %s", Core::Stream::label(stream),
        cfg_.record ? qUtf8Printable(path) : "not recorded");
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
  reportStream(slot);
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

  // hello settled the frame size for this session and nothing after it did. Without
  // this the handshake check is decoration: a client can declare 512 samples and then
  // send a 65535-sample frame, which the parser accepts because its own length is
  // self-consistent. PROTOCOL.md §5.3.
  if (frame.header.frameSamples != Core::kFrameSamples)
  {
    rejectWith("malformed-frame", Core::kCloseMalformedFrame,
               QStringLiteral("frame declares %1 samples, the session negotiated %2")
                   .arg(frame.header.frameSamples).arg(Core::kFrameSamples));
    return;
  }

  const auto slot = indexOf(frame.header.stream);
  // PROTOCOL.md §3: audio for an unopened stream is discarded, not fatal.
  if (!session_->opened[slot]) return;

  // This stream is producing audio, so it is entitled to the long stall leash rather
  // than the short one a stream that has never spoken gets.
  session_->sawFrame[slot] = true;

  Core::samplesInto(frame, scratch_);

  // A frame the recorder rejected went backwards on the shared clock. Forwarding it
  // anyway would feed the engine audio it has already heard and shift every word
  // timing after it — the same corruption gap padding exists to prevent, in the
  // opposite direction.
  if (!session_->recorders[slot].accept(frame.header, scratch_)) return;

  // A recorder that has stopped accepting audio — a full disk, a removed drive, a
  // quota — otherwise says nothing at all: frames keep arriving, the summary keeps
  // counting them, and the file on disk stops growing. Recording is what this
  // application shows for its work when there is no transcription key, so failing at it
  // silently is the worst way it can fail.
  if (session_->recorders[slot].stats().writeFailed && !session_->reportedWriteFailure[slot])
  {
    session_->reportedWriteFailure[slot] = true;
    const auto stream = static_cast<Core::Stream::Value>(slot);
    qWarning("Recording failed for %s — the file stopped accepting audio",
             Core::Stream::label(stream));
    emit notice(tr("Recording stopped for %1 — the disk would not accept it. "
                   "Transcription is unaffected.")
                    .arg(QString::fromLatin1(Core::Stream::label(stream))));
  }

  if (cfg_.transcribe) forwardToEngine(slot, frame);
}

void WsServer::forwardToEngine(std::size_t slot, const Core::ParsedFrame& frame)
{
  auto*& client = session_->stt[slot];

  // A client that was asked to finish and has finished means this stream was closed and
  // opened again: its results are all in, and the new audio needs a connection of its
  // own. One that gave up must not be replaced — it exhausted its retries, said so, and
  // rebuilding it here would start that over on every frame.
  if (client != nullptr && client->isFinished() && !client->gaveUp())
  {
    client->disconnect(this);
    client->deleteLater();
    client = nullptr;
  }

  if (client == nullptr)
  {
    if (session_->sttGaveUp[slot]) return;
    startTranscription(frame.header.stream);
    if (client == nullptr) return;
  }

  // The position on the shared clock travels with the audio. Everything that follows
  // from it — the engine's time origin, silence for a gap, re-basing after a dropped
  // connection — belongs to the client, because it is the only thing that knows which
  // connection this audio is about to go to.
  client->sendAudio(frame.payload,
                    double(frame.header.sampleIndex) / double(Core::kSampleRate));
}


void WsServer::startTranscription(Core::Stream::Value stream)
{
  const auto slot = indexOf(stream);

  auto* client = new SttClient(cfg_.stt, stream, this);
  session_->stt[slot] = client;
  session_->lastResult[slot] = QDateTime::currentDateTimeUtc();
  armStallWatch();

  // Which session this client belongs to. A replaced session leaves its old clients
  // still connected and still finalising whatever they had buffered, and without this
  // check that tail lands in the *new* session's transcript — stamped with the old
  // client's time origin, so it appears as a duplicate several seconds adrift.
  //
  // By id, not by address: a session freed and another allocated can land on the same
  // address, and then a stale client passes the check.
  const auto owner = session_->id;

  const auto mine = [this, owner] { return session_ && session_->id == owner; };

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

  connect(client, &SttClient::interrupted, this, [this, mine, stream](QString reason)
  {
    if (!mine()) return;

    // Interrupted is not closed. The stream may speak again, so it keeps its place in
    // the merger — but it must stop holding the watermark down while it is silent, or
    // the other stream's transcript freezes for the length of the outage.
    session_->transcript.setStalled(stream, true);
    drainTranscript();

    const auto slot = indexOf(stream);
    if (!session_->reportedInterruption[slot])
    {
      session_->reportedInterruption[slot] = true;
      emit notice(tr("Transcription for %1 dropped and is reconnecting — %2. "
                     "Recording is unaffected.")
                      .arg(QString::fromLatin1(Core::Stream::label(stream)), reason));
    }
  });

  connect(client, &SttClient::resumed, this, [this, mine, stream](double lostSeconds)
  {
    if (!mine()) return;

    session_->reportedInterruption[indexOf(stream)] = false;
    qInfo("Transcription for %s resumed; %.1f s of audio was never transcribed",
          Core::Stream::label(stream), lostSeconds);
    emit notice(tr("Transcription for %1 resumed. About %2 s was not transcribed; the "
                   "recording of it is intact.")
                    .arg(QString::fromLatin1(Core::Stream::label(stream)))
                    .arg(lostSeconds, 0, 'f', 1));
  });

  connect(client, &SttClient::finished, this, [this, mine, stream]
  {
    if (!mine()) return;

    // A stream whose transcription gave up must not be started again by the next frame
    // to arrive, or it retries for the rest of the meeting.
    const auto slot = indexOf(stream);
    if (session_->stt[slot] != nullptr && session_->stt[slot]->gaveUp())
      session_->sttGaveUp[slot] = true;

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

  client->start();
}

void WsServer::armStallWatch()
{
  // Only when there is transcription to watch: with none, no stream can be behind and a
  // 2 s timer would be pure overhead.
  if (!cfg_.transcribe || session_ == nullptr || session_->stallWatch != nullptr) return;

  // Parented to the server so it survives being deleted from inside its own slot,
  // and stopped explicitly when the session ends. Left to the parent alone it would
  // outlive every session that created one, and a long-running window would end up
  // carrying a 2 s timer per start-and-stop cycle for the rest of its life.
  session_->stallWatch = new QTimer(this);
  connect(session_->stallWatch, &QTimer::timeout, this, &WsServer::checkForStalls);
  session_->stallWatch->start(2000);
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

  for (std::size_t slot = 0; slot < session_->opened.size(); ++slot)
  {
    // Judged on the stream being open, not on it having a transcription connection. A
    // stream that has never sent a frame has no connection to judge — and it is exactly
    // the case that used to freeze the whole transcript, because it held the watermark
    // at zero and nothing here ever looked at it.
    if (!session_->opened[slot]) continue;

    const auto quietFor = session_->lastResult[slot].msecsTo(now);
    const auto limit    = session_->sawFrame[slot] ? kStallAfterMs : kSilentStreamMs;

    if (quietFor > limit)
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

void WsServer::reportStream(std::size_t slot)
{
  if (!session_ || session_->reported[slot]) return;

  const auto& rec   = session_->recorders[slot];
  const auto& stats = rec.stats();
  if (!stats.started) return;

  // Reported when the stream closes, not only when the session does. A recorder's
  // counters are reset by the next open, so a stream that is closed and opened again
  // would otherwise have the first open's frames disappear from the summary — which
  // reads as a clean short session rather than as half the audio going unaccounted for.
  const auto nth = session_->opens[slot];
  const auto name = nth > 1 ? QStringLiteral("%1 (%2)").arg(QLatin1String(streamKey(static_cast<Core::Stream::Value>(slot)))).arg(nth)
                            : QString::fromLatin1(streamKey(static_cast<Core::Stream::Value>(slot)));

  qInfo("  %-12s %6llu frames  %8.2f s  %u gaps (%llu padded samples)  %u rejected%s",
        qUtf8Printable(name),
        static_cast<unsigned long long>(stats.frames),
        rec.durationSeconds(),
        stats.gaps,
        static_cast<unsigned long long>(stats.paddedSamples),
        stats.rejected,
        stats.writeFailed ? "  WRITE FAILED — the recording is incomplete" : "");

  if (stats.resyncs > 0)
    qWarning("  %-12s %u gap(s) too large to be real — not padded, so this recording's "
             "timeline steps rather than running continuously",
             qUtf8Printable(name), stats.resyncs);

  session_->reported[slot] = true;
}

void WsServer::reportSession()
{
  if (!session_) return;
  for (std::size_t slot = 0; slot < session_->recorders.size(); ++slot) reportStream(slot);
}

void WsServer::closeSession()
{
  if (!session_ || session_->closing) return;
  session_->closing = true;

  for (auto& rec : session_->recorders) rec.close();

  // Ask each engine connection to finalise, then wait. Results for the last few
  // seconds of speech arrive after the request, so flushing now would discard them.
  //
  // Counted first, and over a copy of the pointers. finish() can answer synchronously:
  // a connection that never came up has nothing to finalise, so it aborts and emits
  // `finished` from inside this call. That handler decrements the count, and if the
  // count is still being built it reaches zero early, calls finishSession(), and frees
  // the session — while this loop is still walking an array inside it. That is a
  // segfault on teardown, and the way to reach it is to stop capture before the
  // transcription socket has connected: a wrong key, no network, or simply a short
  // session. Both guards below are for the same reason.
  const auto clients = session_->stt;
  const auto owner   = session_->id;

  session_->sttAwaiting = 0;
  for (auto* client : clients)
    if (client != nullptr && !client->isFinished()) ++session_->sttAwaiting;

  if (session_->sttAwaiting == 0)
  {
    finishSession();
    return;
  }

  for (auto* client : clients)
  {
    if (client == nullptr || client->isFinished()) continue;
    client->finish();
    if (!session_ || session_->id != owner) return; // finished synchronously; already gone
  }

  // A connection that never answers must not strand the session forever.
  //
  // Bound to the session that armed it. The usual case is that the engines answer in a
  // few hundred milliseconds and this timer is left running with nothing to do — and
  // five seconds later it would tear down whichever session existed by then. Stopping
  // and starting capture again inside that window is entirely ordinary, and the second
  // session died with nothing to explain it.
  QTimer::singleShot(5000, this, [this, owner]
  {
    if (!session_ || session_->id != owner) return;
    finishSession();
  });
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

  // Same reason as the clients below: parented to the server, so nothing else would
  // ever stop it.
  if (session_->stallWatch != nullptr)
  {
    session_->stallWatch->stop();
    session_->stallWatch->deleteLater();
    session_->stallWatch = nullptr;
  }

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
