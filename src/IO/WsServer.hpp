// The extension-facing WebSocket server.
//
// Binds loopback, validates the handshake described in rec/PROTOCOL.md §3-4, and
// routes binary frames into one StreamRecorder per stream. This is the Qt-facing
// half; everything it decides with lives in Core and is tested without an event loop.

#ifndef DST_DESK_IO_WSSERVER_HPP
#define DST_DESK_IO_WSSERVER_HPP

#include <QObject>
#include <QString>
#include <QStringList>
#include <QWebSocketServer>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "Core/Frame.hpp"
#include "Core/StreamRecorder.hpp"

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
};

class WsServer : public QObject
{
  Q_OBJECT

public:
  explicit WsServer(ServerConfig cfg, QObject* parent = nullptr);
  ~WsServer() override;

  bool          start();
  std::uint16_t port() const;

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
  };

  void onNewConnection();
  void onTextMessage  (const QString&    message);
  void onBinaryMessage(const QByteArray& message);
  void onDisconnected ();

  void handleHello      (const QJsonObject& msg);
  void handleStreamOpen (const QJsonObject& msg);
  void handleStreamClose(const QJsonObject& msg);
  void handleBye        ();

  void sendReady();
  void rejectWith(const char* code, std::uint16_t closeCode, const QString& detail);
  void closeSession();
  void reportSession() const;

  static const char* streamKey(Core::Stream::Value stream);

  ServerConfig             cfg_;
  QWebSocketServer         server_;
  std::unique_ptr<Session> session_;

  // Reused across frames so a 31-per-second arrival rate allocates nothing.
  std::vector<std::int16_t> scratch_;
};

} } } // namespace DST::DESK::IO

#endif // DST_DESK_IO_WSSERVER_HPP
