/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GlShimEnrollment.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include "detection/ShippedLibDirs.h"

namespace TrophyBackend {

namespace {
constexpr const char *kLibName = "libtrophy_gl_shim.so";
constexpr const char *kConfName = "trophy-gl-shim.conf";
} // namespace

QString GlShimEnrollment::libraryPath() {
  // Explicit override first (tests, custom prefixes).
  const QString env = QString::fromLocal8Bit(qgetenv("TROPHY_GL_SHIM_LIB"));
  if (!env.isEmpty() && QFile::exists(env))
    return env;

  // CMake build-tree copy next to the binaries (dev runs) first, then the
  // installed locations — derived from our own prefix so ~/.local and other
  // custom prefixes resolve instead of falling off a hardcoded /usr list.
  return libraryPathForAppDir(QCoreApplication::applicationDirPath());
}

QString GlShimEnrollment::libraryPathForAppDir(const QString &appDir) {
  return findShippedLib(kLibName, {appDir + "/gl_shim"});
}

QString GlShimEnrollment::confFilePath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         "/environment.d/" + kConfName;
}

bool GlShimEnrollment::isEnrolled() {
  const QString lib = libraryPath();
  if (lib.isEmpty())
    return false;
  QFile f(confFilePath());
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  const QString content = QString::fromUtf8(f.readAll());
  return content.contains(lib);
}

bool GlShimEnrollment::isActiveInCurrentSession() {
  const QString preload = QString::fromLocal8Bit(qgetenv("LD_PRELOAD"));
  return !preload.isEmpty() && preload.contains("trophy_gl_shim");
}

bool GlShimEnrollment::setEnrolled(bool enrolled) {
  const QString path = confFilePath();
  if (!enrolled) {
    // Opt-out: drop our conf file entirely (it only ever holds LD_PRELOAD).
    if (!QFile::exists(path))
      return true;
    return QFile::remove(path);
  }

  const QString lib = libraryPath();
  if (lib.isEmpty())
    return false;

  // Merge with any foreign LD_PRELOAD already present in our file.
  QString other;
  QFile existing(path);
  if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
    const QString content = QString::fromUtf8(existing.readAll());
    existing.close();
    for (const QString &line : content.split('\n')) {
      const QString t = line.trimmed();
      if (t.startsWith("LD_PRELOAD=")) {
        const QString val = t.mid(QString("LD_PRELOAD=").size());
        for (const QString &entry : val.split(':')) {
          if (!entry.isEmpty() && !entry.contains("trophy_gl_shim") &&
              entry != lib)
            other += (other.isEmpty() ? "" : ":") + entry;
        }
      }
    }
  }

  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    return false;
  QTextStream out(&f);
  out << "# Managed by Trophy: preload the OpenGL game-detection shim.\n";
  out << "# Takes effect on next login. Remove this file to opt out.\n";
  out << "LD_PRELOAD=" << lib;
  if (!other.isEmpty())
    out << ":" << other;
  out << "\n";
  return true;
}

} // namespace TrophyBackend
