#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <cmath>
#include <numbers>
#include <vector>
#include "Core/Endian.hpp"
#include "Core/Protocol.hpp"
#include "Simulate.hpp"

namespace DST { namespace DESK { namespace Sim {
namespace {

constexpr double kMicHertz  = 440.0; // A4
constexpr double kTabHertz  = 660.0; // E5 — a fifth up, unmistakably different by ear
constexpr double kAmplitude = 0.30;

// Frames carry a sampleIndex drawn from a clock shared by both streams, which has
// normally been running before either stream opens. Starting at a non-zero value
// keeps the simulation honest about that.
constexpr std::uint32_t kStartIndex = Core::kSampleRate;

QByteArray buildFrame(Core::Stream::Value stream, std::uint32_t sampleIndex, double hertz)
{
  auto frame = QByteArray(int(Core::kFrameBytes), Qt::Uninitialized);
  auto* pp   = reinterpret_cast<std::byte*>(frame.data());

  pp[Core::kOffsetVersion] = static_cast<std::byte>(Core::kVersion);
  pp[Core::kOffsetStream ] = static_cast<std::byte>(stream);
  Core::writeU16(pp + Core::kOffsetFrameSamples, Core::kFrameSamples);
  Core::writeU32(pp + Core::kOffsetSampleIndex,  sampleIndex);
  Core::writeU32(pp + Core::kOffsetFlags,        0);

  // Phase is derived from the absolute sample position, so a dropped frame leaves a
  // hole rather than a discontinuity — exactly what the real capture path does.
  for (std::uint32_t ii = 0; ii < Core::kFrameSamples; ++ii)
  {
    const double tt    = double(sampleIndex + ii) / double(Core::kSampleRate);
    const double value = std::sin(2.0 * std::numbers::pi * hertz * tt) * kAmplitude;
    Core::writeU16(pp + Core::kHeaderBytes + ii * 2,
                   static_cast<std::uint16_t>(static_cast<std::int16_t>(value * 32767.0)));
  }
  return frame;
}

QString encode(const QJsonObject& msg)
{
  return QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

class Simulator
{
public:
  Simulator(const SimulateConfig& cfg, QCoreApplication& app)
    : cfg_(cfg)
    , app_(app)
  {
    const std::uint32_t total = std::uint32_t(cfg_.seconds * Core::kSampleRate);
    frameCount_ = total / Core::kFrameSamples;

    if (cfg_.injectGap)
    {
      gapFrom_ = frameCount_ / 2;
      gapTo_   = gapFrom_ + 10; // ~320 ms of meeting audio never sent
    }

    QObject::connect(&socket_, &QWebSocket::connected,           [this]                     { onConnected();    });
    QObject::connect(&socket_, &QWebSocket::textMessageReceived, [this] (const QString& mm) { onText(mm);       });
    QObject::connect(&socket_, &QWebSocket::errorOccurred,       [this]                     { onError();        });
    QObject::connect(&socket_, &QWebSocket::disconnected,        [this]                     { onDisconnected(); });
    QObject::connect(&timer_,  &QTimer::timeout,                 [this]                     { onTick();         });

    // Without this the simulation would run forever if the server never spoke and
    // never closed. A hung diagnostic is worse than a failing one.
    watchdog_.setSingleShot(true);
    QObject::connect(&watchdog_, &QTimer::timeout, [this] { onWatchdog(); });
    watchdog_.start(int((cfg_.seconds + 10.0) * 1000));
  }

  int run()
  {
    const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(cfg_.port));
    out() << "Connecting to " << url.toString() << Qt::endl;
    socket_.open(url);
    return app_.exec();
  }

private:
  static QTextStream& out()
  {
    static QTextStream stream(stdout);
    return stream;
  }

  void onConnected()
  {
    connected_ = true;

    auto hello = QJsonObject{};
    hello[QStringLiteral("type")]         = QStringLiteral("hello");
    hello[QStringLiteral("protocol")]     = int(Core::kVersion);
    hello[QStringLiteral("sampleRate")]   = int(Core::kSampleRate);
    hello[QStringLiteral("frameSamples")] = int(Core::kFrameSamples);
    hello[QStringLiteral("client")]       = QStringLiteral("dstdesk --simulate");
    hello[QStringLiteral("contextEpochUtcMs")] = double(QDateTime::currentMSecsSinceEpoch());
    if (!cfg_.token.isEmpty()) hello[QStringLiteral("token")] = cfg_.token;

    socket_.sendTextMessage(encode(hello));
  }

  void onText(const QString& message)
  {
    const auto msg  = QJsonDocument::fromJson(message.toUtf8()).object();
    const auto type = msg.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("error"))
    {
      reportFailure(QStringLiteral("Server rejected the session: %1 — %2")
                        .arg(msg.value(QStringLiteral("code")).toString(),
                             msg.value(QStringLiteral("message")).toString()));
      return;
    }

    if (type != QStringLiteral("ready")) return;

    for (const auto stream : { Core::Stream::Mic, Core::Stream::Tab })
    {
      auto open = QJsonObject{};
      open[QStringLiteral("type")]   = QStringLiteral("stream-open");
      open[QStringLiteral("stream")] = int(stream);
      open[QStringLiteral("label")]  = QString::fromLatin1(Core::Stream::label(stream));
      socket_.sendTextMessage(encode(open));
    }

    out() << "Sending " << frameCount_ << " frames per stream (" << cfg_.seconds << " s)";
    if (cfg_.injectGap) out() << ", dropping meeting frames " << gapFrom_ << "-" << gapTo_;
    out() << Qt::endl;

    timer_.start(int(Core::kFrameMillis));
  }

