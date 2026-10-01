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
const char *kDesktopFileName = "trophy.desktop";
const char *kMarker = "# Managed by Trophy - safe to delete to disable autostart";
} // namespace

AutostartManager::AutostartManager(QObject *parent)
    : AutostartManager(QString(), parent) {}

AutostartManager::AutostartManager(const QString &configHomeOverride,
                                   QObject *parent)
    : QObject(parent), m_configHomeOverride(configHomeOverride) {}

QString AutostartManager::filePath() const {
  const QString base = m_configHomeOverride.isEmpty()
                           ? QStandardPaths::writableLocation(
                                 QStandardPaths::ConfigLocation)
                           : m_configHomeOverride;
  return base + "/autostart/" + QString::fromLatin1(kDesktopFileName);
}

bool AutostartManager::isEnabled() const {
  QFile f(filePath());
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }
  QTextStream in(&f);
  return in.readAll().contains(QString::fromLatin1(kMarker));
}

bool AutostartManager::setEnabled(bool enabled, const QString &execPath) {
  const QString path = filePath();
  if (!enabled) {
    // Only remove entries we own; leave foreign ones alone.
    if (!isEnabled()) {
      return true; // nothing of ours to remove
    }
    return QFile::remove(path);
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
      << "Comment=Trophy game recording and clipping\n"
      << "Exec=" << execPath << " --minimized\n"
      << "Icon=trophy\n"
      << "Terminal=false\n"
      << "Categories=AudioVideo;Recorder;\n"
      << "X-GNOME-Autostart-enabled=true\n";
  return true;
}

} // namespace TrophyBackend
