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
//
// A check is only fatal to the feature that needs it. TLS is the clearest case: it is
// wanted for exactly one connection, the one to the transcription service, and that
// connection is optional. The local link the extension uses is plain ws:// on loopback
// and needs no TLS at all — so a machine with no usable OpenSSL can record perfectly
// well, and refusing to start it was refusing to do the half of the job that still
// worked. The same logic applies to the output directory, which only matters when
// something is going to be written to it.

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

  // Whether failing this one should stop the application. False for a precondition of
  // a feature that is switched off, where the honest report is a warning and a smaller
  // application rather than a refusal to start.
  bool    blocking = true;
};

struct SelfTestReport
{
  QList<CheckResult> checks;
  bool ok() const;
};

/// What the run is going to do, so each check can be judged against it rather than
/// against an imagined worst case.
struct SelfTestIntent
{
  bool willRecord     = true;
  bool willTranscribe = true;
};

// `port` and `outputDir` are checked against the configuration the app would actually
// run with, so the report reflects this machine and this setup rather than defaults.
SelfTestReport runSelfTest(std::uint16_t port, const QString& outputDir,
                           SelfTestIntent intent = {});

// Human-readable report. Returns the process exit code: 0 when everything passed.
int printSelfTest(const SelfTestReport& report);

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_SELFTEST_HPP
