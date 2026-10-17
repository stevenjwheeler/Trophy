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
#include <QSaveFile>
#include <QRegularExpression>

#include "detection/ShippedLibDirs.h"
#include "detection/PersistentReporterLib.h"

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
  QStringList dirs{appDir + "/gl_shim"};
  dirs += libSubDirs(prefixFromBinaryDir(appDir));
  return findShippedLib(kLibName, dirs);
}

QString GlShimEnrollment::confFilePath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         "/environment.d/" + kConfName;
}

bool GlShimEnrollment::isEnrolled() {
  const QString lib = libraryPath();
  QFile f(confFilePath());
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  const QString content = QString::fromUtf8(f.readAll());
  for (const QString &line : content.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith("LD_PRELOAD="))
      continue;
    for (const QString &entry : trimmed.mid(11).split(':')) {
      if ((!lib.isEmpty() && entry == lib) ||
          PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim"))
        return true;
    }
  }
  return false;
}

bool GlShimEnrollment::isActiveInCurrentSession() {
  const QString preload = QString::fromLocal8Bit(qgetenv("LD_PRELOAD"));
  return !preload.isEmpty() && preload.contains("trophy_gl_shim");
}

bool GlShimEnrollment::refreshEnrolledBundle() {
  const QString source = libraryPath();
  if (!PersistentReporterLib::isBundled(source))
    return true;
  QFile file(confFilePath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return true; // No readable prior consent; never enroll by default.
  const QString content = QString::fromUtf8(file.readAll());
  file.close();
  for (const QString &line : content.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith("LD_PRELOAD="))
      continue;
    for (const QString &entry : trimmed.mid(11).split(':')) {
      // Legacy AppImages left a now-missing temporary path. Its explicit
      // entry in our dedicated enrollment file still records opt-in.
      if (entry == source || QFileInfo(entry).fileName() == kLibName ||
          PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim"))
        return setEnrolled(true);
    }
  }
  return true;
}

bool GlShimEnrollment::setEnrolled(bool enrolled) {
  const QString path = confFilePath();
  if (!enrolled) {
    if (!QFile::exists(path))
      return true;
    QFile existing(path);
    if (!existing.open(QIODevice::ReadOnly | QIODevice::Text))
      return false;
    const QString source = libraryPath();
    QStringList kept;
    for (const QString &line : QString::fromUtf8(existing.readAll()).split('\n')) {
      const QString trimmed = line.trimmed();
      if (trimmed.startsWith("LD_PRELOAD=")) {
        QStringList foreign;
        for (const QString &entry : trimmed.mid(11).split(
                 QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
          if (entry != source && QFileInfo(entry).fileName() != kLibName &&
              !PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim"))
            foreign.append(entry);
        }
        if (!foreign.isEmpty())
          kept.append("LD_PRELOAD=" + foreign.join(':'));
      } else if (!trimmed.isEmpty() && !trimmed.startsWith('#')) {
        kept.append(line);
      }
    }
    existing.close();
    if (kept.isEmpty())
      return QFile::remove(path);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text))
      return false;
    const QByteArray content = (kept.join('\n') + '\n').toUtf8();
    return output.write(content) == content.size() && output.commit();
  }

  const QString source = libraryPath();
  const QString lib =
      source.isEmpty()
          ? QString()
          : PersistentReporterLib::install(source, "libtrophy_gl_shim");
  if (lib.isEmpty())
    return false;
  // LD_PRELOAD tokenizes spaces/colons regardless of shell quoting, and
  // environment.d expands variables. Fail closed rather than emitting a
  // path that could load different libraries from the requested one.
  if (lib.contains(QRegularExpression("[\\s:$\\\"'\\\\]")))
    return false;

  // Merge with any foreign LD_PRELOAD already present in our file.
  QString other;
  QString existingContent;
  QFile existing(path);
  if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
    existingContent = QString::fromUtf8(existing.readAll());
    existing.close();
    for (const QString &line : existingContent.split('\n')) {
      const QString t = line.trimmed();
      if (t.startsWith("LD_PRELOAD=")) {
        const QString val = t.mid(QString("LD_PRELOAD=").size());
        for (const QString &entry : val.split(':')) {
          if (!entry.isEmpty() && !entry.contains("trophy_gl_shim") &&
              entry != lib && entry != source)
            other += (other.isEmpty() ? "" : ":") + entry;
        }
      }
    }
  }

  QString content = "# Managed by Trophy: preload the OpenGL game-detection shim.\n"
                    "# Takes effect on next login. Remove this file to opt out.\n"
                    "LD_PRELOAD=" + lib;
  if (!other.isEmpty())
    content += ':' + other;
  content += '\n';
  if (existingContent == content)
    return true; // Startup refresh must not rewrite an unchanged enrollment.

  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  const QByteArray bytes = content.toUtf8();
  return f.write(bytes) == bytes.size() && f.commit();
}

} // namespace TrophyBackend
