/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AutostartManager.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace TrophyBackend {

namespace {
// Reverse-DNS id: GNOME derives the portal app id from the autostart unit
// name (app-<id>@autostart.service), and a bare "trophy" is rejected by
// portal backends such as GNOME's GlobalShortcuts provider.
const char *kDesktopFileName = "io.github.stevenjwheeler.Trophy.desktop";
// Entry written by older versions; migrated/removed on the next sync.
const char *kLegacyDesktopFileName = "trophy.desktop";
const char *kMarker = "# Managed by Trophy - safe to delete to disable autostart";

// Desktop Entry Exec quoting: a path with spaces (or other
// shell-special characters) is wrapped in double quotes, escaping
// backslash/double-quote/backtick/dollar. Single quotes are literal
// under the Desktop Entry spec, so they don't quote. Plain paths need
// only literal-percent escaping.
QString quoteExecPath(const QString &path) {
  bool needsQuote = false;
  for (const QChar c : path) {
    if (c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\\') ||
        c == QLatin1Char('\'') || c == QLatin1Char('$') ||
        c == QLatin1Char('`') || c == QLatin1Char(';') ||
        c == QLatin1Char('&') || c == QLatin1Char('|') ||
        c == QLatin1Char('<') || c == QLatin1Char('>') ||
        c == QLatin1Char('(') || c == QLatin1Char(')') ||
        c == QLatin1Char('#') || c == QLatin1Char('!') ||
        c == QLatin1Char('*') || c == QLatin1Char('?') ||
        c == QLatin1Char('~')) {
      needsQuote = true;
      break;
    }
  }
  QString quoted = path;
  // A literal percent must not be interpreted as an Exec field code.
  quoted.replace(QStringLiteral("%"), QStringLiteral("%%"));
  if (!needsQuote) {
    return quoted;
  }
  quoted.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
  quoted.replace(QStringLiteral("\""), QStringLiteral("\\\""));
  quoted.replace(QStringLiteral("`"), QStringLiteral("\\`"));
  quoted.replace(QStringLiteral("$"), QStringLiteral("\\$"));
  // Desktop Entry value escaping is decoded before Exec argument quoting.
  quoted.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
  return QStringLiteral("\"") + quoted + QStringLiteral("\"");
}
} // namespace

AutostartManager::AutostartManager(QObject *parent)
    : AutostartManager(QString(), parent) {}

AutostartManager::AutostartManager(const QString &configHomeOverride,
                                   QObject *parent)
    : QObject(parent), m_configHomeOverride(configHomeOverride) {}

QString AutostartManager::autostartDir() const {
  const QString base = m_configHomeOverride.isEmpty()
                           ? QStandardPaths::writableLocation(
                                 QStandardPaths::ConfigLocation)
                           : m_configHomeOverride;
  return base + "/autostart";
}

QString AutostartManager::filePath() const {
  return autostartDir() + "/" + QString::fromLatin1(kDesktopFileName);
}

QString AutostartManager::legacyFilePath() const {
  return autostartDir() + "/" + QString::fromLatin1(kLegacyDesktopFileName);
}

static bool entryIsOurs(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  return f.readAll().contains(QByteArray(kMarker));
}

bool AutostartManager::isEnabled() const {
  // Treat the pre-migration entry as enabled too: it is ours, and the next
  // enable/disable sync moves or removes it.
  return entryIsOurs(filePath()) || entryIsOurs(legacyFilePath());
}

QString AutostartManager::launchCommand(const QString &execPath,
                                       const QString &appImagePath) {
  return quoteExecPath(appImagePath.isEmpty() ? execPath : appImagePath) +
         QStringLiteral(" --minimized");
}

bool AutostartManager::setEnabled(bool enabled, const QString &execPath,
                                 const QString &appImagePath) {
  const QString path = filePath();
  const QString legacy = legacyFilePath();
  const bool legacyOurs = entryIsOurs(legacy);
  if (!enabled) {
    // Only remove entries we own; leave foreign ones alone.
    const bool hadOurs = entryIsOurs(path);
    if (!hadOurs && !legacyOurs) {
      return true; // nothing of ours to remove
    }
    if (hadOurs && !QFile::remove(path))
      return false;
    if (legacyOurs && !QFile::remove(legacy))
      return false;
    return true;
  }

  // Enabling must not replace a user-maintained entry either.
  if (QFile::exists(path) && !entryIsOurs(path)) {
    return false;
  }

  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return false;
  }
  QTextStream out(&f);
  out << QString::fromLatin1(kMarker) << "\n"
      << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Version=1.0\n"
      << "Name=Trophy\n"
      << "Comment=Game recording and clipping\n"
      << "Exec=" << launchCommand(execPath, appImagePath) << "\n"
      << "Icon=trophy\n"
      << "Terminal=false\n"
      << "Categories=AudioVideo;Game;\n"
      << "X-GNOME-Autostart-enabled=true\n";
  out.flush();
  f.close();
  // The old bare-id entry would keep its own (invalid) autostart unit name,
  // so remove it once the new entry is written.
  if (legacyOurs)
    QFile::remove(legacy);
  return true;
}

} // namespace TrophyBackend
