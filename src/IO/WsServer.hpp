// The extension-facing WebSocket server.
//
// Binds loopback, validates the handshake described in rec/PROTOCOL.md §3-4, and
// routes binary frames into one StreamRecorder per stream. This is the Qt-facing
// half; everything it decides with lives in Core and is tested without an event loop.

#ifndef DST_DESK_IO_WSSERVER_HPP
#define DST_DESK_IO_WSSERVER_HPP

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QWebSocketServer>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "Core/Frame.hpp"
#include "Core/StreamRecorder.hpp"
#include "Core/Transcript.hpp"
#include "SttClient.hpp"

class QWebSocket;

namespace DST { namespace DESK { namespace IO {

struct ServerConfig
{
  std::uint16_t port = Core::kDefaultPort;
  QString       outputDir;

  // An empty token means "do not check", and an empty origin list means the opposite:
  // no browser origin is accepted at all. That asymmetry is deliberate — the origin
  // check is the security boundary of PROTOCOL.md §7, so failing closed is the only
  // safe default — and it is stated here because an earlier version of this comment
  // said empty meant "do not check" for both, which is how a caller that forgot to
  // populate this field ended up locking the extension out.
  QString       token;
  QStringList   allowedOrigins;

  // Recording is not something the task asked for; it began as the evidence that
  // frames were arriving intact and stayed because it still is. --no-record turns it
  // off, leaving the frame accounting — gaps, rejects, duration — which is the part
  // that shows the pipeline working.
  bool      record = true;

  // Transcription follows the key: with one, audio is transcribed as well as
  // recorded; without one, the app still records.
  bool      transcribe = false;

  // Whether it is permitted at all. --no-transcribe clears this, and it must survive a
  // settings change: the flag is a decision the user made about this run, and adding a
  // key in the dialog is not a request to overturn it.
  bool      transcribeAllowed = true;

  SttConfig stt;
};

class WsServer : public QObject
{
  Q_OBJECT

public:
  explicit WsServer(ServerConfig cfg, QObject* parent = nullptr);
  ~WsServer() override;

  bool          start();
  std::uint16_t port() const;

  /// Replaces everything except the listening port, which is bound at startup. Takes
  /// effect when the next session opens: a session's recorders and transcription
  /// connections were built from the values current when it began, and swapping them
  /// underneath would give one session two identities.
  void updateConfig(const ServerConfig& cfg);

  /// What the server is running with. Callers that change one setting must start from
  /// this rather than from a fresh ServerConfig, or every field they do not know about
  /// silently reverts to its default — which is how saving the settings dialog once
  /// emptied the origin allowlist and locked the extension out for the rest of the run.
  const ServerConfig& config() const { return cfg_; }

signals:
  /// Settled text, in conversational order. Emitted once per utterance.
  void utteranceCommitted(const Core::Utterance& utterance);

  /// Text that may still change, for the live area. An empty string clears it.
  void interimChanged(Core::Stream::Value stream, const QString& text);

  void sessionStarted(const QString& client, const QString& directory);
  void sessionEnded();
  void streamOpened(Core::Stream::Value stream);
  void notice(const QString& text);

private:
  // One connection at a time. A reconnecting extension must be able to take over, so
  // a new connection replaces the old rather than being refused.
  struct Session
  {
    // Identity that outlives the object. Callbacks and timers armed by one session can
    // still fire after it has gone, and comparing the address would not tell them
    // apart: the allocator is free to hand the same address to the next session, at
    // which point a stale timer looks exactly like a live one.
    std::uint64_t id = 0;

    QWebSocket* socket    = nullptr;
    bool        helloDone = false;
    QString     client;
    double      contextEpochUtcMs = 0.0;
    QString     dir;

    std::array<Core::StreamRecorder, 2> recorders;
    std::array<bool, 2>                 opened = { false, false };

    // How many times each stream has been opened on this connection. PROTOCOL.md §3
    // allows a stream to be closed and opened again, and each open needs its own file:
    // reusing the name would truncate what the previous one recorded.
    std::array<int, 2>  opens    = { 0, 0 };
    std::array<bool, 2> reported = { false, false };

