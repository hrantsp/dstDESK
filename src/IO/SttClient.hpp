// One streaming transcription connection, for one audio stream.
//
// Audio is forwarded exactly as it arrived from the extension: 16 kHz mono signed
// 16-bit little-endian is precisely what the engine wants, so nothing is transcoded
// anywhere in this application. See decision 8 in dstOMNI/DESIGN.md.
//
// Times reported here are already on the capture clock shared by both streams. The
// engine measures from the first audio it received, so the offset is applied here
// rather than leaving every caller to remember it.

#ifndef DST_DESK_IO_STTCLIENT_HPP
#define DST_DESK_IO_STTCLIENT_HPP

#include <QByteArray>
#include <QObject>
#include <QString>
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

  /// `originSeconds` is where this stream's first forwarded sample sits on the shared
  /// capture clock; every reported time is offset by it.
  void start(double originSeconds);

  /// Raw payload bytes from a wire frame, forwarded untouched.
  void sendAudio(std::span<const std::byte> pcm);

  /// Asks the engine to finalise and close. Results continue briefly afterwards.
  void finish();

  bool isReady() const;
  bool isFinished() const { return finished_; }

signals:
  /// Settled text. Empty transcripts are emitted too: the engine finalises silence,
  /// and those still prove how far this stream has progressed, which is what advances
  /// the ordering watermark.
  void finalResult(double start, double end, QString text, double confidence);

  /// Text that may still change. Never commit it — finals have been observed both
  /// shorter than the interim before them and revised at the start.
  void interimResult(double start, QString text);

  void failed(QString reason);

  /// The engine has delivered everything it had and the connection is closed. Tearing
  /// a session down before this arrives loses the last utterance, which is the one
  /// most likely to matter.
  void finished();

private:
  void onConnected();
  void onTextMessage(const QString& message);
  void onDisconnected();

  QUrl buildUrl() const;

  SttConfig           cfg_;
  Core::Stream::Value stream_;
  QWebSocket          socket_;

  double origin_  = 0.0;
  bool   ready_   = false;
  bool   closing_  = false;
  bool   finished_ = false;

  // Audio that arrived before the connection was ready. Establishing it takes a few
  // hundred milliseconds, and discarding that audio would clip the first words of a
  // session — exactly the part someone is most likely to be listening for.
  QByteArray pending_;
  static constexpr int kMaxPendingBytes = 16000 * 2 * 3; // three seconds
};

} } } // namespace DST::DESK::IO

#endif // DST_DESK_IO_STTCLIENT_HPP
