#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QtGlobal>
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

  connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError)
  {
    // Asking the engine to close and then seeing the socket close is not a fault;
    // reporting it would train the reader to ignore genuine failures.
    if (closing_) return;
    emit failed(socket_.errorString());
  });
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

  if (cfg_.diarize) query.addQueryItem(QStringLiteral("diarize"), QStringLiteral("true"));

  url.setQuery(query);
  return url;
}

void SttClient::start(double originSeconds)
{
  origin_ = originSeconds;

  auto request = QNetworkRequest(buildUrl());
  request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(cfg_.apiKey).toUtf8());

  socket_.open(request);
}

void SttClient::onConnected()
{
  ready_ = true;

  if (!pending_.isEmpty())
  {
    socket_.sendBinaryMessage(pending_);
    pending_.clear();
    pending_.squeeze();
  }
}

bool SttClient::isReady() const { return ready_; }

void SttClient::sendAudio(std::span<const std::byte> pcm)
{
  if (closing_) return;

  const auto bytes = QByteArray(reinterpret_cast<const char*>(pcm.data()),
                                static_cast<qsizetype>(pcm.size()));

  if (!ready_)
  {
    // Bounded: if the connection never comes up, this must not grow without limit.
    if (pending_.size() < kMaxPendingBytes) pending_.append(bytes);
    return;
  }

  socket_.sendBinaryMessage(bytes);
}

void SttClient::finish()
{
  if (closing_) return;
  closing_ = true;

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

void SttClient::onDisconnected()
{
  ready_ = false;
  if (finished_) return;
  finished_ = true;
  emit finished();
}

} } } // namespace DST::DESK::IO
