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

  // Empty means "do not check". Both are documented in PROTOCOL.md §7 as guards
  // against stray local software connecting by accident, not as a defence against a
  // hostile process running as the same user.
  QString       token;
  QStringList   allowedOrigins;

  // Transcription follows the key: with one, audio is transcribed as well as
  // recorded; without one, the app still records. --no-transcribe forces it off.
  bool      transcribe = false;
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
    QWebSocket* socket    = nullptr;
    bool        helloDone = false;
    QString     client;
    double      contextEpochUtcMs = 0.0;
    QString     dir;

    std::array<Core::StreamRecorder, 2> recorders;
    std::array<bool, 2>                 opened = { false, false };

    // Created with the stream, but only connected once the first frame arrives: the
    // engine measures time from the first audio it receives, so the offset onto the
    // shared capture clock is not known until then.
    std::array<SttClient*, 2> stt     = { nullptr, nullptr };
    std::array<bool, 2>       sttOpen = { false, false };

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

  void startTranscription(Core::Stream::Value stream, std::uint32_t firstSampleIndex);
  void noteAlive(Core::Stream::Value stream);
  void checkForStalls();

  // Even a stream carrying pure silence is finalised every few seconds, so total quiet
  // for this long is a broken connection rather than a quiet room. Deliberately far
  // above the longest plausible sentence, since a long sentence is not a stall.
  static constexpr qint64 kStallAfterMs = 20000;
  void drainTranscript();
  void emitUtterance(const Core::Utterance& utterance);

  void sendReady();
  void rejectWith(const char* code, std::uint16_t closeCode, const QString& detail);
  void closeSession();
  void finishSession();
  void reportSession() const;

  bool tokenMatches(const QString& offered) const;
  static const char* streamKey(Core::Stream::Value stream);

  ServerConfig             cfg_;
  QWebSocketServer         server_;
  std::unique_ptr<Session> session_;

  // Reused across frames so a 31-per-second arrival rate allocates nothing.
  std::vector<std::int16_t> scratch_;
};

} } } // namespace DST::DESK::IO

#endif // DST_DESK_IO_WSSERVER_HPP
