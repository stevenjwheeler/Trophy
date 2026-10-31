/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GameIdentity.h"

#include "GameWhitelist.h"
#include "PrerequisiteInstallers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cstring>
#include <unistd.h>

namespace TrophyBackend {

namespace {

QString basenameOf(QString path) {
  path.replace('\\', '/');
  return path.section('/', -1);
}

} // namespace

bool GameIdentity::isWineLoader(const QString &name) {
  return QStringList{"wine", "wine64", "wine-preloader", "wine64-preloader",
                     "wineserver", "proton"}.contains(name.toLower());
}

bool GameIdentity::isHelperName(const QString &name) {
  const QString key = GameWhitelist::normalizeKey(name);
  return prerequisiteInstallerNames().contains(key) ||
         QStringList{"steam", "steamwebhelper", "steam-runtime-launch-client",
                     "steam-runtime-launcher-service", "pressure-vessel-wrap",
                     "d3ddriverquery", "d3ddriverquery64", "steam gpu query",
                     "wineopenxr test instance", "hellovr_vulkan", "xalia",
                     "wineboot", "winecfg", "winedevice", "services", "explorer",
                     "rundll32", "crashpad_handler", "unitycrashhandler64",
                     "unitycrashhandler32", "crashreportclient"}.contains(key);
}

GameIdentity::ReporterIdentity GameIdentity::resolve(const QString &reporter,
                                       const QString &exe,
                                       const QStringList &argv,
                                       uint steamAppId,
                                       const QString &winePrefix) {
  ReporterIdentity result;
  result.name = reporter.trimmed();
  QString executablePath = exe;
  QString executable = basenameOf(exe);
  // Wine may rewrite argv[0] to the Windows executable, or keep the Wine
  // loader followed by it. Never search arbitrary arguments: an installer
  // or launcher can mention another executable without running it.
  if (isWineLoader(executable)) {
    executable.clear();
    executablePath.clear();
    if (!argv.isEmpty()) {
      QString candidate = argv.first();
      if (isWineLoader(basenameOf(candidate)) && argv.size() > 1)
        candidate = argv.at(1);
      if (candidate.endsWith(".exe", Qt::CaseInsensitive)) {
        executable = basenameOf(candidate);
        candidate.replace('\\', '/');
        executablePath = QStringLiteral("wine:") + winePrefix + ":" + candidate;
      }
    }
  }
  result.executable = executable;
  result.ignored = isHelperName(basenameOf(reporter)) ||
                   isHelperName(executable) ||
                   (isWineLoader(basenameOf(exe)) && executable.isEmpty());
  if (result.ignored)
    return result;
  const bool generic = GameWhitelist::isPlaceholderReporterKey(reporter) ||
                       QStringList{"unity", "unreal engine", "godot", "sdl_app"}
                           .contains(reporter.trimmed().toLower());
  if (steamAppId > 0)
    result.key = QStringLiteral("steam_app_%1").arg(steamAppId);
  else if (!executable.isEmpty())
    result.key = GameWhitelist::normalizeKey(
        executablePath.startsWith("wine:")
            ? executablePath
            : "exe:" + executablePath);
  else if (!generic)
    result.key = GameWhitelist::normalizeKey(reporter);
  if (generic && !executable.isEmpty())
    result.name = executable;
  // An unresolved null-name event has no identity. Even Steam evidence
  // must not turn the shared placeholder into a display-name/token key.
  if (generic && executable.isEmpty())
    result.name.clear();
  return result;
}

GameIdentity::ReporterIdentity GameIdentity::forPid(qint64 pid,
                                                    const QString &reporter,
                                                    uint steamAppId) {
  const QString exe =
      QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget();
  QFile command(QString("/proc/%1/cmdline").arg(pid));
  QStringList argv;
  if (command.open(QIODevice::ReadOnly)) {
    const QByteArray bytes = command.read(65537);
    if (bytes.size() <= 65536) {
      for (const QByteArray &arg : bytes.split('\0'))
        argv.append(QString::fromLocal8Bit(arg));
    }
  }
  const QString loader = QFileInfo(exe).fileName().toLower();
  if ((loader == "wine" || loader == "wine64" ||
       loader.endsWith("-preloader")) &&
      !argv.isEmpty()) {
    int index = 0;
    if ((argv.first() == "wine" || argv.first() == "wine64" ||
         QFileInfo(argv.first()).fileName().endsWith("-preloader")) &&
        argv.size() > 1)
      index = 1;
    QString candidate = argv.at(index);
    candidate.replace('\\', '/');
    static const QRegularExpression drive("^[A-Za-z]:");
    if (candidate.endsWith(".exe", Qt::CaseInsensitive) &&
        QDir::isRelativePath(candidate) && !drive.match(candidate).hasMatch()) {
      const QString cwd =
          QFileInfo(QString("/proc/%1/cwd").arg(pid)).symLinkTarget();
      if (!cwd.isEmpty())
        argv[index] = QDir::cleanPath(cwd + "/" + candidate);
    }
  }
  QString winePrefix;
  QFile environment(QString("/proc/%1/environ").arg(pid));
  if (environment.open(QIODevice::ReadOnly)) {
    const QByteArray bytes = environment.read(65537);
    if (bytes.size() <= 65536) {
      for (const QByteArray &entry : bytes.split('\0')) {
        if (entry.startsWith("WINEPREFIX="))
          winePrefix = QString::fromLocal8Bit(entry.mid(11));
      }
    }
  }
  if (winePrefix.isEmpty())
    winePrefix = QDir::homePath() + "/.wine";
  return resolve(reporter, exe, argv, steamAppId, winePrefix);
}

uint GameIdentity::steamAppIdForPid(qint64 pid) {
  if (pid <= 0)
    return 0;
  QFile env(QString("/proc/%1/environ").arg(pid));
  if (!env.open(QIODevice::ReadOnly))
    return 0;
  // Environ is NUL-separated KEY=VALUE records.
  for (const QByteArray &rec : env.readAll().split('\0')) {
    if (rec.startsWith("SteamAppId=")) {
      bool ok = false;
      const uint id = rec.mid(sizeof("SteamAppId=") - 1).toUInt(&ok);
      if (ok && id > 0)
        return id;
    }
  }
  return 0;
}

uint GameIdentity::steamAppIdFromAppId(const QString &appId) {
  static const QRegularExpression re("^steam_app_(\\d+)$",
                                     QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch m = re.match(appId.trimmed());
  if (!m.hasMatch())
    return 0;
  return m.captured(1).toUInt();
}

bool GameIdentity::looksLikeSteamGame(qint64 pid) {
  if (pid <= 0)
    return false;
  if (steamAppIdForPid(pid) > 0)
    return true;
  // Targeted single-PID lookup per reporter/window event (not a scan).
  char exe[512];
  const QString exeLink = QString("/proc/%1/exe").arg(pid);
  const ssize_t n =
      ::readlink(exeLink.toLocal8Bit().constData(), exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    if (strstr(exe, "steamapps"))
      return true;
  }
  QFile cmd(QString("/proc/%1/cmdline").arg(pid));
  if (!cmd.open(QIODevice::ReadOnly))
    return false;
  return cmd.readAll().contains("steamapps");
}

} // namespace TrophyBackend
