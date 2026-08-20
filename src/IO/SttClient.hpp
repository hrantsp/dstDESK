// One streaming transcription connection, for one audio stream.
//
// Audio is forwarded exactly as it arrived from the extension: 16 kHz mono signed
// 16-bit little-endian is precisely what the engine wants, so nothing is transcoded
// anywhere in this application. See decision 8 in dstOMNI/DESIGN.md.
//
// This class owns the mapping between two clocks, and that is most of what it does.
// The engine times its results from the audio it has received; everything else in this
// application works on the capture clock shared by both streams. So every chunk of
// audio arrives here stamped with where it sits on the shared clock, and this class
// keeps the two aligned — inserting silence where frames were lost, and re-basing when
// a dropped connection is replaced. Callers never see engine time.

#ifndef DST_DESK_IO_STTCLIENT_HPP
#define DST_DESK_IO_STTCLIENT_HPP

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <cstddef>
#include <span>
#include "Core/Protocol.hpp"

namespace DST { namespace DESK { namespace IO {

struct SttConfig
{
  QString apiKey;
  QString model    = QStringLiteral("nova-3");
  bool    diarize  = false;
  QUrl    endpoint = QUrl(QStringLiteral("wss://api.deepgram.com/v1/listen"));
};

class SttClient : public QObject
{
  Q_OBJECT

public:
  SttClient(SttConfig cfg, Core::Stream::Value stream, QObject* parent = nullptr);

  void start();

  /// Raw payload bytes from a wire frame, forwarded untouched, with the position of
  /// their first sample on the shared capture clock. The position is what keeps the
  /// engine's own clock aligned: a gap is padded with silence, and a reconnection
  /// re-bases from here rather than pretending the outage did not happen.
  void sendAudio(std::span<const std::byte> pcm, double positionSeconds);

  /// Asks the engine to finalise and close. Results continue briefly afterwards.
  void finish();

  bool isReady() const;
  bool isFinished() const { return finished_; }

  /// True when this connection stopped for good rather than being asked to stop —
  /// the key was refused, or reconnection was tried and did not take. The distinction
  /// matters to the caller: a client that was asked to finish should be replaced when
  /// the stream reopens, and one that gave up should not, or it retries forever.
  bool gaveUp() const { return gaveUp_; }

signals:
  /// Settled text. Empty transcripts are emitted too: the engine finalises silence,
  /// and those still prove how far this stream has progressed, which is what advances
  /// the ordering watermark.
  void finalResult(double start, double end, QString text, double confidence);

  /// Text that may still change. Never commit it — finals have been observed both
  /// shorter than the interim before them and revised at the start.
  void interimResult(double start, QString text);

  /// The connection dropped and is being retried. The stream is not finished: it will
  /// speak again if a retry takes. Callers should stop letting it hold the transcript
  /// back without treating it as closed, because it may yet produce text.
  void interrupted(QString reason);

  /// Reconnected and transcribing again, after `lostSeconds` of audio that no engine
  /// ever saw.
  void resumed(double lostSeconds);

  void failed(QString reason);

  /// The engine has delivered everything it had and the connection is closed for good.
  /// Tearing a session down before this arrives loses the last utterance, which is the
  /// one most likely to matter.
  void finished();

private:
  void onConnected();
  void onTextMessage(const QString& message);
  void onDisconnected();

  void openSocket();
  void scheduleRetry();
  void giveUp(const QString& reason);
  void padTo(double positionSeconds);

  QUrl buildUrl() const;

  /// 401 and 403 are answers, not accidents: the key is wrong or the model is refused.
  /// Retrying them costs time and tells the user nothing new.
  static bool retryable(const QString& detail);

  SttConfig           cfg_;
  Core::Stream::Value stream_;
  QWebSocket          socket_;
  QTimer              retry_;

  // Where this connection's engine-time zero sits on the shared capture clock, and
  // where the next sample is expected. Both are re-established on every connection,
  // because a new socket starts the engine's clock again from nothing.
  double origin_     = 0.0;
  double expected_   = 0.0;
  bool   needOrigin_ = true;

  // Where the audio waiting in `pending_` begins. The engine's clock starts at the
  // first byte it receives, and that is the front of this buffer rather than whatever
  // frame happens to arrive after the socket comes up — so taking the origin from the
  // latter puts every result on the reconnected stream late by however long the outage
  // lasted, which is precisely the error a reconnection exists to avoid.
  double pendingOrigin_ = 0.0;

  // Set while a retry is outstanding, so resumption can be announced once when it
  // actually happens rather than guessed at from the retry count.
  bool   interrupted_ = false;

  bool   ready_    = false;
  bool   closing_  = false;
  bool   finished_ = false;
  bool   gaveUp_   = false;

  int          attempts_ = 0;
  QElapsedTimer connected_;   // how long the current connection has been up
  double  lost_     = 0.0; // seconds of audio no connection was up to receive
  QString lastError_;

  // Retries, and the backoff between them. Bounded because a connection that will not
  // come back should say so rather than trying quietly for the rest of the meeting.
  static constexpr int kMaxAttempts   = 5;
  static constexpr int kRetryBaseMs   = 500;
  static constexpr int kRetryCeilMs   = 10000;

  // How long a connection must last before the retry budget is considered spent well
  // and starts over. Judged on duration rather than on having produced a result, which
  // was the first attempt and was wrong: a connection that comes up, delivers a second
  // of transcript and dies has "worked" by that measure, so a link flapping once a
  // second reset the budget every time and reconnected forever — measured at 34 sessions
  // in 28 seconds, each one a new billable connection.
  static constexpr qint64 kHealthyMs = kRetryCeilMs;

  // The largest gap that will be filled with silence rather than re-based. Matches the
  // recorder's bound for the same reason: the position arrives from the client, and
  // padding writes the difference. See PROTOCOL.md §5.4.
  static constexpr double kMaxPadSeconds = 30.0;

  // Audio that arrived before the connection was ready. Establishing it takes a few
  // hundred milliseconds, and discarding that audio would clip the first words of a
  // session — exactly the part someone is most likely to be listening for.
  QByteArray pending_;
  static constexpr int kMaxPendingBytes = 16000 * 2 * 3; // three seconds
};

} } } // namespace DST::DESK::IO

#endif // DST_DESK_IO_STTCLIENT_HPP
