#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTextStream>

#include "app/selftest.h"
#include "protocol_generated.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("dstdesk"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Receives microphone and meeting audio from the dstORCH "
                       "browser extension."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption selfTestOption(
        QStringLiteral("selftest"),
        QStringLiteral("Check that this machine can actually run the app — Qt, "
                       "WebSockets, TLS, the port and the output directory — then "
                       "exit."));
    const QCommandLineOption portOption(
        QStringLiteral("port"), QStringLiteral("Loopback port to listen on."),
        QStringLiteral("port"), QString::number(dst::protocol::kDefaultPort));
    const QCommandLineOption outputOption(
        QStringLiteral("output"), QStringLiteral("Directory for recorded audio."),
        QStringLiteral("dir"),
        QDir::current().filePath(QStringLiteral("out")));

    parser.addOption(selfTestOption);
    parser.addOption(portOption);
    parser.addOption(outputOption);
    parser.process(app);

    bool portOk = false;
    const uint port = parser.value(portOption).toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) {
        QTextStream(stderr) << "Invalid --port: " << parser.value(portOption)
                            << Qt::endl;
        return 2;
    }
    const QString outputDir = parser.value(outputOption);

    if (parser.isSet(selfTestOption)) {
        return dst::printSelfTest(
            dst::runSelfTest(static_cast<quint16>(port), outputDir));
    }

    // The self-test runs before serving too. Its checks are the ones that would
    // otherwise fail silently mid-session rather than at startup.
    const auto report = dst::runSelfTest(static_cast<quint16>(port), outputDir);
    if (!report.ok()) {
        QTextStream(stderr)
            << "Refusing to start — this machine is not ready:" << Qt::endl;
        dst::printSelfTest(report);
        return 1;
    }

    QTextStream(stdout) << "Environment ready. The WebSocket server is not "
                           "implemented yet."
                        << Qt::endl;
    return 0;
}
