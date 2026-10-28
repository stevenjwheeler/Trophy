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
#include "detection/MultilibGlPreload.h"
#include "detection/DetectorVersion.h"

namespace TrophyBackend {

namespace {
constexpr const char *kLibName = "libtrophy_gl_shim.so";
constexpr const char *kConfName = "trophy-gl-shim.conf";

QString activePreload(const QString &content, int *count = nullptr) {
  QString value;
  int assignments = 0;
  for (const auto &line : content.split('\n')) {
    const auto trimmed = line.trimmed();
    if (trimmed.startsWith("LD_PRELOAD=")) {
      value = trimmed.mid(11);
      ++assignments;
    }
  }
  if (count) *count = assignments;
  return value;
}
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

QString GlShimEnrollment::libraryPath32() {
  const QString override = QString::fromLocal8Bit(qgetenv("TROPHY_GL_SHIM_LIB32"));
  return override.isEmpty() ? ReporterArchitecture::companion32(libraryPath()) : override;
}

bool GlShimEnrollment::isEnrolled() {
  const QString lib = libraryPath();
  QFile f(confFilePath());
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  const QString content = QString::fromUtf8(f.readAll());
  for (const QString &entry : activePreload(content).split(QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
    if ((!lib.isEmpty() && entry == lib) ||
        PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim") ||
        MultilibGlPreload::isOwned(entry))
      return true;
  }
  return false;
}

bool GlShimEnrollment::isActiveInCurrentSession() {
  const QString preload = QString::fromLocal8Bit(qgetenv("LD_PRELOAD"));
  return !preload.isEmpty() && preload.contains("trophy_gl_shim");
}

int GlShimEnrollment::bundledHelperVersion() {
  return DetectorVersion::reporter(libraryPath(), "gl");
}

int GlShimEnrollment::helperVersion() {
  QFile file(confFilePath());
  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    const QString preload = activePreload(QString::fromUtf8(file.readAll()));
    for (const auto &entry : preload.split(QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
      const QString pair = MultilibGlPreload::tokenDirectory(entry);
      if (!pair.isEmpty())
        return DetectorVersion::reporter(pair + "/x86_64/libtrophy_gl_shim.so", "gl");
      if (entry == libraryPath() || QFileInfo(entry).fileName() == kLibName ||
          PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim"))
        return DetectorVersion::reporter(entry, "gl");
    }
  }
  return DetectorVersion::reporter(libraryPath(), "gl");
}

bool GlShimEnrollment::refreshEnrolledBundle() {
  const QString source = libraryPath();
  QFile file(confFilePath());
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return true; // No readable prior consent; never enroll by default.
  const QString content = QString::fromUtf8(file.readAll());
  file.close();
  int count = 0;
  const QString preload = activePreload(content, &count);
  if (count > 1) return false; // Don't reinterpret foreign expansion/order semantics.
  for (const QString &entry : preload.split(QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
    if (!MultilibGlPreload::tokenDirectory(entry).isEmpty()) {
      // Refresh only a previously opted-in architecture pair. A package
      // without a companion retains its validated durable i386 helper while
      // still allowing native helper updates. Never silently downgrade mode.
      if (source.isEmpty()) return true;
      const QString companion = libraryPath32();
      if (!companion.isEmpty()) return setEnrolledImpl(true, true);
      if (!MultilibGlPreload::isOwned(entry)) return false;
      return setEnrolledImpl(true, true,
          MultilibGlPreload::tokenDirectory(entry) + "/i686/libtrophy_gl_shim.so");
    }
    // Legacy AppImages left a now-missing temporary path. Its explicit
    // entry in our dedicated enrollment file still records opt-in.
    if (!entry.contains('$') && PersistentReporterLib::isBundled(source) &&
        (entry == source || QFileInfo(entry).fileName() == kLibName ||
         PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim")))
      return setEnrolledImpl(true, false); // Never silently migrate legacy consent.
  }
  return true;
}

bool GlShimEnrollment::setEnrolled(bool enrolled) {
  return setEnrolledImpl(enrolled, true);
}

bool GlShimEnrollment::setEnrolledImpl(bool enrolled, bool allowMultilib, const QString &retained32) {
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
  const QString companion = retained32.isEmpty() ? libraryPath32() : retained32;
  const QString lib =
      allowMultilib && !companion.isEmpty()
          ? MultilibGlPreload::install(source, companion)
          : source.isEmpty()
          ? QString()
          : PersistentReporterLib::install(source, "libtrophy_gl_shim");
  if (lib.isEmpty())
    return false;
  // LD_PRELOAD tokenizes spaces/colons regardless of shell quoting, and
  // environment.d expands variables. Fail closed rather than emitting a
  // path that could load different libraries from the requested one.
  if (lib.contains(QRegularExpression("[\\s:$\\\"'\\\\]")))
    if (!MultilibGlPreload::isOwned(lib))
      return false; // Only our validated, escaped $$PLATFORM token is exempt.

  // Merge with any foreign LD_PRELOAD already present in our file.
  QString other;
  QString existingContent;
  QStringList assignments;
  int preloadInsertion = -1;
  QFile existing(path);
  if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
    existingContent = QString::fromUtf8(existing.readAll());
    existing.close();
    int count = 0;
    activePreload(existingContent, &count);
    if (count > 1) return false;
    for (const QString &line : existingContent.split('\n')) {
      const QString t = line.trimmed();
      if (t.startsWith("LD_PRELOAD=")) {
        preloadInsertion = assignments.size();
        const QString val = t.mid(QString("LD_PRELOAD=").size());
        for (const QString &entry : val.split(QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
          if (QFileInfo(entry).fileName() != kLibName &&
              !PersistentReporterLib::isOwned(entry, "libtrophy_gl_shim") &&
              MultilibGlPreload::tokenDirectory(entry).isEmpty() &&
              entry != lib && entry != source)
            other += (other.isEmpty() ? "" : ":") + entry;
        }
      } else if (!t.isEmpty() && !t.startsWith('#')) {
        assignments.append(line);
      }
    }
  }

  QString preload = "LD_PRELOAD=" + lib;
  if (!other.isEmpty())
    preload += ':' + other;
  assignments.insert(preloadInsertion < 0 ? assignments.size() : preloadInsertion, preload);
  const QString content = "# Managed by Trophy: preload the OpenGL game-detection shim.\n"
                          "# Takes effect on next login. Remove this file to opt out.\n" +
                          assignments.join('\n') + '\n';
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
