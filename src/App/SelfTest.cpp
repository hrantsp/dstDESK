#include "SelfTest.hpp"

#include "Core/Protocol.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSslSocket>
#include <QTcpServer>
#include <QTextStream>
#include <QWebSocketServer>

namespace DST { namespace DESK { namespace App {
namespace {

CheckResult pass(QString name, QString detail                ) { return { std::move(name), true , std::move(detail),                {} }; }
CheckResult fail(QString name, QString detail, QString remedy) { return { std::move(name), false, std::move(detail), std::move(remedy) }; }

CheckResult checkQtRuntime()
{
  // Compiled-against versus loaded-at-runtime. A mismatch here is the classic
  // "it built on my machine" failure.
  const QString built   = QStringLiteral(QT_VERSION_STR);
  const QString running = QString::fromLatin1(qVersion());

  if (built != running)
  {
    return fail(QStringLiteral("Qt runtime"),
                QStringLiteral("built against %1, loaded %2").arg(built, running),
                QStringLiteral("A different Qt is ahead on the library path. "
                               "Run through `conan install` so the packaged Qt is "
                               "used."));
  }
  return pass(QStringLiteral("Qt runtime"), running);
}

CheckResult checkWebSockets()
{
  // Proves the WebSockets module loaded and that a server can actually bind,
  // rather than merely that the header was present at compile time.
  auto probe = QWebSocketServer(QStringLiteral("dstdesk-selftest"),
                                QWebSocketServer::NonSecureMode);

  if (!probe.listen(QHostAddress::LocalHost, 0))
  {
    return fail(QStringLiteral("WebSocket server"), probe.errorString(),
                QStringLiteral("Qt WebSockets could not bind a loopback port. "
                               "Check for a restrictive local firewall."));
  }

  const quint16 port = probe.serverPort();
  probe.close();
  return pass(QStringLiteral("WebSocket server"),
              QStringLiteral("bound ephemeral port %1").arg(port));
}

CheckResult checkTls()
{
  // The check this whole thing exists for. Qt's TLS support is a plugin that
  // dlopen()s OpenSSL by name; nothing at build time references it, so a machine
  // without a usable OpenSSL builds and starts perfectly and then fails on the
  // first connection to the transcription provider.
  if (!QSslSocket::supportsSsl())
  {
    return fail(QStringLiteral("TLS backend"),
                QStringLiteral("no TLS backend could be loaded (built against %1)")
                    .arg(QSslSocket::sslLibraryBuildVersionString()),
                QStringLiteral("Qt bundles no OpenSSL and loads the system one at "
                               "runtime. The OpenSSL that Conan provides must be on "
                               "the library path — run the app through `conanrun` or "
                               "the packaged launcher. See decision 12 in "
                               "dstOMNI/DESIGN.md."));
  }
  return pass(QStringLiteral("TLS backend"),
              QStringLiteral("%1 (built against %2)")
                  .arg(QSslSocket::sslLibraryVersionString(),
                       QSslSocket::sslLibraryBuildVersionString()));
}

CheckResult checkPort(std::uint16_t port)
{
  auto probe = QTcpServer{};

  if (!probe.listen(QHostAddress::LocalHost, port))
  {
    return fail(QStringLiteral("Configured port"),
                QStringLiteral("cannot bind 127.0.0.1:%1 — %2")
                    .arg(port)
                    .arg(probe.errorString()),
                QStringLiteral("Another process is using the port. Stop it, or set a "
                               "different port in the target config."));
  }

  probe.close();
  return pass(QStringLiteral("Configured port"),
              QStringLiteral("127.0.0.1:%1 is free").arg(port));
}

CheckResult checkOutputDir(const QString& dir)
{
  auto dd = QDir(dir);

  if (!dd.exists() && !dd.mkpath(QStringLiteral(".")))
  {
    return fail(QStringLiteral("Output directory"),
                QStringLiteral("cannot create %1").arg(dir),
                QStringLiteral("Choose a writable location with --output."));
  }

  const QString probePath = dd.filePath(QStringLiteral(".dstdesk-write-test"));
  auto probe = QFile(probePath);

  if (!probe.open(QIODevice::WriteOnly))
  {
    return fail(QStringLiteral("Output directory"),
                QStringLiteral("%1 is not writable").arg(dir),
                QStringLiteral("Choose a writable location with --output."));
  }

  probe.close();
  QFile::remove(probePath);
  return pass(QStringLiteral("Output directory"), QDir::toNativeSeparators(dd.path()));
}

CheckResult reportProtocol()
{
  // Not a pass/fail check — it prints the constants this binary was generated with,
  // which is what you want when a browser and a desktop build disagree.
  return pass(QStringLiteral("Protocol"),
              QStringLiteral("v%1, %2 Hz, %3 samples/frame, %4 B/frame")
                  .arg(Core::kVersion)
                  .arg(Core::kSampleRate)
                  .arg(Core::kFrameSamples)
                  .arg(Core::kFrameBytes));
}

} // namespace

bool SelfTestReport::ok() const
{
  for (const auto& cc : checks)
    if (!cc.passed) return false;
  return true;
}

SelfTestReport runSelfTest(std::uint16_t port, const QString& outputDir)
{
  auto report = SelfTestReport{};
  report.checks.push_back(reportProtocol());
  report.checks.push_back(checkQtRuntime());
  report.checks.push_back(checkWebSockets());
  report.checks.push_back(checkTls());
  report.checks.push_back(checkPort(port));
  report.checks.push_back(checkOutputDir(outputDir));
  return report;
}

int printSelfTest(const SelfTestReport& report)
{
  auto out = QTextStream(stdout);

  for (const auto& cc : report.checks)
    out << (cc.passed ? "  ok    " : "  FAIL  ") << cc.name.leftJustified(20) << cc.detail << Qt::endl;
  out << Qt::endl;

  if (report.ok())
  {
    out << "All checks passed." << Qt::endl;
    return 0;
  }

  out << "Failures:" << Qt::endl;
  for (const auto& cc : report.checks)
    if (!cc.passed) out << "  " << cc.name << ": " << cc.remedy << Qt::endl;
  return 1;
}

} } } // namespace DST::DESK::App