    // Said once per stream, not once per frame: a full disk fails 31 times a second.
    std::array<bool, 2> reportedWriteFailure = { false, false };

    // Whether any audio has ever arrived on this stream since it opened. A stream that
    // is open but has never sent a frame cannot be holding audio the transcript is
    // waiting for, and it gets a much shorter leash than one whose engine has merely
    // gone quiet — see kSilentStreamMs.
    std::array<bool, 2> sawFrame = { false, false };

    // Created with the stream, but only connected once the first frame arrives: the
    // engine measures time from the first audio it receives, so the offset onto the
    // shared capture clock is not known until then.
    std::array<SttClient*, 2> stt     = { nullptr, nullptr };
    std::array<bool, 2>       sttOpen = { false, false };

    // Said once per outage rather than once per retry.
    std::array<bool, 2> reportedInterruption = { false, false };

    // A stream whose transcription exhausted its retries. Remembered on the session
    // rather than on the client, because the client is gone by the time the next
    // frame asks whether to build another.
    std::array<bool, 2> sttGaveUp = { false, false };

    Core::TranscriptMerger transcript;

    // When each stream's engine last said anything. A stream that has gone quiet for
    // longer than the timeout is treated as stalled so it stops holding the transcript
    // back — judged on the engine's silence over wall time, never on how far behind the
    // transcript is, because during a long sentence that distance is just the length of
    // the sentence.
    std::array<QDateTime, 2> lastResult;
    QTimer*                  stallWatch = nullptr;

    // Teardown is two-phase: the engine is asked to finalise, and the session is only
    // destroyed once it has answered. Doing it in one step drops the last utterance.
    bool closing     = false;
    int  sttAwaiting = 0;
  };

  void onNewConnection();
  void onTextMessage  (const QString&    message);
  void onBinaryMessage(const QByteArray& message);
  void onDisconnected ();

  void handleHello      (const QJsonObject& msg);
  void handleStreamOpen (const QJsonObject& msg);
  void handleStreamClose(const QJsonObject& msg);
  void handleBye        ();

  void startTranscription(Core::Stream::Value stream);
  void forwardToEngine(std::size_t slot, const Core::ParsedFrame& frame);
  void noteAlive(Core::Stream::Value stream);
  void armStallWatch();
  void checkForStalls();

  // Even a stream carrying pure silence is finalised every few seconds, so total quiet
  // for this long is a broken connection rather than a quiet room. Deliberately far
  // above the longest plausible sentence, since a long sentence is not a stall.
  static constexpr qint64 kStallAfterMs = 20000;

  // How long an open stream may go without sending a single frame before it stops
  // holding the transcript back. A capture that is working sends its first frame within
  // one 32 ms render quantum of the stream opening, so this is a hundred and fifty times
  // the honest case — and it is a different question from kStallAfterMs, which asks how
  // long an engine that *is* being fed audio may stay silent.
  static constexpr qint64 kSilentStreamMs = 5000;
  void drainTranscript();
  void emitUtterance(const Core::Utterance& utterance);

  void sendReady();
  void rejectWith(const char* code, std::uint16_t closeCode, const QString& detail);
  void closeSession();
  void finishSession();
  void reportSession();
  void reportStream(std::size_t slot);

  bool tokenMatches(const QString& offered) const;

  /// A directory for this session that does not already hold one. The stamp names the
  /// second capture began in, and two sessions can share a second.
  QString sessionDirectory() const;

  static const char* streamKey(Core::Stream::Value stream);

  ServerConfig             cfg_;
  QWebSocketServer         server_;
  std::unique_ptr<Session> session_;
  std::uint64_t            nextSessionId_ = 0;

  // Reused across frames so a 31-per-second arrival rate allocates nothing.
  std::vector<std::int16_t> scratch_;
};

} } } // namespace DST::DESK::IO

#endif // DST_DESK_IO_WSSERVER_HPP
