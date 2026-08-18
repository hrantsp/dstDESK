// Stored configuration.
//
// A double-clicked application takes no command-line flags, so everything the CLI
// accepts has to be reachable another way or the desktop launch stays second-class.
// Precedence is explicit flag, then stored setting, then default — an argument the
// user typed just now should always win over one they saved last week.
//
// The API key is read from the environment first, because that is the documented way
// and keeps the secret out of a file for anyone who prefers that.

#ifndef DST_DESK_APP_SETTINGS_HPP
#define DST_DESK_APP_SETTINGS_HPP

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <cstdint>
#include "Core/Protocol.hpp"

namespace DST { namespace DESK { namespace App {

struct Settings
{
  std::uint16_t port = Core::kDefaultPort;
  QString       token;
  QString       outputDir;
  QString       apiKey;
  QString       model   = QStringLiteral("nova-3");
  bool          diarize = false;

  /// Where recordings go when nothing else says otherwise.
  ///
  /// Not the working directory. A launch from Finder or Explorer has no meaningful one —
  /// macOS starts an application in "/" — so "out" relative to it resolved to "/out",
  /// which cannot be created. The self-test then failed and the application refused to
  /// start, with the explanation going to a console that a double-clicked application
  /// does not have: indistinguishable from an immediate crash.
  static QString defaultOutputDir()
  {
    const auto documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const auto base = documents.isEmpty() ? QDir::homePath() : documents;
    return QDir(base).filePath(QStringLiteral("Kobayashi"));
  }

  /// The model used when nothing else says otherwise, named once so the loader, the
  /// dialog and the command line cannot disagree about it.
  static QString defaultModel() { return QStringLiteral("nova-3"); }

  /// Reads a setting that has no meaningful empty value.
  ///
  /// QSettings returns a stored empty string in preference to the default, so a key
  /// that is present but blank silently overrides it. For a model name that is fatal
  /// and almost invisible: an empty model reaches the engine as `model=`, and Deepgram
  /// answers 403 with a message about project permissions rather than about the model
  /// being missing, which sends the reader looking at their account.
  static QString nonEmpty(QSettings& stored, const QString& key, const QString& fallback)
  {
    const auto value = stored.value(key, fallback).toString().trimmed();
    return value.isEmpty() ? fallback : value;
  }

  // HP:TODO: the API key is stored in plain text. The file is owner-only, which stops
  // other local users reading it, and nothing stops a process running as this user. A
  // real fix means the platform keychain — Credential Manager, Keychain, libsecret —
  // which is three implementations and a fallback, and out of scope here.

  /// Where the stored settings live, so the UI can say so rather than being mysterious.
  static QString path()
  {
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir).filePath(QStringLiteral("kobayashi.ini"));
  }

  /// True when the key came from the environment, which the dialog must not overwrite
  /// with a stale stored value.
  static bool keyFromEnvironment() { return !environmentKey().isEmpty(); }

  static Settings load()
  {
    auto stored = QSettings(path(), QSettings::IniFormat);
    auto out = Settings{};

    out.port = static_cast<std::uint16_t>(
        stored.value(QStringLiteral("server/port"), int(Core::kDefaultPort)).toInt());
    out.token     = stored.value(QStringLiteral("server/token")).toString();
    out.outputDir = nonEmpty(stored, QStringLiteral("server/outputDir"), defaultOutputDir());
    out.model   = nonEmpty(stored, QStringLiteral("deepgram/model"), defaultModel());
    out.diarize = stored.value(QStringLiteral("deepgram/diarize"), false).toBool();

    // The environment wins: someone who exported the key this session means it.
    const auto fromEnv = environmentKey();
    out.apiKey = fromEnv.isEmpty()
                     ? stored.value(QStringLiteral("deepgram/apiKey")).toString().trimmed()
                     : fromEnv;

    return out;
  }

  void save() const
  {
    QDir().mkpath(QFileInfo(path()).absolutePath());

    auto stored = QSettings(path(), QSettings::IniFormat);
    stored.setValue(QStringLiteral("server/port"), int(port));
    stored.setValue(QStringLiteral("server/token"), token);
    stored.setValue(QStringLiteral("server/outputDir"), outputDir);
    stored.setValue(QStringLiteral("deepgram/model"), model);
    stored.setValue(QStringLiteral("deepgram/diarize"), diarize);

    // Never write a key that came from the environment into a file the user did not
    // choose to put it in.
    if (!keyFromEnvironment()) stored.setValue(QStringLiteral("deepgram/apiKey"), apiKey);

    stored.sync();

    // This file holds an API key, and QSettings creates it at whatever the process
    // umask allows — commonly world-readable. Narrowed after sync(), because the file
    // does not exist until then. Windows ignores POSIX permission bits; its own
    // per-user profile is what protects the file there.
    QFile::setPermissions(path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }

private:
  static QString environmentKey()
  {
    return QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("DEEPGRAM_API_KEY"))
        .trimmed();
  }
};

} } } // namespace DST::DESK::App

#endif // DST_DESK_APP_SETTINGS_HPP
