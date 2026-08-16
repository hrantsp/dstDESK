// Runtime environment checks, exposed as `dstdesk --selftest`.
//
// A successful compile proves the headers matched. It says nothing about what will
// exist at runtime, and Qt in particular loads code by string name that no build step
// ever sees — its TLS support is a plugin that dlopen()s the system OpenSSL the first
// time a secure connection is opened. That failure would otherwise surface mid-call,
// not at startup.
//
// Each check reports what to do about a failure, so the output is actionable rather
// than merely informative.

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace dst {

struct CheckResult {
    QString name;
    bool passed = false;
    QString detail;   // what was observed
    QString remedy;   // what to do when it fails; empty when it passed
};

struct SelfTestReport {
    QList<CheckResult> checks;
    bool ok() const;
};

// `port` and `outputDir` are checked against the configuration the app would actually
// run with, so the report reflects this machine and this setup rather than defaults.
SelfTestReport runSelfTest(std::uint16_t port, const QString& outputDir);

// Human-readable report. Returns the process exit code: 0 when everything passed.
int printSelfTest(const SelfTestReport& report);

}  // namespace dst