  void onTick()
  {
    if (sent_ >= frameCount_)
    {
      finish();
      return;
    }

    const std::uint32_t index = kStartIndex + sent_ * Core::kFrameSamples;

    socket_.sendBinaryMessage(buildFrame(Core::Stream::Mic, index, kMicHertz));

    const bool inGap = cfg_.injectGap && sent_ >= gapFrom_ && sent_ < gapTo_;
    if (!inGap) socket_.sendBinaryMessage(buildFrame(Core::Stream::Tab, index, kTabHertz));

    ++sent_;
  }

  void finish()
  {
    timer_.stop();
    finished_ = true;

    for (const auto stream : { Core::Stream::Mic, Core::Stream::Tab })
    {
      auto close = QJsonObject{};
      close[QStringLiteral("type")]   = QStringLiteral("stream-close");
      close[QStringLiteral("stream")] = int(stream);
      close[QStringLiteral("reason")] = QStringLiteral("user-stopped");
      socket_.sendTextMessage(encode(close));
    }

    auto bye = QJsonObject{};
    bye[QStringLiteral("type")] = QStringLiteral("bye");
    socket_.sendTextMessage(encode(bye));

    out() << "Sent " << sent_ << " frames per stream. Done." << Qt::endl;

    // Give the socket a moment to flush before the event loop stops.
    QTimer::singleShot(200, &app_, [this] { socket_.close(); app_.quit(); });
  }

  // Every failure path ends here, and only the first one to arrive speaks: a refused
  // connection raises errorOccurred and disconnected together, and reporting both
  // reads as two unrelated faults.
  void reportFailure(const QString& text)
  {
    if (finished_ || reported_) return;
    reported_ = true;
    timer_.stop();
    out() << text << Qt::endl;
    exit_ = 1;
    app_.quit();
  }

  QString unreachable() const
  {
    return QStringLiteral("Could not reach ws://127.0.0.1:%1 — %2\n"
                          "Is dstdesk running? Start it in another terminal first.")
        .arg(cfg_.port)
        .arg(socket_.errorString());
  }

  void onError()
  {
    // A server that is running and turns us away is a different fault from nothing
    // listening at all, and needs a different fix, so they get different messages.
    reportFailure(connected_ || socket_.error() != QAbstractSocket::ConnectionRefusedError
                      ? QStringLiteral("Connection failed: %1").arg(socket_.errorString())
                      : unreachable());
  }

  void onDisconnected()
  {
    // A server may close without an error frame — a refused upgrade looks exactly
    // like this. Reporting here is what keeps the simulation from hanging. When the
    // socket never opened, disconnected can arrive before errorOccurred, so this
    // must not claim a server closed something it never accepted.
    if (!connected_)
    {
      reportFailure(unreachable());
      return;
    }

    reportFailure(QStringLiteral("Server closed the connection after %1 frames%2")
                      .arg(sent_)
                      .arg(socket_.closeReason().isEmpty()
                               ? QString()
                               : QStringLiteral(" (%1)").arg(socket_.closeReason())));
  }

  void onWatchdog()
  {
    reportFailure(QStringLiteral("Timed out with no response from the server."));
  }

  SimulateConfig    cfg_;
  QCoreApplication& app_;
  QWebSocket        socket_;
  QTimer            timer_;
  QTimer            watchdog_;
  bool              finished_ = false;
  bool              reported_ = false;
  bool              connected_ = false;

  std::uint32_t frameCount_ = 0;
  std::uint32_t sent_       = 0;
  std::uint32_t gapFrom_    = 0;
  std::uint32_t gapTo_      = 0;

public:
  int exit_ = 0;
};

} // namespace

int runSimulation(const SimulateConfig& cfg)
{
  auto* app = QCoreApplication::instance();
  auto  sim = Simulator(cfg, *app);
  const int rc = sim.run();
  return sim.exit_ != 0 ? sim.exit_ : rc;
}

} } } // namespace DST::DESK::Sim
