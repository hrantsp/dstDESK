// Runtime environment checks, exposed as `kobayashi --selftest`.
//
// Not a unit test: this ships inside the application and runs before it serves. A
// successful compile proves the headers matched, and says nothing about what will
// exist at runtime. Qt in particular loads code by string name that no build step
// ever sees — its TLS support is a plugin that dlopen()s the system OpenSSL the first
// time a secure connection is opened. That failure would otherwise surface mid-call
// rather than at startup.
//
// Each check reports what to do about a failure, so the output is actionable rather
// than merely informative.

#ifndef DST_DESK_APP_SELFTEST_HPP
#define DST_DESK_APP_SELFTEST_HPP

#include <QList>
#include <QString>
#include <cstdint>

namespace DST { namespace DESK { namespace App {

struct CheckResult
{
  QString name;
  bool    passed = false;
  QString detail; // what was observed
  QString remedy; // what to do when it fails; empty when it passed
};

struct SelfTestReport
{
  QList<CheckResult> checks;
  bool ok() const;
};

// `port` and `outputDir` are checked against the configuration the app would actually
// run with, so the report reflects this machine and this setup rather than defaults.
SelfTestReport runSelfTest(std::uint16_t port, const QString& outputDir);

// Human-readable report. Returns the process exit code: 0 when everything passed.
int printSelfTest(const SelfTestReport& report);

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_SELFTEST_HPP
