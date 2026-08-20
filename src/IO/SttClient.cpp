#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QtGlobal>
#include <algorithm>
#include <vector>
#include "SttClient.hpp"

namespace DST { namespace DESK { namespace IO {

SttClient::SttClient(SttConfig cfg, Core::Stream::Value stream, QObject* parent)
  : QObject(parent)
  , cfg_(std::move(cfg))
  , stream_(stream)
{
  connect(&socket_, &QWebSocket::connected,           this, &SttClient::onConnected);
  connect(&socket_, &QWebSocket::textMessageReceived, this, &SttClient::onTextMessage);
  connect(&socket_, &QWebSocket::disconnected,        this, &SttClient::onDisconnected);

  retry_.setSingleShot(true);
  connect(&retry_, &QTimer::timeout, this, &SttClient::openSocket);

  connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError)
  {
    // Asking the engine to close and then seeing the socket close is not a fault;
    // reporting it would train the reader to ignore genuine failures.
    if (closing_) return;

    // Qt reports a rejected upgrade as "Unhandled http status code", which gives the
    // number and nothing that helps. The two that actually happen have precise causes,
    // and each reads like the other at first glance: 401 is the key, 403 is the model.
    // An empty model is refused as a project-permissions error, which sends the reader
    // to their account page instead of to their settings.
    auto detail = socket_.errorString();
    if      (detail.contains(QStringLiteral("401"))) detail += QStringLiteral(" — the API key was rejected.");
    else if (detail.contains(QStringLiteral("403"))) detail += QStringLiteral(" — the key is valid but the model '%1' was refused.")
                    .arg(cfg_.model.isEmpty() ? QStringLiteral("(empty)") : cfg_.model);

    // Recorded rather than announced. Whether this is worth telling the user depends on
    // what happens next — a blip that reconnects is not news — so the decision belongs
    // in onDisconnected, which knows.
    lastError_ = detail;
  });
}

bool SttClient::retryable(const QString& detail)
{
  return !detail.contains(QStringLiteral("401")) && !detail.contains(QStringLiteral("403"));
}

QUrl SttClient::buildUrl() const
{
  auto url   = cfg_.endpoint;
  auto query = QUrlQuery{};

  // linear16 at 16 kHz mono is what the wire already carries, so the audio passes
  // through this application without being touched.
  query.addQueryItem(QStringLiteral("model"),           cfg_.model);
  query.addQueryItem(QStringLiteral("encoding"),        QStringLiteral("linear16"));
  query.addQueryItem(QStringLiteral("sample_rate"),     QString::number(Core::kSampleRate));
  query.addQueryItem(QStringLiteral("channels"),        QStringLiteral("1"));
  query.addQueryItem(QStringLiteral("interim_results"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("punctuate"),       QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("smart_format"),    QStringLiteral("true"));

  // HP:TODO: this is requested and then ignored. With diarisation on, every word in the
  // response carries a speaker index, and onTextMessage reads only the transcript text and
  // the confidences — so the option costs a query parameter and changes nothing visible.
  // Finishing it means carrying the index on Utterance and showing it beside the meeting
  // label. Recorded as a known limit rather than quietly left; decision 14 has the state.
  if (cfg_.diarize) query.addQueryItem(QStringLiteral("diarize"), QStringLiteral("true"));

  url.setQuery(query);
  return url;
}

void SttClient::start() { openSocket(); }

void SttClient::openSocket()
{
  if (closing_ || finished_) return;

  // Every connection starts the engine's clock again from zero, so the mapping onto the
  // shared clock has to be re-established rather than carried over. Forgetting this is
  // how a reconnected stream reports times from before the outage and lands in the
  // transcript ahead of text that preceded it.
  //
  // Whatever is buffered is kept, not discarded: it is the audio from the outage, it is
  // what the reconnected engine should hear first, and its front is where the new
  // connection's clock will start.
  needOrigin_ = true;

  // The padding budget belongs to a connection, not to the client: a new socket is a new
  // upload, and carrying a spent budget across would leave a reconnected stream unable to
  // cover the first drop after it.
  padded_ = 0.0;
  sent_   = 0.0;

  auto request = QNetworkRequest(buildUrl());
  request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(cfg_.apiKey).toUtf8());

  socket_.open(request);
}

void SttClient::onConnected()
{
  ready_ = true;
  connected_.start();

  if (!pending_.isEmpty())
  {
    // The origin is the front of this buffer, established before it is sent. Setting it
    // from the next frame instead would date every result on this connection by the
    // length of what was buffered.
    origin_     = pendingOrigin_;
    expected_   = pendingOrigin_ + double(pending_.size() / 2) / double(Core::kSampleRate);
    needOrigin_ = false;

    socket_.sendBinaryMessage(pending_);
    pending_.clear();
    pending_.squeeze();

    if (interrupted_)
    {
      interrupted_ = false;
      emit resumed(lost_);
      lost_ = 0.0;
    }
  }
}

bool SttClient::isReady() const { return ready_; }

void SttClient::padTo(double positionSeconds)
{
  const double missing = positionSeconds - expected_;
  if (missing <= 0.0) return;

  if (missing > kMaxPadSeconds || padded_ + missing > maxTotalPadSeconds())
  {
    // Beyond anything a lost frame explains. Re-base instead of manufacturing minutes
    // of silence: the position arrives from the client, and padding writes the
    // difference, so an unchecked one turns a bad number into an unbounded upload.
    qWarning("Gap of %.1f s on %s is too large to pad; re-basing the engine clock",
             missing, Core::Stream::label(stream_));
    origin_   += missing;
    expected_  = positionSeconds;
    return;
  }

  // Silence in the same 16 kHz mono PCM16 the wire carries, in frame-sized pieces so
  // nothing here allocates in proportion to the gap.
  static const auto kSilence = QByteArray(int(Core::kFrameSamples) * 2, '\0');

  auto remaining = static_cast<qint64>(missing * Core::kSampleRate);
  while (remaining > 0)
  {
    const auto chunk = static_cast<int>(std::min<qint64>(remaining, Core::kFrameSamples));
    socket_.sendBinaryMessage(QByteArray(kSilence.constData(), chunk * 2));
    remaining -= chunk;
  }
  padded_  += missing;
  expected_ = positionSeconds;
}

void SttClient::sendAudio(std::span<const std::byte> pcm, double positionSeconds)
{
  if (closing_ || finished_) return;

  const auto bytes    = QByteArray(reinterpret_cast<const char*>(pcm.data()),
                                   static_cast<qsizetype>(pcm.size()));
  const double seconds = double(pcm.size() / 2) / double(Core::kSampleRate);

  if (!ready_)
  {
    // Not connected: either still opening, or between retries. Buffer a little so the
    // opening words survive the handshake, and count the rest as lost rather than
    // letting it grow — three seconds of audio is worth keeping, three minutes is not.
    if (pending_.isEmpty()) pendingOrigin_ = positionSeconds;

    if (pending_.size() + bytes.size() <= kMaxPendingBytes) pending_.append(bytes);
    else                                                    lost_ += seconds;
    return;
  }

  if (needOrigin_)
  {
    // This connection's engine-time zero is here. Everything it reports is offset by
    // this, so the transcript stays on the one clock both streams share.
    origin_     = positionSeconds;
    expected_   = positionSeconds;
    needOrigin_ = false;

    if (interrupted_)
    {
      interrupted_ = false;
      emit resumed(lost_);
      lost_ = 0.0;
    }
  }
  else
  {
    padTo(positionSeconds);
  }

  socket_.sendBinaryMessage(bytes);
  sent_    += seconds;
  expected_ = positionSeconds + seconds;
}

void SttClient::finish()
{
  if (closing_) return;
  closing_ = true;
  retry_.stop();

  if (ready_)
  {
    // Asks the engine to finalise what it is holding. Closing the socket outright
    // would discard the last utterance, which is usually the one that matters.
    socket_.sendTextMessage(QStringLiteral(R"({"type":"CloseStream"})"));
  }
  else
  {
    // Never connected, so there is nothing to finalise and nothing to wait for.
    socket_.abort();
    if (!finished_) { finished_ = true; emit finished(); }
  }
}

void SttClient::onTextMessage(const QString& message)
{
  const auto doc = QJsonDocument::fromJson(message.toUtf8());
  if (!doc.isObject()) return;

  const auto msg  = doc.object();
  const auto type = msg.value(QStringLiteral("type")).toString();

  if (type == QStringLiteral("Metadata"))
  {
    socket_.close();
    return;
  }

  if (type != QStringLiteral("Results")) return; // SpeechStarted, UtteranceEnd: unused

  const auto channel = msg.value(QStringLiteral("channel")).toObject();
  const auto alts    = channel.value(QStringLiteral("alternatives")).toArray();
  if (alts.isEmpty()) return;

  const auto   alt      = alts.first().toObject();
  const auto   text     = alt.value(QStringLiteral("transcript")).toString();
  const double start    = origin_ + msg.value(QStringLiteral("start")).toDouble();
  const double duration = msg.value(QStringLiteral("duration")).toDouble();

  if (!msg.value(QStringLiteral("is_final")).toBool())
  {
    if (!text.isEmpty()) emit interimResult(start, text);
    return;
  }

  double confidence = 0.0;
  const auto words = alt.value(QStringLiteral("words")).toArray();
  if (!words.isEmpty())
  {
    for (const auto& word : words)
      confidence += word.toObject().value(QStringLiteral("confidence")).toDouble();
    confidence /= words.size();
  }

  // Empty finals are forwarded deliberately: they carry no text but do carry how far
  // this stream has been finalised, which is what lets the merger commit safely.
  emit finalResult(start, start + duration, text, confidence);
}

void SttClient::scheduleRetry()
{
  // Exponential, capped. A transcription service that is briefly unreachable should be
  // retried quickly; one that is down for the meeting should not be hammered.
  const auto delay = std::min(kRetryCeilMs, kRetryBaseMs * (1 << std::min(attempts_, 5)));
  ++attempts_;

  qInfo("Transcription for %s dropped; retry %d of %d in %d ms",
        Core::Stream::label(stream_), attempts_, kMaxAttempts, delay);

  retry_.start(delay);
}

void SttClient::giveUp(const QString& reason)
{
  gaveUp_   = true;
  finished_ = true;
  emit failed(reason);
  emit finished();
}

void SttClient::onDisconnected()
{
  ready_ = false;
  if (finished_) return;

  // Asked to stop: this is the ordinary end of a session.
  if (closing_)
  {
    finished_ = true;
    emit finished();
    return;
  }

  // A connection that stayed up is proof the link works, so the retry budget starts
  // over — otherwise the first few blips of a long meeting spend the whole budget and
  // the one that matters gets no retries at all. Judged on how long it lasted, not on
  // whether it produced anything: see kHealthyMs.
  if (connected_.isValid() && connected_.elapsed() >= kHealthyMs) attempts_ = 0;

  // Not asked to stop. Until this existed, an unexpected drop was indistinguishable
  // from a clean finish: the stream was closed in the merger and never transcribed
  // again for the rest of the meeting, while audio kept arriving and being recorded —
  // so the recording was complete and half the transcript simply stopped, with nothing
  // said about it.
  const auto reason = lastError_.isEmpty()
                          ? QStringLiteral("the connection closed unexpectedly")
                          : lastError_;

  if (!retryable(reason))
  {
    giveUp(reason);
    return;
  }

  if (attempts_ >= kMaxAttempts)
  {
    giveUp(QStringLiteral("%1 (gave up after %2 attempts)").arg(reason).arg(kMaxAttempts));
    return;
  }

  interrupted_ = true;
  emit interrupted(reason);
  scheduleRetry();
}

} } } // namespace DST::DESK::IO
